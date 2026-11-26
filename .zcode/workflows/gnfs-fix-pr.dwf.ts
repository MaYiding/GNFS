/* zcode-workflow
description: Fix a GNFS pull request whose GitHub checks failed, using the
  failed-run logs and the branch worktree, then push.
whenToUse: When a pull request shows failed checks and the fix needs the CI
  logs. Takes the pull request number; finds or creates the worktree for its
  branch under .worktrees/.
args:
  pr:
    type: number
    description: Number of the pull request to fix.
    required: true
*/
// Fix a GNFS pull request: read the logs of the failed checks, fix the branch in its worktree,
// and push once the static self-checks pass. Nothing is compiled locally; build and test
// results come only from the GitHub CI logs.

interface Fix {
  /** 每个失败的原因和你的处理，中文，两到四句；属于不稳定测试或外部故障的要说明判断依据。 */
  summary: string;
  /** 修不了的原因；没有就写空字符串。 */
  blockers: string;
}

const rawPr = args.pr;
const prNumber = Number(rawPr);
if (!Number.isInteger(prNumber) || prNumber <= 0) throw new Error("pr 必须是拉取请求编号");
const GATE_ROUNDS = 3;

const FIXER =
  "你是 GNFS 仓库的资深 C++ 工程师。这台机器不允许编译，你只能读 CI 日志和代码来定位问题。" +
  "修真正的病因，不要为了让检查变绿去放宽断言或跳过测试。遇到做不到的事，直接说明并写进 blockers。";

// Static self-checks equivalent to the CI jobs "zsh/bash syntax" and "Local Markdown links":
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
  "step 'test catalog'                      zsh ./scripts/test.sh list\n" +
  "exit $fail\n";

const RULES =
  "硬性约束：\n" +
  "- 本机不允许编译。不要运行构建、测试或任何会触发编译的命令：./scripts/test.sh 除 list 以外的子命令、cmake、make、ninja、编译器、scripts/test-app.sh 都不行。\n" +
  "- 可以运行的静态检查（都与 CI 等价、不触发编译）：git diff --check；zsh ./scripts/test.sh list；python3 scripts/check_harness.py；" +
  "python3 scripts/check_distributed_sieve_policy.py --self-test；python3 scripts/check_stdout_contract.py；python3 scripts/ci/check_markdown_links.py。\n" +
  "- 搜索和读取只限当前仓库，不要以 ~ 或 ~/Library 为搜索根。\n" +
  "- 不要 force-push，不要合并，不要改动 main。推送由脚本做。\n" +
  "- 代码注释和提交标题用英文；提交标题遵循仓库历史的写法（fix(scope): ...、test(scope): ...、ci(...)）。\n" +
  "- 脚本会在你之后统一跑全套静态自检，你自己只跑与这次改动直接相关的那几项。";

interface PrInfo {
  title: string;
  state: string;
  headRefName: string;
  headRefOid: string;
  url: string;
}

async function gate(wt: string): Promise<{ ok: boolean; detail: string }> {
  const whitespace = await world.run("git", ["-C", wt, "diff", "--check", "origin/main...HEAD"]);
  if (whitespace.exitCode !== 0) {
    return { ok: false, detail: `git diff --check 报告了空白问题：\n${whitespace.stdout}${whitespace.stderr}` };
  }
  const status = await world.run("git", ["-C", wt, "status", "--porcelain"]);
  if (status.stdout.trim() !== "") {
    return { ok: false, detail: `工作树里有未提交的改动：\n${status.stdout}` };
  }
  const checks = await world.run("sh", ["-c", STATIC, "sh", wt], {
    timeoutMs: 1_200_000,
    output: { tail: 16_384 },
  });
  if (checks.exitCode !== 0) return { ok: false, detail: `静态自检有失败：\n${checks.stdout}` };
  return { ok: true, detail: checks.stdout };
}

phase("找到这个拉取请求的分支和失败的检查");
const root = (await world.run("pwd")).stdout.trim();
const view = await world.run("gh", [
  "pr", "view", String(prNumber), "--json", "title,state,headRefName,headRefOid,url",
]);
if (view.exitCode !== 0) throw new Error(`读不到拉取请求：${view.stderr.trim()}`);
const info = JSON.parse(view.stdout) as PrInfo;
if (info.state !== "OPEN") throw new Error(`拉取请求不是打开状态（${info.state}），不需要修。`);
const checks = await world.run("gh", ["pr", "checks", String(prNumber), "--json", "name,bucket"]);
if (checks.stdout.trim() === "") throw new Error(`读不到检查结果：${checks.stderr.trim()}`);
const failedNames = (JSON.parse(checks.stdout) as { name: string; bucket: string }[])
  .filter((check) => check.bucket === "fail")
  .map((check) => check.name);
if (failedNames.length === 0) throw new Error("这个拉取请求没有失败的检查，不需要修。");

// Find the failed runs by branch and current head: a check link comes in two shapes, so the run id cannot be read from the link.
const runsOut = await world.run("gh", [
  "run", "list", "--branch", info.headRefName, "--status", "failure", "--limit", "30",
  "--json", "databaseId,workflowName,headSha",
]);
const runs = JSON.parse(runsOut.stdout) as { databaseId: number; workflowName: string; headSha: string }[];
const onHead = runs.filter((run) => run.headSha === info.headRefOid);
const excerpts: string[] = [];
for (const run of (onHead.length > 0 ? onHead : runs).slice(0, 5)) {
  const log = await world.run("gh", ["run", "view", String(run.databaseId), "--log-failed"], {
    timeoutMs: 300_000,
    output: { tail: 12_000 },
  });
  const text = log.stdout === "" ? log.stderr : log.stdout;
  if (text.trim() !== "") excerpts.push(`【${run.workflowName}】\n${text}`);
}
if (excerpts.length === 0) {
  excerpts.push(`按分支 ${info.headRefName} 没有取到失败的运行日志。失败的检查项是：${failedNames.join("、")}。`);
}

const listing = await world.run("git", ["worktree", "list", "--porcelain"]);
let wt = "";
for (const block of listing.stdout.split("\n\n").map((text) => text.trim()).filter((text) => text !== "")) {
  if (!block.includes(`branch refs/heads/${info.headRefName}`)) continue;
  const line = block.split("\n").find((text) => text.startsWith("worktree "));
  if (line !== undefined) wt = line.slice("worktree ".length).trim();
}
if (wt === "") {
  const slug = info.headRefName.replace(/^codex\//, "").replace(/[^A-Za-z0-9._-]+/g, "-");
  wt = `${root}/.worktrees/${slug}`;
  const fetchBranch = await world.run("git", ["fetch", "origin", info.headRefName, "--quiet"]);
  if (fetchBranch.exitCode !== 0) throw new Error(`拉不到分支 ${info.headRefName}：${fetchBranch.stderr.trim()}`);
  const add = await world.run("git", ["worktree", "add", wt, info.headRefName]);
  if (add.exitCode !== 0) throw new Error(`建不了工作树：${add.stderr.trim()}`);
} else {
  await world.run("git", ["-C", wt, "pull", "--ff-only", "--quiet"]);
}
log(`${info.title} 有 ${failedNames.length} 项检查失败，工作树在 ${wt}`);

phase("修复并做静态自检");
const fixer = agent("修复者", FIXER);
let fix = await fixer.ask<Fix>(
  `${RULES}\n` +
    `- 只在工作树 ${wt} 里改动（分支 ${info.headRefName}）。\n\n` +
    `拉取请求 #${prNumber}「${info.title}」在 GitHub 上有 ${failedNames.length} 项检查失败：${failedNames.join("、")}。日志末尾如下：\n\n` +
    excerpts.join("\n\n") +
    "\n\n请判断每个失败的原因：是这次改动引入的，还是不稳定测试或外部依赖故障（比如 Launchpad 返回 503）。" +
    "前者修掉；后者把判断依据写进 summary，不要动代码。" +
    "如果是格式检查失败，按 clang-format 18 的规则修：本机装的是 23 版，它的输出不能直接当准。" +
    "改完提交，并确认 git diff --check 和 git status --porcelain 都没有输出。修不了就把原因写进 blockers。",
);
let passed = false;
let gateOutput = "";
for (let round = 0; round < GATE_ROUNDS && fix.blockers === ""; round++) {
  const result = await gate(wt);
  if (result.ok) {
    passed = true;
    gateOutput = result.detail;
    break;
  }
  fix = await fixer.ask<Fix>(
    "脚本在工作树里跑了与 CI 等价的静态自检，有失败，输出如下：\n" +
      result.detail +
      "\n\n请修到全部通过：有空白问题就改掉，有未提交的改动就提交或者撤销，静态自检失败就修到通过。" +
      "修完提交，并确认 git status --porcelain 没有输出。修不了就把原因写进 blockers。",
  );
}

phase("推送修复");
let pushed = false;
if (passed && fix.blockers === "") {
  const push = await world.run("git", ["-C", wt, "push", "origin", `HEAD:refs/heads/${info.headRefName}`, "--quiet"]);
  pushed = push.exitCode === 0;
  if (!pushed) fix = { ...fix, blockers: `推送失败：${push.stderr.trim()}` };
}

const ok = passed && fix.blockers === "" && pushed;
return {
  conclusion: ok
    ? `拉取请求 #${prNumber} 的修复已推送：${fix.summary} 新一轮检查开门约十分钟后出早期结果，约四十五分钟全部出来。`
    : `拉取请求 #${prNumber} 没有修成：${fix.blockers === "" ? "静态自检没有通过" : fix.blockers}`,
  findings: [
    {
      where: `${wt}（${info.headRefName}）`,
      what: `${info.title}：${ok ? "修复已推送" : "没有修成"}`,
      evidence: ok ? `静态自检全部通过：\n${gateOutput}` : fix.blockers,
      status: ok ? "verified" : "unconfirmed",
      severity: "medium",
    },
  ],
  verified: ok ? ["推送前通过了 git diff --check、干净的工作树，以及与 CI 等价的十二项静态自检", "分支已推送到远端"] : [],
  notCovered: [
    "构建和测试：本机不允许编译，修复是否有效要等这个拉取请求的新一轮 GitHub 检查",
    "格式：本机 clang-format 是 23 版、CI 是 18 版，格式由 CI 的检查把关",
  ],
};
