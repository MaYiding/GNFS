/* zcode-workflow
description: Implement a set of independent GNFS directions in parallel, each in
  its own worktree with an independent review, and open one pull request per
  direction as soon as that direction is ready.
whenToUse: When a wave of mutually independent directions is ready to build.
  Give the directions in a JSON file (tasksFile) or inline as JSON (tasks), plus
  a wave label. Each direction gets a worktree under .worktrees/ and flows
  through implementation, review, static self-checks and its pull request on its
  own; a slow direction never delays the others. Remove each worktree once its
  pull request closes.
args:
  tasks:
    type: json
    description: The same array of directions given inline instead of in a file.
      Exactly one of tasksFile or tasks is required.
  tasksFile:
    type: string
    description: Workspace-relative path to a JSON file holding the array of
      directions. Each entry has slug, title, brief, and acceptance, plus an
      optional reviewFocus.
  wave:
    type: string
    description: Label used in the branch names, for example 20261128-5.
    required: true
*/
// Pipelined implementation wave for GNFS. Three stages pass directions through channels:
// implement and review, static self-checks, then push and open the pull request. A direction
// leaves a stage the moment it is ready, so one slow review does not hold back the others.
// Nothing is compiled locally; builds and tests run on GitHub CI.

interface Task {
  /** 路径安全的短标识，只含小写字母、数字和连字符，用于分支名和工作树目录名。 */
  slug: string;
  /** 一句话说明要做的事。 */
  title: string;
  /** 要改什么、改哪里、为什么，连同已知的约束和证据位置。 */
  brief: string;
  /** 用哪个 CI 工作流、测试或基准证明做成了。 */
  acceptance: string;
  /** 给独立评审的额外关注点，可选：这个方向最容易出错、评审必须盯住的地方。 */
  reviewFocus?: string;
}

interface Work {
  /** 英文的拉取请求标题，遵循仓库历史的写法，如 "fix(sieve): ..."。 */
  prTitle: string;
  /** 英文的拉取请求正文，Markdown：改动内容、验证方式、剩余风险。 */
  prBody: string;
  /** 你改了什么，中文，三到五句。 */
  summary: string;
  /** 改动的文件，仓库相对路径。 */
  filesChanged: string[];
  /** 你运行过的静态检查，以及结果。 */
  selfChecks: string[];
  /** 做不下去的原因；没有就写空字符串。 */
  blockers: string;
}

interface Problem {
  /** 位置：仓库相对路径加行号。用英文写，它会原样进入拉取请求正文。 */
  where: string;
  /** 什么情况下会出错，或者违背了哪条规则。用英文写，它会原样进入拉取请求正文。 */
  what: string;
  /** 会算错结果或崩溃为 high，其余为 medium 或 low。 */
  severity: "low" | "medium" | "high";
}

interface Review {
  /** 没有必须修的问题则为 true。 */
  approved: boolean;
  /** 必须修的问题；只是风格偏好的不要列。 */
  problems: Problem[];
}

interface Finding {
  /** 工作树路径和分支。 */
  where: string;
  /** 这个方向的结果。 */
  what: string;
  /** 依据：静态自检的输出，独立评审的遗留问题，以及拉取请求地址。 */
  evidence: string;
  /** 拉取请求确实开出、静态自检全部通过、独立评审也通过，才是 verified；否则为 unconfirmed。 */
  status: "verified" | "unconfirmed";
  /** 重要程度。 */
  severity: "low" | "medium" | "high";
}
interface WorkflowReport {
  conclusion: string;
  findings: Finding[];
  verified: string[];
  notCovered: string[];
}

interface Item {
  task: Task;
  slug: string;
  branch: string;
  wt: string;
}

/** A direction whose implementation and review have finished. */
interface Reviewed {
  item: Item;
  worker: Agent;
  work: Work;
  /** 最后一轮评审仍然提出、之后没有再处理的问题；评审通过后是空数组。 */
  leftover: Problem[];
}

/** A direction after the static self-checks: ready for its pull request, or stopped. */
type Gated =
  | { kind: "ready"; item: Item; work: Work; gate: string; leftover: Problem[] }
  | { kind: "stuck"; item: Item; work: Work; note: string };
const isReady = (value: Gated): value is Extract<Gated, { kind: "ready" }> => value.kind === "ready";

const rawWave = args.wave;
const waveRaw = typeof rawWave === "string" ? rawWave : "wip";
const wave = waveRaw.toLowerCase().replace(/[^a-z0-9-]+/g, "-") || "wip";
// The directions come from a JSON file (tasksFile) or inline (tasks). Inline arguments arrive
// as a JSON string, so both spellings are accepted here; exactly one of the two must be given.
const rawTasksFile = args.tasksFile;
const rawTasks = args.tasks;
type Source = { kind: "file"; path: string } | { kind: "inline"; text: string };
let source: Source;
if (typeof rawTasksFile === "string" && rawTasksFile !== "") {
  source = { kind: "file", path: rawTasksFile };
} else if (Array.isArray(rawTasks)) {
  source = { kind: "inline", text: JSON.stringify(rawTasks) };
} else if (typeof rawTasks === "string" && rawTasks.trim() !== "") {
  source = { kind: "inline", text: rawTasks };
} else {
  throw new Error("要给 tasksFile（任务 JSON 文件的仓库相对路径）或 tasks（方向的 JSON 数组），二者给一个");
}

const MAX_REVISIONS = 2;
const GATE_ROUNDS = 3;
// Static self-checks are CPU-bound Python; more than a few at once only slow each other down.
const GATE_WORKERS = 3;

const WORKER =
  "你是 GNFS 仓库的资深 C++ 工程师，熟悉数域筛算法。" +
  "这台机器不允许编译，你的改动只能靠静态阅读和几项静态检查把关，所以宁可小而完整，也不要大而没把握。" +
  "遇到做不到的事，或者约束之间互相冲突，直接说明并写进 blockers，不要硬凑。";

const REVIEWER =
  "你是 GNFS 仓库的独立代码评审员，专找会算错结果、崩溃、或在另一个平台表现不同的问题。你只读代码，不改代码。";

// Static self-checks equivalent to the CI jobs "zsh/bash syntax", "Local Markdown links" and the env-flag ledger:
// Python, shell, and git inspection only. No compiler, build system, or test binary is invoked.
const STATIC =
  "cd \"$1\" || exit 99\n" +
  "out=$(mktemp) || exit 99\n" +
  "trap 'rm -f \"$out\"' EXIT\n" +
  "fail=0\n" +
  "step() { name=\"$1\"; shift; if \"$@\" >\"$out\" 2>&1; then printf '  ok   %s\\n' \"$name\"; " +
  "else printf '  FAIL %s\\n' \"$name\"; tail -n 25 \"$out\" | sed 's/^/       /'; fail=1; fi; }\n" +
  "step 'release_contract self-test'        python3 scripts/release_contract.py self-test\n" +
  "step 'release_contract check-workflows'  python3 scripts/release_contract.py check-workflows\n" +
  "step 'release_binary_contract self-test' python3 scripts/release_binary_contract.py self-test\n" +
  "step 'reproducible_archive self-test'    python3 scripts/reproducible_archive.py self-test\n" +
  "step 'windows_release_runtime self-test' python3 scripts/windows_release_runtime.py self-test\n" +
  "step 'windows_runtime_contract'          python3 scripts/windows_runtime_contract.py self-test\n" +
  "step 'stdout_contract self-test'         python3 scripts/check_stdout_contract.py --self-test\n" +
  "step 'stdout_contract'                   python3 scripts/check_stdout_contract.py\n" +
  "step 'harness'                           python3 scripts/check_harness.py\n" +
  "step 'policy self-test'                  python3 scripts/check_distributed_sieve_policy.py --self-test\n" +
  "step 'markdown links'                    python3 scripts/ci/check_markdown_links.py\n" +
  "[ -f scripts/ci/check_markdown_links.py ] && step 'link checker self-test' python3 scripts/ci/check_markdown_links.py --self-test\n" +
  "[ -f scripts/ci/check_env_flag_docs.py ] && step 'env flag docs' python3 scripts/ci/check_env_flag_docs.py\n" +
  "[ -f scripts/ci/format_changed.sh ] && [ -x /tmp/gnfs-ops/cf18/bin/clang-format ] && "
    "step 'clang-format 18 (changed files)' sh scripts/ci/format_changed.sh origin/main /tmp/gnfs-ops/cf18/bin/clang-format\n" +
  "[ -f scripts/ci/check_test_asserts.py ] && step 'test asserts' python3 scripts/ci/check_test_asserts.py\n" +
  "step 'test catalog'                      zsh ./scripts/test.sh list\n" +
  "exit $fail\n";

const RULES =
  "硬性约束：\n" +
  "- 本机不允许编译。不要运行构建、测试或任何会触发编译的命令：./scripts/test.sh 除 list 以外的子命令、cmake、make、ninja、编译器、scripts/test-app.sh 都不行。\n" +
  "- 可以运行的静态检查（都与 CI 等价、不触发编译）：git diff --check；zsh ./scripts/test.sh list；python3 scripts/check_harness.py；" +
  "python3 scripts/check_distributed_sieve_policy.py --self-test；python3 scripts/check_stdout_contract.py；python3 scripts/ci/check_markdown_links.py；" +
  "python3 scripts/ci/check_env_flag_docs.py（存在的话）。\n" +
  "- 格式：CI 用 clang-format 18 检查，本机装的版本不同，它的输出不能当准。照着周边代码的写法写。\n" +
  "- 搜索和读取只限当前仓库，不要以 ~ 或 ~/Library 为搜索根。\n" +
  "- 不要 force-push，不要合并，不要改动 main。";

function implementAsk(item: Item): string {
  return (
    `${RULES}\n` +
    `- 只在工作树 ${item.wt} 里改动（分支 ${item.branch}），不要碰仓库的其它目录。\n` +
    "- 动手前先读 AGENTS.md 和 docs/writing-style-guide.md。\n" +
    "- 代码注释和提交标题用英文；提交标题遵循仓库历史的写法（fix(scope): ...、feat(scope): ...、perf(...)、test(...)、docs(...)、refactor(...)、ci(...)）。\n" +
    "- 不要推送，也不要开拉取请求：这两步由脚本在你完成并通过自检后统一做。\n" +
    "- 脚本会在你之后统一跑全套静态自检，你自己只跑与这次改动直接相关的那几项，不必全套重跑。\n\n" +
    `你要做的：${item.task.title}\n` +
    `要改什么：${item.task.brief}\n` +
    `怎么算做成：${item.task.acceptance}\n\n` +
    "步骤：读相关代码和测试，做最小的完整改动（需要的话加上测试和文档），" +
    "把改动提交成若干个各自说得清的提交，" +
    "最后运行 git diff --check 并确认 git status --porcelain 没有输出。\n" +
    "prTitle 和 prBody 按写作规范用英文写；prBody 写清改了什么、没能在本地验证什么、剩余风险是什么。\n" +
    "blockers 只在真的做不下去时才写：只是没法在本机验证的事不算做不下去，写进 prBody 的风险一节即可，CI 会替你验证。" +
    "做不下去就把原因写进 blockers。"
  );
}

function reviewAsk(item: Item): string {
  return (
    "硬性约束：只读，不要编辑任何文件；不要运行构建、测试或会触发编译的命令。\n" +
    `工作树在 ${item.wt}（分支 ${item.branch}），改动范围是 origin/main...HEAD，用 git -C 查看。\n\n` +
    "你没有参与实现，请来挑毛病：读改动和它周边的代码，对照 AGENTS.md 的规则" +
    "（代数元素写成 a - b*alpha；GF(2) 矩阵只能用 schirokauer_primes = {2}；拒绝 gcd(a - b*m, N) > 1 的关系；" +
    "大素数列启用时裁剪上限必须算上大素数列；LP64 差异；浮点转整数前做范围检查；不能只靠 assert 做测试断言；" +
    "新测试要登记 CMake 和 scripts/test.sh；直接用到的标准库头文件要自己包含）。\n" +
    "只看这次改动和它直接触及的代码，不要通读整个仓库；给每个必须修的问题指出具体的 path:line。\n" +
    "回答：什么情况下这段改动会算错结果、崩溃、或在另一个平台上表现不同？有没有漏掉测试和文档？" +
    "approved 只在没有必须修的问题时为 true；只列必须修的问题，风格偏好不要列。" +
    "where 和 what 两个字段用英文写，它们会原样进入拉取请求正文。" +
    (item.task.reviewFocus === undefined ? "" : `\n\n这个方向要特别盯住的地方：\n${item.task.reviewFocus}`)
  );
}

function reviseAsk(review: Review): string {
  return (
    "独立评审认为你的改动有以下必须修的问题（JSON）：\n" +
    JSON.stringify(review.problems) +
    "\n\n请在同一个工作树里逐条处理：同意的修掉，不同意的在 selfChecks 里写明理由。" +
    "修完重新提交，并确认 git diff --check 和 git status --porcelain 都没有输出。" +
    "prBody 里的说明跟着更新，其它字段一并更新。评审意见不会让你做不下去：不同意的不改并说明理由即可，不要写进 blockers。"
  );
}

function gateAsk(detail: string): string {
  return (
    "脚本在你的工作树里跑了提交前的检查，有失败，输出如下：\n" +
    detail +
    "\n\n请修到全部通过：有空白问题就改掉，有未提交的改动就提交或者撤销，静态自检失败就修到通过。" +
    "修完提交，并确认 git status --porcelain 没有输出。prBody 如果需要也一并更新。修不了就把原因写进 blockers。"
  );
}

// A network command that times out must count as that step failing, not as the whole run failing.
async function pushBranch(item: Item): Promise<WorldRunResult> {
  try {
    return await world.run("git", ["-C", item.wt, "push", "-u", "origin", item.branch], {
      timeoutMs: 900_000,
      output: { tail: 16_384 },
    });
  } catch (error) {
    return { exitCode: 1, stdout: "", stderr: `推送没有完成：${String(error)}` };
  }
}

async function openPullRequest(
  item: Item,
  body: string,
  title: string,
  draft: boolean,
): Promise<WorldRunResult> {
  try {
    return await world.run("gh", [
      "pr", "create", "--base", "main", "--head", item.branch, "--title", title, "--body", body,
      ...(draft ? ["--draft"] : []),
    ]);
  } catch (error) {
    return { exitCode: 1, stdout: "", stderr: `拉取请求没有开成：${String(error)}` };
  }
}

function prUrl(result: WorldRunResult): string {
  return result.exitCode === 0
    ? (result.stdout.split("\n").map((line) => line.trim()).filter((line) => line.includes("/pull/")).pop() ?? "")
    : "";
}

async function gate(wt: string): Promise<{ ok: boolean; detail: string }> {
  const changed = await world.run("git", ["-C", wt, "diff", "--name-only", "origin/main...HEAD"]);
  const files = changed.stdout.split("\n").map((line) => line.trim()).filter((line) => line !== "");
  if (files.length === 0) return { ok: false, detail: "这个分支相对 main 没有任何改动。" };
  const whitespace = await world.run("git", ["-C", wt, "diff", "--check", "origin/main...HEAD"]);
  if (whitespace.exitCode !== 0) {
    return { ok: false, detail: `git diff --check 报告了空白问题：\n${whitespace.stdout}${whitespace.stderr}` };
  }
  const status = await world.run("git", ["-C", wt, "status", "--porcelain"]);
  if (status.stdout.trim() !== "") {
    return { ok: false, detail: `工作树里有未提交的改动：\n${status.stdout}` };
  }
  let checks: WorldRunResult;
  try {
    checks = await world.run("sh", ["-c", STATIC, "sh", wt], {
      timeoutMs: 1_200_000,
      output: { tail: 16_384 },
    });
  } catch (error) {
    return { ok: false, detail: `静态自检没有跑完：${String(error)}` };
  }
  if (checks.exitCode !== 0) return { ok: false, detail: `静态自检有失败：\n${checks.stdout}` };
  return { ok: true, detail: checks.stdout };
}

// Whether the branch holds committed work worth a draft pull request.
async function hasCommittedWork(item: Item): Promise<boolean> {
  const count = await world.run("git", ["-C", item.wt, "rev-list", "--count", "origin/main..HEAD"]);
  return count.exitCode === 0 && count.stdout.trim() !== "0" && count.stdout.trim() !== "";
}

artifact.board("progress", {
  title: "本波次进度",
  key: "slug",
  status: "state",
  columns: ["实现中", "自检中", "已开拉取请求", "受阻"],
  cardTitle: "title",
  detail: [
    { field: "pr", label: "拉取请求" },
    { field: "note", label: "说明" },
  ],
});

phase("为每个方向准备独立的工作树");
const tasksText = source.kind === "file" ? await files.read(source.path) : source.text;
const tasks = JSON.parse(tasksText);
if (!Array.isArray(tasks)) throw new Error("任务文件必须是方向的数组");
const root = (await world.run("pwd")).stdout.trim();
const fetched = await world.run("git", ["fetch", "origin", "main", "--quiet"], { timeoutMs: 600_000 });
if (fetched.exitCode !== 0) throw new Error("拿不到 main 的最新提交，无法开始。");
const listing = await world.run("git", ["worktree", "list", "--porcelain"]);
const items: Item[] = [];
const usedSlugs: string[] = [];
for (const task of tasks as Task[]) {
  const base = (typeof task.slug === "string" ? task.slug : "").toLowerCase().replace(/[^a-z0-9-]+/g, "-");
  if (base === "") {
    log(`有一个方向的 slug 不可用，跳过：${String(task.title)}`);
    continue;
  }
  let slug = base;
  for (let n = 2; usedSlugs.includes(slug); n++) slug = `${base}-${n}`;
  usedSlugs.push(slug);
  const item: Item = { task, slug, branch: `codex/${slug}-${wave}`, wt: `${root}/.worktrees/${slug}` };
  // A rerun after an interruption finds the worktree already on the branch and reuses it.
  const exists = listing.stdout.split("\n\n").some(
    (block) => block.includes(`worktree ${item.wt}\n`) && block.includes(`branch refs/heads/${item.branch}\n`),
  );
  if (!exists) {
    const add = await world.run("git", ["worktree", "add", "-b", item.branch, item.wt, "origin/main"]);
    if (add.exitCode !== 0) {
      log(`建不了 ${slug} 的工作树：${add.stderr.trim()}`);
      continue;
    }
  }
  items.push(item);
  report({ slug, title: String(task.title), state: "实现中", pr: "", note: "" }, "progress");
}
if (items.length === 0) throw new Error("没有建成任何工作树。");
log(`共 ${items.length} 个方向，开始并行实现`);

const reviewed = channel<Reviewed>("reviewed");
const gated = channel<Gated>("gated");
let findings: Finding[] = [];
let opened: string[] = [];
let stuckCount = 0;

const implementStage = future(async () => {
  phase("并行实现，每个方向都由独立评审把关");
  try {
    await Promise.all(
      items.map(async (item) => {
        const worker = agent(`实现者·${item.slug}`, WORKER);
        try {
          let work = await worker.ask<Work>(implementAsk(item));
          let leftover: Problem[] = [];
          // Every revision is reviewed again; the last round only reviews.
          for (let round = 0; round <= MAX_REVISIONS && work.blockers === ""; round++) {
            const review = await agent(`评审者·${item.slug}·第${round + 1}轮`, REVIEWER).ask<Review>(
              reviewAsk(item),
            );
            if (review.approved) {
              leftover = [];
              break;
            }
            leftover = review.problems;
            if (round < MAX_REVISIONS) work = await worker.ask<Work>(reviseAsk(review));
          }
          reviewed.send({ item, worker, work, leftover });
        } catch (error) {
          const failed: Work = {
            prTitle: "",
            prBody: "",
            summary: "",
            filesChanged: [],
            selfChecks: [],
            blockers: `实现过程中出错：${String(error)}`,
          };
          reviewed.send({ item, worker, work: failed, leftover: [] });
        }
      }),
    );
  } finally {
    reviewed.close(); // the producer side owns the close; a failed implementer still ends the stream
  }
});

const gateStage = future(async () => {
  phase("对每个方向做提交前的静态自检");
  try {
    await Promise.all(
      [1, 2, 3].slice(0, GATE_WORKERS).map(async () => {
        for await (const entry of reviewed) {
          const item = entry.item;
          if (entry.work.blockers !== "") {
            gated.send({ kind: "stuck", item, work: entry.work, note: entry.work.blockers });
            continue;
          }
          report({ slug: item.slug, title: item.task.title, state: "自检中", pr: "", note: "" }, "progress");
          let work = entry.work;
          let passed = false;
          let detail = "";
          for (let round = 0; round < GATE_ROUNDS; round++) {
            const result = await gate(item.wt);
            if (result.ok) {
              passed = true;
              detail = result.detail;
              break;
            }
            detail = result.detail;
            try {
              work = await entry.worker.ask<Work>(gateAsk(result.detail));
            } catch (error) {
              work = { ...work, blockers: `修自检问题时出错：${String(error)}` };
            }
            if (work.blockers !== "") {
              detail = work.blockers;
              break;
            }
          }
          if (passed) gated.send({ kind: "ready", item, work, gate: detail, leftover: entry.leftover });
          else gated.send({ kind: "stuck", item, work, note: `自检没有通过：${detail}` });
        }
      }),
    );
  } finally {
    gated.close();
  }
});

const pushStage = future(async () => {
  phase("推送并开拉取请求");
  // Pull requests open one after another so each result carries forward to the next.
  for await (const outcome of gated) {
    const item = outcome.item;
    if (!isReady(outcome)) {
      // Committed work still gets a draft pull request: CI then says what could not be checked here.
      const committable = await hasCommittedWork(item);
      let note = outcome.note;
      let url = "";
      if (committable && (await gate(item.wt)).ok) {
        const push = await pushBranch(item);
        if (push.exitCode === 0) {
          const body =
            `## Blockers\n\nThe implementer stopped here; this draft exists so that CI can run on the committed work.\n\n${outcome.note}\n\n` +
            (outcome.work.prBody === "" ? "" : `${outcome.work.prBody}\n\n`);
          const title = outcome.work.prTitle === "" ? `WIP: ${item.slug}` : `WIP: ${outcome.work.prTitle}`;
          url = prUrl(await openPullRequest(item, body, title, true));
        } else {
          note = `${note}；推送失败：${push.stderr.trim()}`;
        }
      }
      stuckCount += 1;
      report(
        {
          slug: item.slug,
          title: item.task.title,
          state: "受阻",
          pr: url,
          note: `${note}${url === "" ? "" : "（已开草稿拉取请求）"}`,
        },
        "progress",
      );
      findings = [
        ...findings,
        {
          where: `${item.wt}（${item.branch}）`,
          what: `${item.task.title}：受阻，${url === "" ? "没有开拉取请求" : "开了草稿拉取请求"}`,
          evidence: note,
          status: "unconfirmed",
          severity: "medium",
        },
      ];
      continue;
    }

    const push = await pushBranch(item);
    if (push.exitCode !== 0) {
      const note = `推送失败：${push.stderr.trim()}`;
      stuckCount += 1;
      report({ slug: item.slug, title: item.task.title, state: "受阻", pr: "", note }, "progress");
      findings = [...findings, { where: `${item.wt}（${item.branch}）`, what: `${item.task.title}：没有开成拉取请求`, evidence: note, status: "unconfirmed", severity: "medium" }];
      continue;
    }
    const lines = outcome.leftover.map((problem) => `- **${problem.severity}** \`${problem.where}\`: ${problem.what}`);
    const body =
      outcome.leftover.length === 0
        ? outcome.work.prBody
        : `${outcome.work.prBody}\n\n## Open Review Findings\n\n` +
          "An independent reviewer still raised the following points after the last revision. " +
          "They are unresolved at the time this pull request opens:\n\n" +
          lines.join("\n");
    const url = prUrl(await openPullRequest(item, body, outcome.work.prTitle, false));
    if (url === "") {
      stuckCount += 1;
      const note = `拉取请求没有开成`;
      report({ slug: item.slug, title: item.task.title, state: "受阻", pr: "", note }, "progress");
      findings = [...findings, { where: `${item.wt}（${item.branch}）`, what: `${item.task.title}：没有开成拉取请求`, evidence: note, status: "unconfirmed", severity: "medium" }];
      continue;
    }
    const reviewNote =
      outcome.leftover.length === 0
        ? "独立评审通过。"
        : `独立评审在最后一次修订后仍提出 ${outcome.leftover.length} 条问题，已附在拉取请求正文里。`;
    opened = [...opened, `${item.task.title}：${url}`];
    report(
      {
        slug: item.slug,
        title: item.task.title,
        state: "已开拉取请求",
        pr: url,
        note: `${outcome.work.summary} ${reviewNote}`,
      },
      "progress",
    );
    findings = [
      ...findings,
      {
        where: `${item.wt}（${item.branch}）`,
        what: `${item.task.title}：${outcome.work.summary}`,
        evidence: `静态自检全部通过：\n${outcome.gate}\n${reviewNote}\n拉取请求：${url}`,
        status: outcome.leftover.length === 0 ? "verified" : "unconfirmed",
        severity: outcome.leftover.some((problem) => problem.severity === "high") ? "high" : "medium",
      },
    ];
  }
});

// Start every stage, then join once.
await Promise.all([implementStage, gateStage, pushStage]);

return {
  conclusion:
    `${items.length} 个方向里，${opened.length} 个开出了拉取请求，${stuckCount} 个受阻。` +
    "改动只经过了静态自检和独立评审，还没有经过构建和测试：拉取请求开门约十分钟后能看到编译和多数测试的结果，约四十五分钟全部出来。" +
    "工作树留在各自目录里等检查结果，拉取请求合并或者关闭后要清掉。",
  findings,
  verified: [
    "每个方向都由没有参与实现的评审员读改动挑毛病，每次修订之后都会再审一次",
    "每个推出去的分支都通过了 git diff --check、干净的工作树，以及与 CI 等价的十二项静态自检",
    "分支已推送到远端，拉取请求地址由 gh 命令返回",
  ],
  notCovered: [
    "构建和测试：本机不允许编译，由拉取请求的 GitHub 检查执行",
    "格式：本机 clang-format 是 23 版、CI 是 18 版，格式由 CI 的检查把关，开门一分钟内出结果",
    ...opened,
  ],
} satisfies WorkflowReport;
