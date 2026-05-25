# TODO.md — 用户待办清单

> **用途**: 由用户手动书写，Claude 的 Stop Hook 会在此文件有未完成项时阻止 Claude 结束，
> 要求 Claude 先 `/compact` 压缩上下文再继续工作。
>
> **格式**: 标准 Markdown 任务列表
> - 未完成的项: `- [ ] 任务描述`
> - 已完成的项: `- [x] 任务描述`（手动打勾或由 Claude 完成后标记）
>
> **与 task_plan.md / BACKLOG.md 的关系**:
> - `TODO.md` — 用户书写的待办（本文件）
> - `task_plan.md` — 模型自动生成的详细开发计划
> - `BACKLOG.md` — 模型发现的 bug 和技术债务
> - 三者互不冲突，Stop Hook 仅检查本文件

---

<!-- 在下方书写你的待办事项，Claude 会持续处理直到全部标记为 [x] -->
<!-- 示例: -->
<!-- - [ ] 修复 test_linalg 段错误 -->
<!-- - [x] 已完成的优化任务 -->

- [x] 告诉用户当前时间