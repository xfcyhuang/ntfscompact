# ntfscompact

NTFS 文件 WOF 压缩命令行工具：按指定算法（XPRESS4K / XPRESS8K / XPRESS16K / LZX）压缩文件或目录，已处于指定算法的文件自动跳过，磁盘上的 LZNT1 老文件先解除再重压为指定算法；被占用的文件直接卸载所在卷后重试（需管理员权限）；亦支持解除压缩恢复常规存储。

对应系统功能与 `compact /exe` 同源（Windows Overlay Filter，WOF），文件压缩后保持透明读写，读取时自动解压，无需任何挂载/解包步骤。

## 要求

- Windows 10 1709+（WOF 四算法与 `WofSetFileDataLocation` XPRESS16K 支持的下限）
- NTFS 本地卷（WOF 压缩不支持 ReFS/FAT32/exFAT 与 SMB/网络卷）
- 普通压缩/解压/干跑无需管理员权限；仅「占用处理（卷卸载）」需要（见下）

## 构建

CMake + Ninja + Clang（llvm-mingw 或 LLVM-Clang 21+，需支持 C++26 `import std;`）：

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=D:/software/LLVM/bin/c++.exe
cmake --build build
ctest --test-dir build --timeout 60
```

产物为 `build/ntfscompact.exe`。测试集中含真实压缩用例（在本机临时目录执行），不要在网络卷上运行 ctest。

## 用法

```
ntfscompact <路径...> [-a xpress4k|xpress8k|xpress16k|lzx] [-u] [-n] [-q] [-v]
```

| 选项 | 说明 |
|------|------|
| `-a <算法>` | 指定压缩算法，默认 `xpress8k`；与 `-u` 互斥 |
| `-u` | 解压：解除压缩，恢复常规存储 |
| `-n` | 干跑：只报告将执行的动作，不修改任何文件 |
| `-q` | 静默：仅输出末尾汇总一行（失败信息仍输出到 stderr） |
| `-v` | 详细输出：含逐文件行与卷卸载处置记录 |
| `--help` / `--version` | 显示用法 / 版本 |

路径可为文件、目录（递归）或通配符，可多个。输出逐文件行走 stdout，失败行走 stderr。

示例：

```sh
ntfscompact D:\data                        # 默认 xpress8k 压缩整个目录
ntfscompact -a lzx D:\iso D:\vm\disk.vhdx  # LZX 最高压缩比
ntfscompact -a xpress16k -n D:\src         # 干跑查看计划
ntfscompact -a xpress4k -q D:\logs         # 静默，仅看汇总
ntfscompact -v -a lzx D:\db\dump.bak       # 详细模式，含占用处置记录
ntfscompact -u D:\data                     # 解除压缩恢复常规存储
```

## 算法

| 算法 | 特点 |
|------|------|
| `xpress4k` | 最快，压缩比最低，适合小文件密集场景 |
| `xpress8k` | **默认**。速度与压缩比均衡 |
| `xpress16k` | 压缩比更高，速度略降，适合大文件 |
| `lzx` | 最高压缩比，压缩速度最慢，适合冷数据/归档 |

## 决策表

对每个扫描到的文件：

| 文件现状 | 压缩模式动作 | 解压模式动作 |
|----------|--------------|--------------|
| 未压缩（且 >4KB、非黑名单） | 压缩为指定算法 | 跳过（未压缩） |
| 已是指定算法 | 跳过（已是指定算法） | 解压 |
| 其他 WOF 算法 | 重压：先解除再压为指定算法 | 解压 |
| LZNT1（传统压缩） | 重压：先解除再压为指定算法 | 解压 |
| 内核判定无收益 | 跳过（无收益，保持原样，不计错误） | 解压 |
| ≤4KB | 跳过（文件大小 ≤4KB） | 跳过 |
| WIM/Cloud backing、EFS 加密、非常规 reparse point、pagefile/hiberfil/swapfile、WinSxS 内文件 | 跳过（黑名单） | 跳过（WIM/Cloud 不解除） |

只读属性文件：压缩期间临时清除只读，完成后恢复。

## 汇总与退出码

每次运行末尾输出一行汇总（`-q` 模式下为唯一输出）：

```
共 5 个文件：压缩 2、重压 2、解压 0、跳过 1；压缩/重压 961920 → 28672 字节，节省 97%
```

「原始逻辑字节 → 磁盘占用字节」按 `GetCompressedFileSizeW` 统计实际落盘占用，干跑与实跑同一口径（干跑统计的是压缩前的现状占用）。

| 退出码 | 含义 |
|--------|------|
| 0 | 全部成功（跳过不算失败） |
| 1 | 部分失败：检测失败、执行错误、复核失败（压缩后未达到指定算法）等 |
| 2 | 用法错误：非法参数、`-a` 与 `-u` 同现、缺路径等 |

## 被占用文件的处理：直接卷卸载

文件被其他进程占用导致无法压缩时，工具对目标文件所在卷执行 `FSCTL_DISMOUNT_VOLUME`，使全卷打开句柄失效，随后重试一次；执行器报「错误码 32」时同样先卸载再重试。卷随后由系统在下次访问时自动重新挂载。

**风险声明（务必阅读）：**

- 卷卸载影响的是**整卷所有进程的打开句柄**，不限于占用目标文件的进程；其他进程可能遇到句柄失效、未保存数据丢失或 I/O 错误。请勿对系统卷或含关键工作状态的卷使用。
- 该操作**需要管理员权限**；非提权运行时占用文件直接计跳过并说明原因，绝不尝试杀进程或强关句柄。
- 卷卸载失败不重试，该文件计跳过。
- SMB/网络卷、无盘符的卷不支持此机制（也无法探测到卷根），占用时一律跳过。

## 权限说明

- 普通压缩、解压、干跑：**无需管理员**，对目标文件有读写权限即可。
- 仅「占用文件触发卷卸载」需要管理员；未提权时遇到占用只跳过，其余文件照常处理。

## 开发

- 代码规范：C++26，`import std;`，自由函数 + 聚合 struct，`std::expected` 错误处理，零注释零警告（`-Wall -Wextra -Wpedantic`）。
- 领域术语见 [GLOSSARY.md](GLOSSARY.md)；架构决策见 [docs/adr/](docs/adr/)。
- 测试：`ctest --test-dir build --timeout 60`（含 `e2e_flow` 端到端：混合目录压缩→复核→干跑全跳过→解压→复核）。
