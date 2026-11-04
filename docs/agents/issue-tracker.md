# Issue 追踪器：GitHub

本仓库的 issue 和规格说明存放于 GitHub Issues。所有操作使用 `gh` CLI。

## 约定

- **创建 issue**：`gh issue create --title "..." --body "..."`。多行正文用 heredoc。
- **读取 issue**：`gh issue view <number> --json number,title,body,labels,comments`。
- **列出 issue**：`gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'`，按需加 `--label` 和 `--state` 过滤。
- **将 issue 设为父 issue 的子 issue**：`gh issue create --parent <parent> ...`，或事后 `gh issue edit <parent> --add-sub-issue <child>`（`gh` 2.94+）。旧版 `gh`：`gh api --method POST repos/<owner>/<repo>/issues/<parent>/sub_issues -F sub_issue_id=<child-db-id>`（数据库 id，见下方 **阻塞关系**）。无法使用子 issue 时，在子 issue 正文顶部写 `Part of #<parent>`。
- **阻塞关系**：GitHub 的**原生 issue 依赖**是规范的、UI 可见的表示方式。添加边：`gh issue create --blocked-by <n>,<n> ...` 或 `gh issue edit <child> --add-blocked-by <blocker>`（`gh` 2.94+）。旧版 `gh`：`gh api --method POST repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`，其中 `<blocker-db-id>` 是阻塞方的数字**数据库 id**（`gh api repos/<owner>/<repo>/issues/<n> --jq .id`，不是 `#number` 也不是 `node_id`）。依赖不可用时，退而在子 issue 正文顶部写 `Blocked by: #<n>, #<n>`。所有阻塞方关闭后，票才算解除阻塞。
- **统计未关闭的阻塞方**：`gh api repos/<owner>/<repo>/issues/<n> --jq .issue_dependencies_summary.blocked_by`（仅统计未关闭的阻塞方；0 表示可开工）。`gh issue view <n> --json blockedBy` 也会列出已关闭的阻塞方，需过滤：`--jq '[.blockedBy.nodes[] | select(.state == "OPEN")] | length'`。
- **评论 issue**：`gh issue comment <number> --body "..."`
- **添加 / 移除标签**：`gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **关闭**：先用 `gh issue comment <number> --body "..."` 说明原因，再 `gh issue close <number>`。对已关闭的 issue，`gh issue close --comment` 会丢弃评论但仍退出码 0。

从 `git remote -v` 推断仓库；在克隆目录内运行时 `gh` 自动完成。

只向用户自己的仓库提 issue。绝不向 `mattpocock/skills`（这些技能的来源仓库）提 issue，除非用户拥有它。在 fork 中，`gh` 可能默认指向上游父仓库，因此每个会话首次 `gh issue create` 前，用 `gh repo view --json nameWithOwner` 检查目标；若不是用户的仓库，先与用户确认。

## Pull request 作为 triage 面

**PRs as a request surface：no。**_（若本仓库将外部 PR 视为功能请求，改为 `yes`；`/triage` 读取此标志。）_

设为 `yes` 时，PR 与 issue 使用相同的标签和状态，用 `gh pr` 等价命令：

- **读取 PR**：`gh pr view <number>`，然后 `gh pr view <number> --comments`（非 TTY 下 `--comments` 只输出评论，不含正文），以及 `gh pr diff <number>` 看 diff。
- **列出待 triage 的外部 PR**：`gh api --paginate 'repos/{owner}/{repo}/pulls?state=open' --jq '.[] | select(.author_association | IN("OWNER","MEMBER","COLLABORATOR") | not) | {number, title, author: .user.login, author_association, labels: [.labels[].name]}'`。
- **评论 / 打标签 / 关闭**：`gh pr comment`、`gh pr edit --add-label`/`--remove-label`、`gh pr close`。

GitHub 的 issue 和 PR 共用同一编号空间，裸 `#42` 可能是任一：用 `gh pr view 42` 解析，失败则 `gh issue view 42`。

## 当技能说「发布到 issue 追踪器」

创建一个 GitHub issue。

## 当技能说「获取相关工单」

按上方**读取 issue**的方式读取。

## Wayfinder 操作

供 `/wayfinder` 使用。**地图（map）**是单个 issue，工单为其**子** issue。

- **地图**：单个 issue，标签 `wayfinder:map`，正文含 Notes / Decisions-so-far / Fog。`gh issue create --label wayfinder:map`。
- **子工单**：作为 GitHub 子 issue 链接到地图（见**将 issue 设为父 issue 的子 issue**）。子 issue 不可用时，把子项加入地图正文的任务列表，并在子 issue 正文顶部写 `Part of #<map>`。标签：`wayfinder:<type>`（`research`/`prototype`/`grilling`/`task`）。被认领后，工单指派给主导开发者。
- **阻塞关系**：见**约定**下的**阻塞关系**。
- **前沿查询（Frontier query）**：列出地图的未关闭子项（`gh issue list --state open`，限定于地图的子 issue / 任务列表），去掉有未关闭阻塞方（见**统计未关闭的阻塞方**，或 `Blocked by` 行中有未关闭 issue）或有指派人的；按地图顺序取第一个。
- **认领**：`gh issue edit <n> --add-assignee @me`，这是会话的第一次写操作。
- **解决**：`gh issue comment <n> --body "<answer>"`，然后 `gh issue close <n>`，最后向地图的 Decisions-so-far 追加上下文指针（gist + 链接）。
