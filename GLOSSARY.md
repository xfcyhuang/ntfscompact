# ntfscompact

NTFS 文件 WOF 压缩的命令行工具：按指定算法压缩文件，已被指定算法压缩的跳过，被占用的先释放占用再压缩。

## Language

**指定算法（Target Algorithm）**:
用户在命令行选定的压缩算法，四种之一：XPRESS4K、XPRESS8K、XPRESS16K、LZX。
_Avoid_: 预定算法、目标格式

**重压（Recompress）**:
文件现有压缩算法与指定算法不同时，先解除现有压缩，再以指定算法重新压缩。
_Avoid_: 转换、二次压缩

**跳过（Skip）**:
文件已以指定算法压缩（或不满足压缩条件）时，不做任何修改，仅记录。
_Avoid_: 忽略、放过

**强制卸载（Forced Release）**:
文件被占用时逐级释放占用以获得独占访问的操作：Restart Manager 优雅关、Restart Manager 强制关、句柄强关；可选 --dismount 卷卸载极端级。全部失败则跳过。
_Avoid_: 强杀、解锁

**解压（Uncompress）**:
解除文件压缩，恢复常规存储形态。
_Avoid_: 还原、释放压缩

**无收益（Not Beneficial）**:
内核判定压缩该文件得不到容量收益而拒绝压缩，文件保持原样。计为跳过，不是错误。
_Avoid_: 压缩失败、不可压错误
