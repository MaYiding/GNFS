# README 与文档写作规范

> 撰写或修改 README / docs 设计文档 / 长 commit message body / PR 描述等正式文本前必读。
> 返回: [CLAUDE.md](../CLAUDE.md)

---

`README.md` 是首次访问 GitHub 仓库页面的访客的「门面」，撰写或修改 README/docs 时严格遵循以下规范。所有规则同样适用于 `docs/` 下的设计文档、长 commit message body、PR 描述等正式文本。

## GitHub Markdown 渲染规范

GitHub 使用 GFM（GitHub Flavored Markdown），与 CommonMark 有细微差异，撰写时必须考虑实际渲染效果：

- **标题层级**：全文仅 1 个 H1（项目名，文件首行）；H2 作章节，H3 作子节；不跳级（H2 → H4 ❌）
- **标题锚点**：GitHub 自动从标题文本生成锚点（lowercase + dash，删除标点）。跨章节链接用 `[文本](#标题文本)`。含中文的锚点保持中文（URL encoded），但稳定性差，**优先用英文标题或独立 anchor**
- **代码块（必带语言标识）**：三反引号后紧贴语言标识，**不要留空格**
  - `` ```cpp ``、`` ```bash ``、`` ```cmake ``、`` ```python `` 等小写
  - 纯输出或无语言用 `` ```text ``
  - ❌ `` ``` `` 裸开 — GitHub 不会高亮且语义丢失
- **行内代码**：函数名、变量名、文件路径、命令、ENV 变量一律用反引号
  - ✅ `` `Pipeline::filter()` ``、`` `include/gnfs/api/pipeline.hpp` ``、`` `GNFS_OOC_RELATIONS` ``
- **表格**：GFM 表格需对齐符（`|---|:---:|---:|` 表左/居中/右）；单元格内不能换行，长文用 `<br>` 或拆行；列数 ≤ 6 以保证窄屏移动端不挤压
- **任务列表**：`- [ ]` 未完成 / `- [x]` 已完成（GFM 扩展；仅 README/issue/PR 渲染为复选框）
- **水平线**：统一用 `---`（不混用 `***` / `___`）
- **引用块**：`>` 前后必须留空行；嵌套用 `> >`（两层）
- **链接**：
  - 仓库内文件用相对路径：`[Pipeline](src/api/pipeline.cpp)` 优于绝对 URL
  - 裸 URL 用尖括号包裹防止 markdown linkify 失败：`<https://example.com>`
  - 锚点链接必须 **lowercase**：`[Build](#build-system)` ✅，`[Build](#Build-System)` ❌
- **图片**：
  - 优先存仓库内 `docs/images/`（用路径引用，不依赖外部 host）
  - **避免** `user-attachments.githubusercontent.com` 这类 issue 上传 URL — 用户重命名仓库或删除 attachment 会失效
  - 必须有 `alt` 文本：`![CI Status](badge.svg "CI passing")`，不要写 `![](badge.svg)`
  - 深浅模式双图用 `<picture>` 包裹：
    ```html
    <picture>
      <source srcset="dark.png" media="(prefers-color-scheme: dark)">
      <img src="light.png" alt="Architecture">
    </picture>
    ```
- **允许的 HTML 子集**：GitHub sanitize HTML，仅放行 `<details>`、`<summary>`、`<sub>`、`<sup>`、`<br>`、`<kbd>`、`<picture>`、`<source>`、`<img>`、`<a>` 等少数标签。`<script>`、`<style>`、自定义 `class`/`id`/`onclick` 等属性会被剥离
- **HTML 块内不解析 Markdown**：`<details>` 内的 markdown 需在 `<summary>` 之后**空一行**才会解析
- **Badge**：用 [shields.io](https://shields.io) 标准格式；紧邻 H1 下方同行排列；推荐顺序 — CI status / coverage / version / license
- **不要使用 emoji**：项目政策禁止 emoji（除非用户明确请求）。即便用，shortcode（`:rocket:`）的 GitHub 兼容性也优于直接 Unicode emoji
- **trailing whitespace**：行末空格在 Markdown 中是 hard line break，**不要留**；编辑器开 "trim trailing whitespace on save"

## 中英文混排规范

参考 [中文文案排版指北](https://github.com/sparanoid/chinese-copywriting-guidelines)：

- **中英文之间加半角空格**：`使用 GMP 库` ✅，`使用GMP库` ❌
- **中文与阿拉伯数字之间加空格**：`支持 64 位整数` ✅，`支持64位整数` ❌
- **数字与单位之间不加空格**（SI 惯例）：`100MB`、`5%`、`10ms` ✅；但 `10 个测试` 要加空格（数字 + 量词）
- **半角与全角标点**：
  - 中文体：用全角 `，。；：？！「」（）`
  - 英文体：用半角 `,.;:?!"'()`
  - **数学符号 / 代码内 / inline code 内**：半角（如 `x = 3, y = 5`）
- **不混用简繁体**：全文保持简体中文（除非引用繁体原文献）
- **常见错别字**：「的 / 地 / 得」、「再 / 在」、「做 / 作」、「他 / 她 / 它」需区分
- **数学公式**：行内用 `$...$`（GitHub 已支持 MathJax），块级用 `$$...$$`；纯字母变量用斜体（`$f(x)$`），运算符不要斜体

## 正规英语写作规范

撰写 README 英文段落、API 文档、英文 commit body、英文注释时遵循：

- **避免破折号（em-dash）插入语，优先用从句或括号**
  - ❌ `GNFS — the most powerful classical factoring algorithm — is the standard for large numbers.`
  - ✅ `GNFS, which is the most powerful classical factoring algorithm, is the standard for large numbers.`
  - ✅ `GNFS (the most powerful classical factoring algorithm) is the standard for large numbers.`
- **复合形容词的连字符（hyphen）规则**
  - 修饰名词时连字符：`high-performance code`、`64-bit integer`、`memory-mapped file`
  - 作谓语时不连字符：`the code is high performance`、`the integer is 64 bits wide`
- **Oxford comma 必须保留**：`A, B, and C`（清晰区分 list 最后两项与并列结构）
- **缩写首次使用展开**：`General Number Field Sieve (GNFS)` 首次出现展开；后续可直接 `GNFS`
- **拉丁缩写后加逗号**：`e.g.,`、`i.e.,`、`etc.,`；句首避免用，改为 `For example,` / `That is,`
- **被动语态优先改主动**：
  - ❌ `The matrix was solved by Block Lanczos.`
  - ✅ `Block Lanczos solves the matrix.`
- **数字拼写规则**：0-9 拼写（`zero`、`nine`）；10+ 用阿拉伯数字（`10`、`1000`）；**句首数字必须拼写**（`Twelve tests pass` ✅，`12 tests pass` ❌ 作句首）
- **避免缩约形式（contraction）**：正式文档用 `do not` 不用 `don't`，`cannot` 不用 `can't`，`it is` 不用 `it's`
- **避免主观强化词**：`very fast`、`really good`、`quite efficient` → 量化（`5× faster than baseline`、`under 5ms`）
- **句号后单空格**（现代标准；早期打字机时代的双空格规则已废除）
- **标题用 Title Case**：`## Build System`、`## Performance-Critical Code`（主要词首字母大写，介词 / 冠词 / 并列连词除外，但句首词永远大写）
- **避免冗余**：
  - ❌ `in order to` → ✅ `to`
  - ❌ `due to the fact that` → ✅ `because`
  - ❌ `at this point in time` → ✅ `now`

## 正规中文写作规范

中文 README 段落、设计文档、长 commit body 遵循：

- **统一全角标点**：`，。；：？！「」（）——……`
- **省略号**：用 `……`（两个 `…`，共 6 个点），不用 `...` 或 `。。。`
- **破折号**：表示插入语或转折时用 `——`（两个全角 em-dash），不用 `--` 或 `-`
- **引号嵌套**：外层 `「」`，内层 `『』`；或外层 `""`，内层 `''`；全文一致，不混用
- **数字格式**：
  - 阿拉伯数字表示精确量：`64 位`、`300 个测试`、`5 月 18 日`
  - 汉字数字表示约数或习语：`几十种`、`三五次`、`两三天`
- **专有名词不翻译**：GitHub、CMake、GMP、NTL、Linux、macOS、LLVM 保持英文
- **避免直译腔**：
  - ❌ `这是一个高性能的实现` → ✅ `实现高性能` 或 `性能优异`
  - ❌ `让我们考虑这个问题` → ✅ `考虑这个问题`
- **避免冗余动词组**：
  - ❌ `进行编译` → ✅ `编译`
  - ❌ `做出修改` → ✅ `修改`
  - ❌ `给予支持` → ✅ `支持`
- **避免「做」滥用**：用具体动词替代（实现 / 完成 / 执行 / 编写 / 调用 / 部署）
- **句长控制**：单句不超过 50 字；超过则拆分或加分号 / 句号
- **避免「的」字句堆叠**：`一个高性能的多线程的并行的实现` ❌ → `高性能多线程并行实现` ✅

## README 撰写检查清单

提交 README/docs 改动前确认：

- [ ] **本地渲染验证**：用 `grip` 或 VSCode "Markdown Preview Enhanced" 检查 GFM 渲染效果（裸 CommonMark preview 不等同 GitHub）
- [ ] **所有内部链接有效**：文件相对路径 / 锚点链接 / 图片路径都可点开
- [ ] **所有外部链接 HTTP 200**：第三方库主页 / 论文 DOI / 在线 demo 用 `lychee` 批量校验
- [ ] **代码块带正确语言标识**：每个 fence 都标 `cpp` / `bash` / `cmake` / `text` 等
- [ ] **表格在窄屏可读**：列数 ≤ 6；列宽不要让单元格挤到换行
- [ ] **图片有 alt 文本**：`![desc](path)` 而非裸 `![](path)`
- [ ] **HTML 嵌入实测**：`<details>` / `<picture>` 等需在 GitHub 实际渲染验证（本地 preview 可能假阳性）
- [ ] **无 trailing whitespace**：grep 检查 `grep -nE " +$" README.md` 应为空
- [ ] **中英文混排空格一致**：用 `autocorrect` 自动修正
- [ ] **拼写检查**：英文用 `aspell` 或 VSCode "Code Spell Checker"
- [ ] **目录（TOC）同步**：章节增删后头部 TOC 一并更新
- [ ] **commit 前 diff 复审**：`git diff README.md` 重读一遍，避免误删 / 误改

## 推荐工具链

| 工具 | 用途 | 安装 |
|------|------|------|
| `autocorrect` | 中英文混排空格自动修正 | `cargo install autocorrect-cli` |
| `markdownlint-cli` | Markdown 语法静态检查 | `npm i -g markdownlint-cli` |
| `grip` | 本地 GitHub-flavored Markdown 渲染预览 | `pip install grip` |
| `lychee` | 批量链接有效性检查 | `cargo install lychee` |
| `aspell` / `hunspell` | 英文拼写检查 | `brew install aspell` |
| `prettier` | Markdown 格式统一（缩进、列表对齐） | `npm i -g prettier` |
