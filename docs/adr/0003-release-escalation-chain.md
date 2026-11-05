# 占用释放四级链与 --dismount 极端开关

文件被占用时按四级链释放，逐级升级：① Restart Manager 优雅关（RmShutdown(0)，协作进程经写打开触发的 oplock 中断配合，已覆盖「讲理」进程场景，不单列 oplock 层）；② Restart Manager 强制关（RmForceShutdown）；③ 句柄强关——NtQuerySystemInformation 枚举系统句柄表，定位目标文件句柄，DuplicateHandle(DUPLICATE_CLOSE_SOURCE) 强关；系统/受保护进程以 PROCESS_DUP_HANDLE 打不开即自然止步；④ 仍占用则跳过报告，绝不 TerminateProcess。目标用户进程可能因句柄强关崩溃，属已授权风险，README 声明。另设 --dismount 极端开关：四级链尽后经 FSCTL_DISMOUNT_VOLUME 卸载目标卷使全卷句柄失效后重试，默认关，需管理员权限与显式确认，仅限受控场景使用。本 ADR 取代 0002 的「两级+跳过」结论（0002 前两级保留为链前段）。
