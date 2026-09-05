# GNFS ENV 调优开关文档

本目录收录所有 `GNFS_*` 运行时调优开关 / helper 的**详细设计文档**(算法、ENV 解析规则、bit-for-bit 保证、ROI 与定位、集成点、测试清单),按代码模块组织。

顶层一句话速查表见 [CLAUDE.md → ENV 调优开关总览](../../CLAUDE.md#env-调优开关总览)。新增开关的文档化规范见 [CLAUDE.md → 设计细节文档化规范](../../CLAUDE.md#设计细节文档化规范强制)。

## 模块导航

| 模块 | 开关数 | 文档 | 主题 |
|------|-------:|------|------|
| relation | 13 | [relation.md](relation.md) | structured filter、filter merge (V0/V3)、OOC、LP key dedup、radix sort |
| linalg | 15 | [linalg.md](linalg.md) | BW Krylov、SpMV/GF(2) SIMD、SGE、转置、进度遥测 |
| cofactor | 10 | [cofactor.md](cofactor.md) | ECM stage 并行/cache、survival predictor、Brent rho |
| polynomial | 9 | [polynomial.md](polynomial.md) | Karatsuba / NTT / HGCD / squaring / mod-p SIMD |
| sieve | 9 | [sieve.md](sieve.md) | checkpoint、cache tile、prefetch、lattice SIMD |
| siqs | 1 | [siqs.md](siqs.md) | default-off shadow proof observe / explicit prefer |
| util | 6 | [util.md](util.md) | Integer scratch pool、批量 GMP `mpz_*` 并行 |
| sqrt | 2 | [sqrt.md](sqrt.md) | Hensel lift 并行、Couveignes pattern search 并行 |
| factor_base | 1 | [factor_base.md](factor_base.md) | Cantor-Zassenhaus 求根并行 |

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
