# 强制卸载走 Restart Manager 两级，失败跳过而非杀进程（已被 0003 取代）

文件被占用时用 Restart Manager 释放：先优雅关闭（RmShutdown），失败再 RmForceShutdown 强制关；两级都失败则跳过该文件并报告，绝不升级为 TerminateProcess 杀进程或 NtQuerySystemInformation 强关句柄。理由：csrss、杀毒软件等内核组件的占用杀不死也不该杀；强关句柄（Process Explorer 路线）对系统文件句柄可致系统不稳；未保存数据丢失风险需有边界。未来读者看到「用户要求强制卸载但代码拒绝杀进程」时应知道这是刻意为之（决策自 grilling Q4/Q11）。
