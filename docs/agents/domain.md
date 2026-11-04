# Domain 文档

工程技能在探索本代码库时，应如何消费领域文档。

## 探索前先读这些

- 仓库根目录的 **`GLOSSARY.md`**；若存在根目录 **`GLOSSARY-MAP.md`**，它指向每个上下文一份的 `GLOSSARY.md`，读取与主题相关的每一份。
- **`docs/adr/`**：阅读与你即将工作的区域相关的 ADR。多上下文仓库中，还要检查 `src/<context>/docs/adr/` 中的上下文级决策。

这些文件若不存在，**静默继续**。不提示缺失，不建议预先创建。`/domain-modeling` 技能（经 `/grill-with-docs` 和 `/improve-codebase-architecture` 到达）会在术语或决策实际确定时惰性创建它们。

## 文件结构

单上下文仓库（大多数仓库）：

```
/
├── GLOSSARY.md
├── docs/adr/
│   ├── 0001-event-sourced-orders.md
│   └── 0002-postgres-for-write-model.md
└── src/
```

多上下文仓库（根目录存在 `GLOSSARY-MAP.md` 时）：

```
/
├── GLOSSARY-MAP.md
├── docs/adr/                          ← 系统级决策
└── src/
    ├── ordering/
    │   ├── GLOSSARY.md
    │   └── docs/adr/                  ← 上下文级决策
    └── billing/
        ├── GLOSSARY.md
        └── docs/adr/
```

## 使用术语表的词汇

输出中命名领域概念时（issue 标题、重构提案、假设、测试名），使用 `GLOSSARY.md` 中定义的术语。不要漂移到术语表明确避免的同义词。

所需概念尚不在术语表中时，这是个信号：要么你在发明项目不用的语言（重新考虑），要么存在真实缺口（记下来给 `/domain-modeling`）。

## 标记 ADR 冲突

输出与现有 ADR 矛盾时，显式提出，而非静默覆盖：

> _与 ADR-0007（事件溯源订单）矛盾，但值得重开，因为……_
