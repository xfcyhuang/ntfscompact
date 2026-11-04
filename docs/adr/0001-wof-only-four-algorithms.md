# 仅支持 WOF 四算法，不含 LZNT1

工具只支持 WOF（Windows Overlay Filter）路径的四种算法：XPRESS4K、XPRESS8K、XPRESS16K、LZX，不提供 LZNT1（传统 FSCTL_SET_COMPRESSION）压缩。理由：WOF 算法全面优于 LZNT1（压缩比更高、支持到 30GB+ 无单文件上限约束），compact.exe 的现代路径也是它，维护两套压缩路径徒增复杂度。但磁盘上存在的 LZNT1 老文件不算「已压缩」：遇 LZNT1 文件先解除再按指定 WOF 算法重压，保证终态是指定算法（决策自 grilling Q1/Q3/Q19）。
