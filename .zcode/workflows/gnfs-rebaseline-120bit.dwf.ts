/* zcode-workflow
description: Re-baseline the fixed 120-bit gate identity on a GNFS branch by
  running the Nightly workflow on that branch, reading the observed values from
  its log, updating the expected constants, and confirming with a second run.
whenToUse: "When a change moves the 120-bit matrix identity and the new expected
  values can only be collected on GitHub CI. The test must print its observed
  values before asserting. AmendWorkflow does not carry arguments: set
  FALLBACK_BRANCH in the file before amending a run."
args:
  branch:
    type: string
    description: Branch that holds the change and has a worktree under .worktrees/.
    required: true
  testFile:
    type: string
    description: Test source that holds the expected constants and prints
      GNFS_STRUCTURED_120BIT_V1 before asserting.
    default: tests/test_structured_filter_pipeline_120bit.cpp
*/
// Re-baseline the 120-bit gate on a branch. Nothing is compiled locally: the Nightly workflow
// runs the gate on GitHub, its log carries the observed identity, and a second run confirms the
// updated constants.

interface Rebaseline {
  /** 新旧期望值各是多少，英文，逐个常量列出；没有需要改的就写“no change”。 */
  summary: string;
  /** 修不了的原因；没有就写空字符串。 */
  blockers: string;
}

interface Finding {
  where: string;
  what: string;
  evidence: string;
  status: "verified" | "unconfirmed";
  severity: "low" | "medium" | "high";
}
interface WorkflowReport {
  conclusion: string;
  findings: Finding[];
  verified: string[];
  notCovered: string[];
}

interface GhRun {
  databaseId: number;
  headSha: string;
  status: string;
  conclusion: string;
  createdAt: string;
}
interface GhJob {
  name: string;
  status: string;
  conclusion: string;
  databaseId: number;
}

// AmendWorkflow does not carry the arguments over, so an amended run sees no arguments. There is
// no safe default for the branch, so the amended run fails fast and names the fix.
const rawBranch = args.branch;
if (typeof rawBranch !== "string" || rawBranch === "") {
  throw new Error("branch 必须是分支名。若这是用 AmendWorkflow 修订的运行：它不继承参数，请把本文件里的 FALLBACK_BRANCH 设成当前分支再修订。");
}
const branch = rawBranch;
const testFile = typeof args.testFile === "string" && args.testFile !== "" ? args.testFile : "tests/test_structured_filter_pipeline_120bit.cpp";
const GATE_JOB = "Structured 120-bit route gate";
const MAX_POLLS = 120;

const UPDATER =
  "你负责把一条固定身份门禁的期望值更新成 CI 上观测到的新值。这台机器不允许编译。" +
  "只改期望值常量，不要动测试逻辑，也不要改任何不是从日志里读到的数字。日志里找不到某个常量对应的观测值时不要猜，把它写进 blockers。";

// Static self-checks equivalent to the CI script jobs: Python, shell, and git inspection only.
const STATIC =
  "cd \"$1\" || exit 99\n" +
  "out=$(mktemp) || exit 99\n" +
  "trap 'rm -f \"$out\"' EXIT\n" +
  "fail=0\n" +
  "step() { name=\"$1\"; shift; if \"$@\" >\"$out\" 2>&1; then printf '  ok   %s\\n' \"$name\"; " +
  "else printf '  FAIL %s\\n' \"$name\"; tail -n 25 \"$out\" | sed 's/^/       /'; fail=1; fi; }\n" +
  "step 'release_contract self-test'        python3 scripts/release_contract.py self-test\n" +
  "step 'release_contract check-workflows'  python3 scripts/release_contract.py check-workflows\n" +
  "step 'stdout_contract'                   python3 scripts/check_stdout_contract.py\n" +
  "step 'harness'                           python3 scripts/check_harness.py\n" +
  "step 'policy self-test'                  python3 scripts/check_distributed_sieve_policy.py --self-test\n" +
  "step 'markdown links'                    python3 scripts/ci/check_markdown_links.py\n" +
  "[ -f scripts/ci/check_markdown_links.py ] && step 'link checker self-test' python3 scripts/ci/check_markdown_links.py --self-test\n" +
  "[ -f scripts/ci/check_env_flag_docs.py ] && step 'env flag docs' python3 scripts/ci/check_env_flag_docs.py\n" +
  "step 'test catalog'                      zsh ./scripts/test.sh list\n" +
  "exit $fail\n";

// A network command that times out must count as that step failing, not as the whole run failing.
async function gh(argv: string[], timeoutMs = 300_000, tail?: number): Promise<WorldRunResult> {
  try {
    return await world.run("gh", argv, tail === undefined ? { timeoutMs } : { timeoutMs, output: { tail } });
  } catch (error) {
    return { exitCode: 1, stdout: "", stderr: String(error) };
  }
}

async function gateJob(runId: string): Promise<GhJob | undefined> {
  const view = await gh(["run", "view", runId, "--json", "jobs"]);
  if (view.exitCode !== 0) return undefined;
  // gh wraps the list in an object: { "jobs": [ ... ] }. A job of a called workflow is
  // reported under a prefixed name, such as
  // "Reusable release qualification / Structured 120-bit route gate", so match the suffix.
  const parsed = JSON.parse(view.stdout) as { jobs: GhJob[] };
  return parsed.jobs.find((job) => job.name.endsWith(GATE_JOB));
}

// Start Nightly on the branch for the current head, or rerun the existing dispatch for that head.
// Returns the run id, or "" with the reason in the second element.
async function startNightly(head: string): Promise<{ id: string; note: string }> {
  const listed = await gh([
    "run", "list", "--workflow", "nightly.yml", "--branch", branch, "--event", "workflow_dispatch",
    "--limit", "10", "--json", "databaseId,headSha,status,conclusion,createdAt",
  ]);
  if (listed.exitCode !== 0) return { id: "", note: `读不到 Nightly 的运行记录：${listed.stderr.trim()}` };
  const runs = (JSON.parse(listed.stdout) as GhRun[])
    .filter((run) => run.headSha === head)
    .sort((a, b) => b.createdAt.localeCompare(a.createdAt));
  // Never start a second run for the same head: a finished one is what there is to read, and a
  // running one is what there is to wait for.
  const existing = runs[0];
  if (existing !== undefined) {
    return {
      id: String(existing.databaseId),
      note: existing.status === "completed" ? "already finished" : "already running",
    };
  }
  const trigger = await gh(["workflow", "run", "nightly.yml", "--ref", branch]);
  if (trigger.exitCode !== 0) return { id: "", note: `触发失败：${trigger.stderr.trim()}` };
  for (let poll = 0; poll < 20; poll++) {
    await world.run("sleep", ["15"]);
    const again = await gh([
      "run", "list", "--workflow", "nightly.yml", "--branch", branch, "--event", "workflow_dispatch",
      "--limit", "10", "--json", "databaseId,headSha,status,conclusion,createdAt",
    ]);
    if (again.exitCode !== 0) continue;
    const found = (JSON.parse(again.stdout) as GhRun[]).filter((run) => run.headSha === head);
    if (found.length > 0) {
      found.sort((a, b) => b.createdAt.localeCompare(a.createdAt));
      return { id: String(found[0].databaseId), note: "triggered" };
    }
  }
  return { id: "", note: "触发之后没有等到运行出现" };
}

// Wait until the 120-bit gate job settles. Returns its conclusion, or "" when it never settled.
async function waitGate(runId: string): Promise<string> {
  for (let poll = 0; poll < MAX_POLLS; poll++) {
    const job = await gateJob(runId);
    if (job !== undefined && job.status === "completed") return job.conclusion;
    await world.run("sleep", ["45"]);
  }
  return "";
}

phase("找到分支的工作树并确认它与远端一致");
const root = (await world.run("pwd")).stdout.trim();
const listing = await world.run("git", ["worktree", "list", "--porcelain"]);
let wt = "";
for (const block of listing.stdout.split("\n\n").map((text) => text.trim()).filter((text) => text !== "")) {
  if (!block.includes(`branch refs/heads/${branch}`)) continue;
  const line = block.split("\n").find((text) => text.startsWith("worktree "));
  if (line !== undefined) wt = line.slice("worktree ".length).trim();
}
if (wt === "") throw new Error(`分支 ${branch} 没有工作树`);
const pushed = await world.run("git", ["-C", wt, "rev-parse", "HEAD", `origin/${branch}`]);
const [head, remoteHead] = pushed.stdout.trim().split("\n");
if (pushed.exitCode !== 0 || head !== remoteHead) {
  throw new Error(`分支 ${branch} 的本地提交 ${head.slice(0, 8)} 与远端 ${remoteHead.slice(0, 8)} 不一致，先推送再重定基线`);
}
const clean = await world.run("git", ["-C", wt, "status", "--porcelain"]);
if (clean.stdout.trim() !== "") throw new Error(`工作树 ${wt} 里有未提交的改动`);

phase("在分支上跑第一轮 Nightly 取实际值");
const first = await startNightly(head);
if (first.id === "") throw new Error(first.note);
log(`Nightly 运行 ${first.id} 已在 ${branch} 上启动（${first.note}），等 120 bit 作业结束`);
const firstConclusion = await waitGate(first.id);
if (firstConclusion === "") throw new Error(`运行 ${first.id} 的 120 bit 作业在等待上限内没有结束`);

// Reassigned once the first run has failed and the constants have been updated.
let findings: Finding[] = [];
let verified: string[] = [];
let notCovered: string[] = [];
let conclusion = "";

if (firstConclusion === "success") {
  conclusion = `分支 ${branch} 上的 120 bit 门禁在当前提交上已经通过，不需要重定基线（运行 ${first.id}）。`;
  verified = [`Nightly 运行 ${first.id} 的「${GATE_JOB}」作业成功`];
} else {
  phase("从失败日志里取实际值并更新期望常量");
  // The gate job has settled but the workflow has not, and `gh run view --log-failed`
  // returns nothing until the whole run completes. Read the job's own log through the API;
  // the flag is needed because CI logs carry terminal escape sequences.
  const gate = await gateJob(first.id);
  if (gate === undefined) throw new Error(`运行 ${first.id} 里找不到「${GATE_JOB}」作业`);
  const failedLog = await gh(
    ["api", "--allow-escape-sequences", `repos/{owner}/{repo}/actions/jobs/${gate.databaseId}/logs`],
    300_000,
    40_000,
  );
  const updater = agent("基线更新者", UPDATER);
  let work = await updater.ask<Rebaseline>(
    `硬性约束：本机不允许编译，不要运行构建、测试或会触发编译的命令。只在工作树 ${wt} 里改动（分支 ${branch}）。\n` +
    `- 搜索和读取只限当前仓库。不要 force-push，不要合并，不要推送：推送由脚本做。代码注释和提交标题用英文。\n\n` +
    `${testFile} 把 120 bit 门禁的矩阵与归约身份写死成期望常量，并在这份文件里打印一行 GNFS_STRUCTURED_120BIT_V1 status=observed 的观测值。` +
    `分支上的改动让这些身份变了，CI 上第一轮运行失败。失败日志的末尾如下：\n\n${failedLog.stdout}${failedLog.stderr}\n\n` +
    "请从日志里读出每个观测值，与文件里的期望常量逐个对照，只更新不一致的那些。" +
    "日志里应当还有一条 status=fail 的错误行，写明是哪一组身份不符；如果日志里没有观测值那一行，说明测试没有在断言之前打印观测值，把它写进 blockers。" +
    "改完提交（提交标题用 test(relation): re-baseline the 120-bit identity ...），并确认 git diff --check 和 git status --porcelain 都没有输出。",
  );
  if (work.blockers !== "") {
    throw new Error(`期望常量没有更新：${work.blockers}`);
  }
  const checks = await world.run("sh", ["-c", STATIC, "sh", wt], { timeoutMs: 1_200_000, output: { tail: 16_384 } });
  if (checks.exitCode !== 0) throw new Error(`更新之后静态自检没有通过：\n${checks.stdout}`);
  const push = await world.run("git", ["-C", wt, "push", "origin", `HEAD:refs/heads/${branch}`, "--quiet"], { timeoutMs: 900_000 });
  if (push.exitCode !== 0) throw new Error(`推送失败：${push.stderr.trim()}`);

  phase("再跑一轮 Nightly 确认新基线");
  const newHead = (await world.run("git", ["-C", wt, "rev-parse", "HEAD"])).stdout.trim();
  const second = await startNightly(newHead);
  if (second.id === "") throw new Error(second.note);
  log(`期望常量已更新并推送，Nightly 运行 ${second.id} 在新提交上启动，等 120 bit 作业结束`);
  const secondConclusion = await waitGate(second.id);
  findings = [
    {
      where: `${wt}（${branch}）`,
      what: `120 bit 门禁的期望常量已按第一轮运行的观测值更新：${work.summary}`,
      evidence: `第一轮运行 ${first.id} 失败（${firstConclusion}），第二轮运行 ${second.id} 的结论是 ${secondConclusion === "" ? "没有结束" : secondConclusion}。`,
      status: secondConclusion === "success" ? "verified" : "unconfirmed",
      severity: "medium",
    },
  ];
  verified = [`分支通过了 git diff --check、干净的工作树，以及静态自检`];
  notCovered = [
    "Nightly 的另外两个作业（Release thorough 回归、50 位有界路线对比）没有等，它们的结果要在 GitHub 上另看",
    "拉取请求自己的检查由推送触发，结果另看",
  ];
  if (secondConclusion === "success") {
    conclusion = `分支 ${branch} 的 120 bit 基线已重定并在第二轮 Nightly（运行 ${second.id}）上通过。${work.summary}`;
  } else {
    conclusion = `期望常量已更新并推送，但第二轮 Nightly（运行 ${second.id}）的 120 bit 作业${secondConclusion === "" ? "没有在等待上限内结束" : `结论是 ${secondConclusion}`}，基线还没有确认。${work.summary}`;
  }
}

const result: WorkflowReport = { conclusion, findings, verified, notCovered };
return result;
