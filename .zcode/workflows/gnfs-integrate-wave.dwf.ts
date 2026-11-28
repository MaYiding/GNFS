/* zcode-workflow
description: Merge several open GNFS pull request branches into one integration
  branch and open a single integration pull request.
whenToUse: When a wave's pull requests are open and should land together, so CI
  validates the combined result once. Takes the pull request numbers and a wave
  label; merge the integration pull request, not the individual ones.
args:
  prs:
    type: string
    description: JSON array of open pull request numbers to integrate, passed as a
      string, for example "[429, 430]".
    required: true
  wave:
    type: string
    description: Label for the integration branch, for example 20261128-5.
    required: true
*/
// Integration wave for GNFS: merge the branches of several open pull requests into one
// integration branch and open a single integration pull request, so CI validates the combined
// result once. Everything reaches main through that pull request; nothing is merged locally.

interface Merge {
  /** 冲突怎么解决的，英文，写进集成拉取请求的正文；没有冲突就写“无冲突”。 */
  resolution: string;
  /** 解决不了的原因；没有就写空字符串。 */
  blockers: string;
}

interface Fix {
  /** 你改了什么，英文，写进集成拉取请求的正文。 */
  summary: string;
  /** 修不了的原因；没有就写空字符串。 */
  blockers: string;
}

interface Finding {
  /** 集成分支的工作树。 */
  where: string;
  /** 结果。 */
  what: string;
  /** 依据。 */
  evidence: string;
  /** 集成拉取请求确实开出且静态自检全部通过为 verified，否则为 unconfirmed。 */
  status: "verified" | "unconfirmed";
  /** 重要程度。 */
  severity: "low" | "medium" | "high";
}

interface PrInfo {
  number: number;
  title: string;
  state: string;
  headRefName: string;
}

// Arguments arrive as strings, so a JSON array has to be parsed here.
const rawPrs = typeof args.prs === "string" ? JSON.parse(args.prs) : args.prs;
if (!Array.isArray(rawPrs)) throw new Error("prs 必须是拉取请求编号数组");
const prNumbers = rawPrs.map((value) => Number(value)).filter((value) => Number.isInteger(value) && value > 0);
if (prNumbers.length === 0) throw new Error("prs 里没有有效的拉取请求编号");
const wave = typeof args.wave === "string" ? args.wave : "wip";
const GATE_ROUNDS = 3;

const RESOLVER =
  "你负责解决 GNFS 仓库里合并分支时出现的冲突。原则是保留双方各自的意图：一边是 main 上后来的修正，另一边是拉取请求要做的改动。" +
  "看不懂某一边为什么这么改就去读提交说明和周边代码；实在判断不了的，不要猜，把原因写进 blockers。";

const FIXER =
  "你是 GNFS 仓库的资深 C++ 工程师。这台机器不允许编译，你只能靠读代码和几项静态检查把关。" +
  "只修让静态检查通过所必需的地方，不要顺手改别的。做不到就把原因写进 blockers。";

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
  // The two checks below arrive with the pull requests of a wave; run each one only when
  // its script is on the integration branch, so a skipped pull request does not fail the gate.
  "grep -q -- '--self-test' scripts/ci/check_markdown_links.py && step 'link checker self-test' python3 scripts/ci/check_markdown_links.py --self-test\n" +
  "[ -f scripts/ci/check_env_flag_docs.py ] && step 'env flag docs' python3 scripts/ci/check_env_flag_docs.py\n" +
  "[ -f scripts/ci/format_changed.sh ] && [ -x /tmp/gnfs-ops/cf18/bin/clang-format ] && "
    "step 'clang-format 18 (changed files)' sh scripts/ci/format_changed.sh origin/main /tmp/gnfs-ops/cf18/bin/clang-format\n" +
  "[ -f scripts/ci/check_test_asserts.py ] && step 'test asserts' python3 scripts/ci/check_test_asserts.py\n" +
  "step 'test catalog'                      zsh ./scripts/test.sh list\n" +
  "exit $fail\n";

const RULES =
  "硬性约束：\n" +
  "- 本机不允许编译。不要运行构建、测试或任何会触发编译的命令：./scripts/test.sh 除 list 以外的子命令、cmake、make、ninja、编译器、scripts/test-app.sh 都不行。\n" +
  "- 搜索和读取只限当前仓库，不要以 ~ 或 ~/Library 为搜索根。\n" +
  "- 不要 force-push，不要合并到 main，不要推送：推送由脚本做。\n" +
  "- 代码注释和提交标题用英文。\n" +
  "- 脚本会在你之后统一跑全套静态自检，你自己只跑与这次改动直接相关的那几项。";

async function gate(wt: string): Promise<{ ok: boolean; detail: string }> {
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

phase("准备集成分支的工作树");
const root = (await world.run("pwd")).stdout.trim();
const fetched = await world.run("git", ["fetch", "origin", "main", "--quiet"]);
if (fetched.exitCode !== 0) throw new Error("拿不到 main 的最新提交，无法开始。");
const branch = `codex/integrate-${wave}`;
const wt = `${root}/.worktrees/integrate-${wave}`;
const add = await world.run("git", ["worktree", "add", "-b", branch, wt, "origin/main"]);
if (add.exitCode !== 0) {
  throw new Error(`建不了集成分支 ${branch} 的工作树（同名分支或工作树可能还在）：${add.stderr.trim()}`);
}

phase("把各拉取请求的分支逐个合进来");
let merged: { info: PrInfo; resolution: string }[] = [];
let skipped: string[] = [];
for (const number of prNumbers) {
  // A network command that times out must count as this pull request being skipped,
  // not as the whole run failing.
  let view: WorldRunResult;
  try {
    view = await world.run(
      "gh", ["pr", "view", String(number), "--json", "number,title,state,headRefName"],
      { timeoutMs: 300_000 },
    );
  } catch (error) {
    view = { exitCode: 1, stdout: "", stderr: String(error) };
  }
  if (view.exitCode !== 0) {
    skipped = [...skipped, `#${number}：读不到，${view.stderr.trim()}`];
    continue;
  }
  const info = JSON.parse(view.stdout) as PrInfo;
  if (info.state !== "OPEN") {
    skipped = [...skipped, `#${number}「${info.title}」不是打开状态（${info.state}），没有合入`];
    continue;
  }
  let fetchBranch: WorldRunResult;
  try {
    fetchBranch = await world.run("git", ["fetch", "origin", info.headRefName, "--quiet"], { timeoutMs: 600_000 });
  } catch (error) {
    fetchBranch = { exitCode: 1, stdout: "", stderr: String(error) };
  }
  if (fetchBranch.exitCode !== 0) {
    skipped = [...skipped, `#${number}「${info.title}」的分支 ${info.headRefName} 拉不到`];
    continue;
  }
  const merge = await world.run("git", ["-C", wt, "merge", "--no-ff", "--no-edit", `origin/${info.headRefName}`]);
  if (merge.exitCode === 0) {
    merged = [...merged, { info, resolution: "无冲突" }];
    log(`#${number}「${info.title}」已合入，没有冲突`);
    continue;
  }
  const resolver = agent(`冲突解决者·PR${number}`, RESOLVER);
  const outcome = await resolver.ask<Merge>(
    `${RULES}\n` +
      `- 只在集成分支的工作树 ${wt} 里操作，这里正处在一次没合完的合并中。\n\n` +
      `正在把拉取请求 #${number}「${info.title}」（分支 ${info.headRefName}）合进集成分支，git 报告了冲突：\n` +
      `${merge.stdout}${merge.stderr}\n\n` +
      "请查看冲突的文件（git -C 查看状态和两边的改动），逐个解决，然后 git add 并用 git commit 完成这次合并，" +
      "不要改写提交信息以外的历史。resolution 用英文写你怎么解决的，它会进集成拉取请求的正文。" +
      "判断不了的冲突不要猜，把它写进 blockers，脚本会撤销这次合并。",
  );
  const status = await world.run("git", ["-C", wt, "status", "--porcelain"]);
  if (outcome.blockers !== "" || status.stdout.trim() !== "") {
    await world.run("git", ["-C", wt, "merge", "--abort"]);
    skipped = [...skipped,
      `#${number}「${info.title}」的冲突没有解决，已撤销这次合并：${outcome.blockers === "" ? "合并没有提交完整" : outcome.blockers}`,
    ];
    continue;
  }
  merged = [...merged, { info, resolution: outcome.resolution }];
  log(`#${number}「${info.title}」已合入，冲突已解决`);
}
if (merged.length === 0) throw new Error(`没有任何拉取请求合进集成分支：${skipped.join("；")}`);

phase("集成结果做静态自检");
const fixer = agent("集成修复者", FIXER);
let fix: Fix = { summary: "", blockers: "" };
let passed = false;
let gateOutput = "";
for (let round = 0; round < GATE_ROUNDS; round++) {
  const result = await gate(wt);
  if (result.ok) {
    passed = true;
    gateOutput = result.detail;
    break;
  }
  if (fix.blockers !== "") break;
  fix = await fixer.ask<Fix>(
    `${RULES}\n` +
      `- 只在集成分支的工作树 ${wt} 里操作（分支 ${branch}）。它已经合入了 ${merged.length} 个拉取请求的分支。\n\n` +
      "脚本跑了提交前的检查，有失败，输出如下：\n" +
      result.detail +
      "\n\n请判断这是合并本身造成的问题（比如两边各自能过、合在一起就冲突的指纹或清单），还是某个拉取请求本来就有的问题。" +
      "只修前者：让两边各自的意图都保留。修完提交，并确认 git status --porcelain 没有输出。" +
      "如果是后者或者修不了，把原因写进 blockers。summary 用英文写你改了什么，它会进集成拉取请求的正文。",
  );
}

phase("推送并开集成拉取请求");
let findings: Finding[] = [];
let conclusion = "";
if (!passed) {
  conclusion = `集成分支没有通过静态自检，没有开拉取请求：${fix.blockers === "" ? "静态自检没有通过" : fix.blockers}`;
  findings = [
    ...findings,
    {
      where: wt,
      what: `集成 ${merged.length} 个拉取请求的分支没有通过静态自检`,
      evidence: fix.blockers === "" ? "见自检输出" : fix.blockers,
      status: "unconfirmed",
      severity: "medium",
    },
  ];
} else {
  let push: WorldRunResult;
  try {
    push = await world.run("git", ["-C", wt, "push", "-u", "origin", branch, "--quiet"], { timeoutMs: 900_000 });
  } catch (error) {
    push = { exitCode: 1, stdout: "", stderr: String(error) };
  }
  if (push.exitCode !== 0) {
    conclusion = `集成分支推送失败：${push.stderr.trim()}`;
    findings = [...findings, { where: wt, what: "集成分支推送失败", evidence: push.stderr, status: "unconfirmed", severity: "medium" }];
  } else {
    const body = [
      "Integrates the following pull requests on one branch so that CI validates the combined result:",
      "",
      ...merged.map((entry) => `- #${entry.info.number} ${entry.info.title}`),
      "",
      "## Conflict Resolution",
      "",
      ...(merged.filter((entry) => entry.resolution !== "无冲突").length === 0
        ? ["No conflicts."]
        : merged.filter((entry) => entry.resolution !== "无冲突").map((entry) => `- #${entry.info.number}: ${entry.resolution}`)),
      ...(fix.summary === "" ? [] : ["", "## Integration Fixes", "", fix.summary]),
      "",
      "## Validation",
      "",
      "The static self-checks that mirror the CI script and Markdown link jobs pass locally. Builds and tests run on this pull request's checks.",
    ].join("\n");
    let pr: WorldRunResult;
    try {
      pr = await world.run("gh", [
        "pr", "create", "--base", "main", "--head", branch,
        "--title", `Integrate the pull requests for wave ${wave}`, "--body", body,
      ]);
    } catch (error) {
      pr = { exitCode: 1, stdout: "", stderr: String(error) };
    }
    const url =
      pr.exitCode === 0
        ? (pr.stdout.split("\n").map((line) => line.trim()).filter((line) => line.includes("/pull/")).pop() ?? "")
        : "";
    if (url === "") {
      conclusion = `集成拉取请求没有开成：${pr.stderr.trim() || pr.stdout.trim()}`;
      findings = [...findings, { where: wt, what: "集成拉取请求没有开成", evidence: pr.stderr, status: "unconfirmed", severity: "medium" }];
    } else {
      conclusion =
        `集成拉取请求 ${url} 已开，合入了 ${merged.length} 个拉取请求的分支` +
        (skipped.length > 0 ? `，另有 ${skipped.length} 个没有合入` : "") +
        "。它开门约十分钟后出早期结果，约四十五分钟全部出来；通过后合并它，GitHub 会把其中的各个拉取请求标记为已合并。";
      findings = [
        ...findings,
        {
          where: `${wt}（${branch}）`,
          what: `集成拉取请求已开，合入了 ${merged.map((entry) => `#${entry.info.number}`).join("、")}`,
          evidence: `静态自检全部通过：\n${gateOutput}\n拉取请求：${url}`,
          status: "verified",
          severity: "medium",
        },
      ];
    }
  }
}

return {
  conclusion,
  findings,
  verified: passed ? ["集成分支通过了 git diff --check、干净的工作树，以及与 CI 等价的十二项静态自检"] : [],
  notCovered: [
    "构建和测试：本机不允许编译，合并后的结果由集成拉取请求的 GitHub 检查验证",
    "格式：本机 clang-format 是 23 版、CI 是 18 版，格式由 CI 的检查把关",
    ...skipped,
  ],
};
