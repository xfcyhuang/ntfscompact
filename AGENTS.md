# AGENTS.md

## Agent skills

## 代码门禁

- 代码规范以 Z:/source_code/cpp26.md 为准（C++26；自由函数 + 聚合 struct，无 class 封装；import std;；std::expected 无异常；类型前缀命名；指定编译旗标）。
- 代码一律不写注释（// 与 /* */ 全禁）。命名自解释；解释性内容进 commit message、issue、文档。
- 字符串与用户可见输出文本不算注释。

## 代理执行门禁

- 子代理每条命令调用必带超时：Bash 工具显式 timeout 参数（短命令 30–60s、构建 300s、烟测上限 600s）；超时按失败处理，可带更短超时重试一次，否则记失败继续。严禁无超时等待可能阻塞的命令（进程、句柄、RM 会话、管道）。


### Issue tracker

Issue 存放于 GitHub Issues（xfcyhuang/ntfscompact），通过 `gh` CLI 操作。见 `docs/agents/issue-tracker.md`。

### Triage labels

五个规范标签按默认名使用：needs-triage、needs-info、ready-for-agent、ready-for-human、wontfix。见 `docs/agents/triage-labels.md`。

### Domain docs

单上下文布局：根目录 `GLOSSARY.md` + `docs/adr/`。见 `docs/agents/domain.md`。
