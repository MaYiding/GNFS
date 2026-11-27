# GNFS ENV 调优开关文档

本目录收录所有 `GNFS_*` 运行时调优开关 / helper 的**详细设计文档**(算法、ENV 解析规则、bit-for-bit 保证、ROI 与定位、集成点、测试清单),按代码模块组织。

顶层一句话速查表见 [CLAUDE.md → ENV 调优开关总览](../../CLAUDE.md#env-调优开关总览)。新增开关的文档化规范见 [CLAUDE.md → 设计细节文档化规范](../../CLAUDE.md#设计细节文档化规范强制)。

## 模块导航

| 模块 | 开关数 | 文档 | 主题 |
|------|-------:|------|------|
| relation | 13 | [relation.md](relation.md) | structured filter、filter merge (V0/V3)、OOC、LP key dedup、radix sort |
| linalg | 15 | [linalg.md](linalg.md) | BW Krylov、SpMV/GF(2) SIMD、SGE、转置、进度遥测 |
| cofactor | 11 | [cofactor.md](cofactor.md) | batch trial、ECM stage 并行/cache、survival predictor、Brent rho |
| polynomial | 9 | [polynomial.md](polynomial.md) | Karatsuba / NTT / HGCD / squaring / mod-p SIMD |
| sieve | 16 | [sieve.md](sieve.md) | checkpoint、cache tile、prefetch、raw target multiplier、distributed route/watchdog、lattice SIMD、E-core QoS |
| siqs | 1 | [siqs.md](siqs.md) | default-off shadow proof observe / explicit prefer |
| util | 6 | [util.md](util.md) | Integer scratch pool、批量 GMP `mpz_*` 并行 |
| sqrt | 2 | [sqrt.md](sqrt.md) | Hensel lift 并行、Couveignes pattern search 并行 |
| factor_base | 1 | [factor_base.md](factor_base.md) | Cantor-Zassenhaus 求根并行 |
| workbench | 1 | [workbench.md](workbench.md) | macOS workbench 的引擎路径覆盖与环境隔离 |

## 通用设计约定

绝大多数开关共享以下契约,各模块文档不再重复:

- **ENV 解析**:性能 helper 通常 cached(`std::once_flag` + `std::atomic`),每进程一次 `getenv`;正确性策略可采用无缓存纯 parser。非法值通常 fallback 到 default,但模块文档可以定义更严格的 fail-closed 契约。
- **三态 SIMD gate**(`auto|0|1`):`auto` = 平台/尺寸可用则启用,`0` = 强制 scalar(回归 bisect / sanitizer 用),`1` = 强制 SIMD(无 SIMD 平台仍 fallback scalar)。
- **并行 dispatcher**(整数 `N`,default 1):`N=1` 走 sequential,不创建 ThreadPool,零开销;`N>=2` dispatch 到大小 `min(N, batch)` 的 ThreadPool。
- **cache / pool**(整数 `N`,default 0):`N=0` disabled,零开销;`N>=1` 启用,容量 `N`。
- **bit-for-bit guarantee**:性能开关的开 / 关产生逐位一致的输出(仅性能不同),由单元测试强制。会改变算法策略的开关单独记录禁用路径与依赖空间契约。
- **helper-only future-infra**:许多 helper 当前主 pipeline **未 wire-in**,是预留基础设施;ENV 仅在调用方显式 wire-in 后才生效。各文档「集成点」「Default」小节会注明当前是否 wire-in。

## Parallel Dispatcher Family

W7–W15 期间落地的一组 opt-in 并行 dispatcher,共享同一 ENV-gate + ThreadPool 模板(`<env_reader>()` cached + `parallel_<x>(...)` 模板 + `resolve_<x>(batch)` helper + `*_reset_env_cache_for_testing()` test hook)。全部 **default 1(sequential)**,互相正交,可同时启用。

| # | ENV | 模块 | 并行对象 |
|--:|-----|------|----------|
| 1 | `GNFS_SQRT_HENSEL_THREADS` | sqrt | Hensel lift K-prime slot |
| 2 | `GNFS_ECM_STAGE2_PARALLEL` | cofactor | ECM Stage 2 BSGS 多曲线 |
| 3 | `GNFS_ECM_STAGE1_PARALLEL_THREADS` | cofactor | ECM Stage 1 Lucas-chain 多曲线 |
| 4 | `GNFS_FILTER_MERGE_THREADS` | relation | LP-key bucket merge |
| 5 | `GNFS_MPZ_POWM_BATCH_THREADS` | util | batched `mpz_powm` |
| 6 | `GNFS_LATTICE_BASIS_PARALLEL_THREADS` | sieve | lattice basis reduction 多基 |
| 7 | `GNFS_MPZ_INVERT_BATCH_THREADS` | util | batched `mpz_invert` |
| 8 | `GNFS_SIEVE_APPLY_TILE_THREADS` | sieve | apply-tile work distribution |
| 9 | `GNFS_MPZ_MOD_BATCH_THREADS` | util | batched `mpz_mod` |
| 10 | `GNFS_MPZ_GCD_BATCH_THREADS` | util | batched `mpz_gcd` |
| 11 | `GNFS_BRENT_POLLARD_RHO_THREADS` | cofactor | Brent rho 多配置 |
| 12 | `GNFS_MPZ_MUL_BATCH_THREADS` | util | batched `mpz_mul` |

相关但不在严格编号内的并行开关:`GNFS_MURPHY_ALPHA_THREADS`(polynomial)、`GNFS_SCHIROKAUER_THREADS`(linalg)、`GNFS_FB_ROOTS_THREADS`(factor_base)、`GNFS_COUVEIGNES_PARALLEL_THREADS`(sqrt)、`GNFS_BW_KRYLOV_STREAMS`(linalg)。

## SIMD Helper Family

一组纯 header、不依赖外部库的 SIMD 内核(NEON ARM64 / AVX2 x86_64),三态 `auto|0|1` gate,bit-for-bit 等同 scalar reference:

- **GF(2) word 级**(linalg):`GNFS_GF2_POPCNT_SIMD`、`GNFS_GF2_AND_POPCNT_SIMD`、`GNFS_GF2_XOR_POPCNT_SIMD`、`GNFS_GF2_ROW_POPCOUNT_SIMD`、`GNFS_GF2_ROW_XOR_SIMD`、`GNFS_GF2_AND_WORDS_SIMD`、`GNFS_SPMV_SIMD`
- **mod-p 多项式**(polynomial):`GNFS_POLY_ADD_MOD_SIMD`、`GNFS_POLY_HORNER_MOD_SIMD`、`GNFS_POLY_HORNER_BATCH_SIMD`
- **sieve**(sieve):`GNFS_LATTICE_COORDS_SIMD`、`GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD`、`GNFS_SIEVE_SATURATED_SUB_SIMD`
- **cofactor**(cofactor):`GNFS_TRIAL_DIV_SIMD`

## 新增开关

新增 `GNFS_*` 开关 / helper 时,**详细设计写到本目录对应模块文件**,并在 [CLAUDE.md ENV 调优开关总览](../../CLAUDE.md#env-调优开关总览)表加一行。完整规范见 [CLAUDE.md → 设计细节文档化规范](../../CLAUDE.md#设计细节文档化规范强制)。

---

## 补充台账速查表

下表是 2026-11-27 清点代码后补进模块页的开关：此前在 `include`、`src`、`apps` 的字符串
字面量里能找到它们，本目录却没有任何记载。解析方式、默认值、对结果的影响、接入点与
测试见链接指向的小节。

`scripts/ci/check_env_flag_docs.py` 在 Script Checks 里守着这份台账：代码里新出现的
`GNFS_*` 名字必须出现在本目录下，否则检查失败。确实不是运行时开关的名字写在该脚本的
豁免表里并附有理由，目前有三类：测试钩子、记录格式前缀，以及只在 workbench 测试源码里
出现的样例名字。最后一类限定了允许出现的目录，同一个名字出现在别的文件里就必须先写进
文档；豁免表里的名字一旦从允许的位置消失，检查同样失败。

| 开关 | 默认值 | 作用 | 模块页 |
|---|---|---|---|
| `GNFS_3LP` | 关闭 | cofactorizer 接受 3 个大素数的余因子，合并器放行 3LP partial | [relation.md](relation.md#3lp-relation-acceptance-gnfs_3lp) |
| `GNFS_ADAPTIVE_LATTICE` | 关闭 | 命中密度过低的 special-q 换格基重筛 | [sieve.md](sieve.md#adaptive-lattice-re-reduction-gnfs_adaptive_lattice) |
| `GNFS_ADAPTIVE_LATTICE_MAX_RETRIES` | `2` | 每个 special-q 最多重筛几次，范围 0 到 16 | [sieve.md](sieve.md#gnfs_adaptive_lattice_max_retries) |
| `GNFS_ADAPTIVE_LATTICE_SEED` | `0` | 重筛斜切方向的种子，非零时与 special-q 混合 | [sieve.md](sieve.md#gnfs_adaptive_lattice_seed) |
| `GNFS_ADAPTIVE_LATTICE_THRESHOLD` | `0.5` | 触发重筛的命中密度阈值，范围 $(0, 100]$ | [sieve.md](sieve.md#gnfs_adaptive_lattice_threshold) |
| `GNFS_BW_ALGORITHM` | block 路径 | `scalar` 强制内存路线的 Block Wiedemann 走 scalar Berlekamp-Massey（调试） | [linalg.md](linalg.md#block-wiedemann-algorithm-selection-gnfs_bw_algorithm) |
| `GNFS_CLI_PATH` | 未设置 | workbench 优先用它指定的可执行文件当引擎，其次是内置的 `gnfs` | [workbench.md](workbench.md#engine-executable-override-gnfs_cli_path) |
| `GNFS_COUVEIGNES_VERBOSE` | 未设置 | 向 stderr 打印一行 Couveignes 指标（诊断） | [sqrt.md](sqrt.md#couveignes-telemetry-gnfs_couveignes_verbose) |
| `GNFS_DEBUG_BL_CKPT` | 未设置 | 每次 Block Lanczos checkpoint 保存后打印一行（诊断） | [linalg.md](linalg.md#block-lanczos-checkpoint-trace-gnfs_debug_bl_ckpt) |
| `GNFS_DEBUG_GAUSSIAN` | 未设置 | Gaussian elimination 结束后打印一行统计（诊断） | [linalg.md](linalg.md#gaussian-elimination-trace-gnfs_debug_gaussian) |
| `GNFS_DISABLE_SIQS` | 未设置 | 跳过整个 SIQS 阶段，显式 `--method siqs` 也落入 GNFS（调试） | [siqs.md](siqs.md#method-routing-overrides-gnfs_force_siqs-gnfs_disable_siqs) |
| `GNFS_ECM_BRENT_SUYAMA` | 关闭 | ECM Stage 2 启用 Brent-Suyama 多项式扩展 | [cofactor.md](cofactor.md#ecm-brent-suyama-extension-gnfs_ecm_brent_suyama) |
| `GNFS_ECM_BS_DEGREE` | `12`（需启用扩展） | Brent-Suyama 次数，取 1、2、6、12 或 30 | [cofactor.md](cofactor.md#gnfs_ecm_bs_degree) |
| `GNFS_FORCE_COUVEIGNES` | 未设置 | 跳过 Hensel lifting，只走 Couveignes（测试） | [sqrt.md](sqrt.md#couveignes-only-square-root-gnfs_force_couveignes) |
| `GNFS_FORCE_SIQS` | 未设置 | 只影响路由：试除判定之后的输入选 SIQS，显式 `--method` 优先（调试） | [siqs.md](siqs.md#method-routing-overrides-gnfs_force_siqs-gnfs_disable_siqs) |
| `GNFS_LATTICE_LLL` | `LLL` | `0` 或 `gauss` 改用 Lagrange-Gauss 约化 | [sieve.md](sieve.md#lattice-basis-reduction-method-gnfs_lattice_lll) |
| `GNFS_LATTICE_SKEW` | 关闭 | skewness 不为 1 时把 LLL 升级为 SkewLLL | [sieve.md](sieve.md#skew-aware-lattice-reduction-gnfs_lattice_skew) |
| `GNFS_LINALG_MMAP` | `off` | Block Wiedemann 的矩阵落盘：`on` 强制，`auto` 按大小；落盘后 `GNFS_BW_ALGORITHM` 失效 | [linalg.md](linalg.md#linear-algebra-matrix-mmap-policy-gnfs_linalg_mmap) |
| `GNFS_LINALG_MMAP_THRESHOLD_BYTES` | `2147483648` | `auto` 模式的落盘阈值，单位字节 | [linalg.md](linalg.md#gnfs_linalg_mmap_threshold_bytes) |
| `GNFS_METAL_SPMV` | 关闭 | macOS 上 Block Wiedemann 的 SpMV 优先用 Metal GPU | [linalg.md](linalg.md#metal-spmv-opt-in-gnfs_metal_spmv) |
| `GNFS_NO_LINEAR_SHORTCUT` | 未设置 | 关闭首一一次多项式的 alpha 短路，用于基准对比 | [polynomial.md](polynomial.md#linear-polynomial-alpha-shortcut-gnfs_no_linear_shortcut) |
| `GNFS_POLY_BAI_BRENT` | 关闭 | 次数不低于 5 时优先尝试非首一的 Bai-Brent 选择器 | [polynomial.md](polynomial.md#non-monic-bai-brent-selection-gnfs_poly_bai_brent) |
| `GNFS_POLY_ROOT_CACHE_SIZE` | `0` | Murphy E 的 alpha 计算启用按素数记忆的缓存，值为条目上限 | [polynomial.md](polynomial.md#root-property-cache-gnfs_poly_root_cache_size) |
| `GNFS_SGE_STREAMING` | 关闭 | legacy 路线流式构建矩阵，`auto` 跟随 OOC 与续跑 | [linalg.md](linalg.md#streaming-matrix-build-gnfs_sge_streaming) |
| `GNFS_SIEVE_NO_TINY_SIMD` | 未设置 | 关闭 lattice sieve 小素数写入的 SIMD 路径，只有 `0` 能重新启用 | [sieve.md](sieve.md#tiny-prime-simd-gate-gnfs_sieve_no_tiny_simd) |
| `GNFS_TOPK_ALPHA` | 关闭 | Stage 2 候选按 lognorm 加廉价 alpha 排序 | [polynomial.md](polynomial.md#top-k-alpha-ranking-gnfs_topk_alpha) |
