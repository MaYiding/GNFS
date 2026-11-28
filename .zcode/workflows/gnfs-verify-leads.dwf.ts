/* zcode-workflow
description: Verify several GNFS correctness leads, each with one verifier that
  reads the code (optionally running exact pure-Python experiments) and one
  independent confirmer that saw nothing of the verification.
whenToUse: When a direction survey or a reading of the code leaves unconfirmed
  correctness leads whose consequences must be settled before anyone implements
  a fix. Write the leads into a JSON file, set experiments to "python" only when
  exact arithmetic can decide the question, then read the report. Nothing is
  compiled.
args:
  leadsFile:
    type: string
    description: 'Workspace-relative path to a JSON file: { context?: string,
      experiments?: "python" | "none", leads: [{ slug, title, ask }] }.'
    required: true
*/
// 核实若干条关于 GNFS 的线索。每条线索由一名核实员读码（可选做纯 Python 的精确数值实验），结论再交一名没看过过程的
// 复核员换人复核（复核员必须自己另写一份脚本）。全程不编译、不运行项目的任何测试。

// 结论再交一名没看过过程的复核员换人复核（复核员必须自己另写一份脚本）。全程不编译、不运行项目的任何测试。

interface Fact {
  /** 代码事实：仓库相对路径加行号，一句话说明那里写了什么。 */
  where: string;
  /** 那里写了什么。 */
  what: string;
}

interface Verification {
  /** 读码确认的事实，每条带 path:line。 */
  facts: Fact[];
  /** 后果：会算错因子，还是只是降低成功率或浪费算力；依据是什么。 */
  consequence: string;
  /** 会算出错误结果为 high；只是成功率下降或浪费为 medium；文档或可维护性为 low。 */
  severity: "low" | "medium" | "high";
  /** 建议的修法：要改哪些文件、怎么改、怎么验证；不值得修就说明原因。 */
  proposal: string;
  /** 做过的数值实验：脚本放在哪里、算了什么、结果是什么（给出具体数字）。 */
  experiments: string[];
  /** 推断（不是读码或实验直接确认）的部分，逐条列出。 */
  inferences: string[];
  /** 没能确认的点，以及需要什么才能确认（比如只能在 CI 上测的数字）。 */
  unknowns: string[];
}

interface Confirmation {
  /** 核实结论里的事实、数值结果与主要推断都成立为 true；有不成立或证据不足的为 false。 */
  confirmed: boolean;
  /** 主结论成立但细节要改为 revisions；主结论不成立为 refuted；全部成立为 none。 */
  verdict: "none" | "revisions" | "refuted";
  /** 哪些不成立、哪些证据不足、应该怎么改；全部成立就写“无”。 */
  corrections: string;
  /** 你自己核对时打开的文件与行号、自己写的脚本放在哪里、重新推导与重算的要点和数字。 */
  evidence: string;
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

// Numeric experiments are allowed only when the leads file asks for them.
const rawLeadsFile = args.leadsFile;
if (typeof rawLeadsFile !== "string" || rawLeadsFile === "") throw new Error("leadsFile 必须是线索文件的仓库相对路径");

const READ_ONLY =
  "- 仓库只读。不要在仓库里编辑、创建或删除任何文件；不要提交、推送或改动分支。" +
  "也不要读它的 .worktrees 目录（别的任务可能正在那里施工，读到的是半成品）。\n" +
  "- 本机不允许编译。不要运行构建、测试或任何会触发编译的命令：./scripts/test.sh 的任何子命令、cmake、make、ninja、任何编译器都不行；也不要导入或运行仓库里的 Python 脚本。\n";

const WITH_PYTHON =
  "- 可以做数值实验，但只能用纯 Python 的整数运算（python3 版本 3 以上，不装任何第三方包）。脚本只能写到 /tmp/gnfs-verify-<线索代号>/ 下面，用 python3 -I 运行。" +
  "需要精确算术时不要用浮点数近似。\n";

const WITHOUT_PYTHON =
  "- 结论只能来自读码：读文件、grep、git log、git show、git blame。\n";

const TAIL =
  "- 搜索只限当前仓库，不要以 ~ 或 ~/Library 为搜索根。\n" +
  "- 每条代码事实必须给出 path:line，并且是你亲自打开核对过的；没有亲自核对的说法放进 inferences。\n" +
  "- 结论里严格区分三类依据：读码确认的事实、数值实验的结果、推断。做不到或判断不了，直接写进 unknowns，不要猜。\n" +
  "- 用中文回答；代码标识符、公式里的符号保持原样。";

const VERIFIER =
  "你是数域筛实现方面的审查员，熟悉代数数论、筛法、线性代数与平方根各阶段的实现细节。" +
  "你的任务是凭代码（允许时再加上精确的数值实验）确认一条线索是否成立、后果是什么、该怎么修。你只读代码，不改代码。";

const CONFIRMER =
  "你是独立复核员。你没有参与前面的核实，只凭代码和你自己另做的数值实验复核别人的结论，专找不成立、证据不足、说过头的地方。你只读代码，不改代码。";

interface Lead {
  /** 路径安全的短标识，用在复核员的临时目录名里。 */
  slug: string;
  /** 一句话说明线索是什么。 */
  title: string;
  /** 给核实员的任务说明：线索、涉及位置的 path:line、要确认的各个问题。 */
  ask: string;
}

interface LeadsFile {
  /** 所有线索共用的背景，可以不给。 */
  context?: string;
  /** 允许纯 Python 数值实验就写 "python"，否则只读码。 */
  experiments?: "python" | "none";
  leads: Lead[];
}

artifact.table("leads", {
  title: "线索核实进度",
  key: "slug",
  columns: [
    { field: "where", label: "线索" },
    { field: "status", label: "复核" },
    { field: "severity", label: "严重程度" },
  ],
});

phase("核实员分头读码，允许时并做精确的数值实验");
const parsed = JSON.parse(await files.read(rawLeadsFile)) as LeadsFile;
if (!Array.isArray(parsed.leads) || parsed.leads.length === 0) throw new Error("线索文件里的 leads 必须是非空数组");
const leadList = parsed.leads;
const context = typeof parsed.context === "string" ? parsed.context : "";
const experiments = parsed.experiments === "python" ? "python" : "none";
const RULES = "硬性约束：\n" + READ_ONLY + (experiments === "python" ? WITH_PYTHON : WITHOUT_PYTHON) + TAIL;
for (const lead of leadList) {
  if (typeof lead.slug !== "string" || lead.slug === "" || typeof lead.title !== "string" || typeof lead.ask !== "string") {
    throw new Error("线索文件里每条线索都要有非空的 slug、title、ask");
  }
}
log(`共 ${leadList.length} 条线索；数值实验：${experiments === "python" ? "允许纯 Python" : "不允许，只读码"}`);
const verifications = await Promise.all(
  leadList.map(async (lead) => {
    const verifier = agent(`核实员·${lead.title}`, VERIFIER);
    const result = await verifier.ask<Verification>(`${RULES}\n\n${context === "" ? "" : `${context}\n`}${lead.ask}`);
    report({ slug: lead.slug, title: lead.title, where: lead.slug, status: "核实中", severity: result.severity }, "leads");
    return { lead, result };
  }),
);

phase("换人独立复核每份核实结论");
// Each confirmation follows its own verification; the only join is here, before the report.
const reviewed = await Promise.all(
  verifications.map(async ({ lead, result }) => {
    const confirmer = agent(`复核员·${lead.title}`, CONFIRMER);
    const confirmation = await confirmer.ask<Confirmation>(
      `${RULES}\n\n${context === "" ? "" : `${context}\n`}你没有参与核实。下面是另一位审查员对线索「${lead.title}」的结论（JSON）：\n` +
        `${JSON.stringify(result)}\n\n` +
        "请只凭代码和你自己的实验复核：facts 里的每一条都打开文件核对行号与内容；consequence、proposal 与 inferences 里的推断自己重新推一遍，包括数学推导。" +
        (experiments === "python"
          ? "数值实验必须自己另写一份脚本（放在 /tmp/gnfs-verify/<线索代号>-confirm/ 下），在写完并跑出自己的数字之前不要去读核实员的脚本，然后再对照；两边数字不一致要查清原因。"
          : "事实要自己打开文件核对；全称断言（“没有调用点”“唯一”之类）要先弄清分发机制再下结论。") +
        "verdict 的取法：主结论成立且细节也无误为 none；主结论成立但有细节要改为 revisions；主结论不成立为 refuted。confirmed 只在 none 或 revisions 时为 true。" +
        "corrections 里逐条写出哪条不成立、哪条证据不足、哪里说过头、应该怎么改。不要因为对方写得详细就默认成立：确认一条需要你自己看到证据。",
    );
    report(
      {
        slug: lead.slug,
        title: lead.title,
        where: lead.slug,
        status: confirmation.confirmed ? (confirmation.verdict === "none" ? "已复核" : "成立，细节有修订") : "复核后不成立",
        severity: result.severity,
      },
      "leads",
    );
    return { lead, result, confirmation };
  }),
);

const findings: Finding[] = reviewed.map(({ lead, result, confirmation }): Finding => ({
  where: lead.slug,
  what: `${lead.title}：${result.consequence}`,
  evidence:
    `${result.facts.map((fact) => `${fact.where} ${fact.what}`).join("；")}\n` +
    (result.experiments.length === 0 ? "" : `数值实验：${result.experiments.join("；")}\n`) +
    `复核意见（${confirmation.verdict}）：${confirmation.corrections}`,
  status: confirmation.confirmed ? "verified" : "unconfirmed",
  severity: result.severity,
}));

const lines: string[] = [`# ${leadList.length} 条线索的核实结论`, ""];
for (const { lead, result, confirmation } of reviewed) {
  lines.push(`## ${lead.title}`, "", `**复核：** ${confirmation.confirmed ? (confirmation.verdict === "none" ? "独立复核成立" : "主结论成立，细节按复核意见修订") : "复核后不成立"}`, "");
  lines.push("**读码确认的事实**", "");
  for (const fact of result.facts) lines.push(`- \`${fact.where}\`：${fact.what}`);
  if (result.experiments.length > 0) {
    lines.push("", "**数值实验**", "");
    for (const item of result.experiments) lines.push(`- ${item}`);
  }
  lines.push("", "**后果**", "", result.consequence, "", "**建议**", "", result.proposal, "");
  if (result.inferences.length > 0) {
    lines.push("**推断（不是读码或实验直接确认）**", "");
    for (const item of result.inferences) lines.push(`- ${item}`);
    lines.push("");
  }
  lines.push("**没能确认的**", "");
  for (const item of result.unknowns) lines.push(`- ${item}`);
  lines.push("", "**复核意见**", "", confirmation.corrections, "", `复核依据：${confirmation.evidence}`, "");
}
await artifact.markdown("report", lines.join("\n"), {
  title: "线索核实报告",
  primary: true,
});

const result: WorkflowReport = {
  conclusion:
    `${reviewed.length} 条线索各由一名核实员读码并做精确数值实验、一名复核员换人复核：` +
    `${reviewed.filter((entry) => entry.confirmation.confirmed).length} 条的主结论经复核成立` +
    `（其中 ${reviewed.filter((entry) => entry.confirmation.verdict === "revisions").length} 条细节有修订）。` +
    (experiments === "python" ? "全部是读码与纯 Python 精确算术的结论" : "全部是读码结论") +
    "，没有编译或运行项目的测试；标为不成立的条目不能当作事实。",
  findings,
  verified: reviewed.map(
    ({ lead, confirmation }) => `${lead.title}：${confirmation.evidence}`,
  ),
  notCovered: reviewed.flatMap(({ lead, result }) => result.unknowns.map((item) => `${lead.slug}：${item}`)),
};
return result;
