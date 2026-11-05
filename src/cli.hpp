// ntfscompact 命令行解析层（工单 T1 / #2）
// 仅负责解析：不压缩、不遍历文件系统，无任何 I/O 副作用。
#pragma once

import std;

namespace ntfscompact {

// 版本号（发布时同步更新）
inline constexpr wchar_t kVersion[] = L"0.1.0";

// 指定算法（Target Algorithm）：WOF 四算法之一，见 docs/adr/0001
enum class Algorithm {
    Xpress4k,   // xpress4k
    Xpress8k,   // xpress8k（默认）
    Xpress16k,  // xpress16k
    Lzx,        // lzx
};

// 命令行选项：main 解析后交后续管线（压缩/遍历）工单消费
struct Options {
    std::vector<std::wstring> paths;   // 待处理路径列表
    Algorithm algorithm = Algorithm::Xpress8k;  // 指定算法，默认 XPRESS8K
    bool uncompress = false;           // -u：解压
    bool dry_run = false;              // -n：干跑，只报不动
    bool quiet = false;                // -q：静默
    bool verbose = false;              // -v：详细输出
};

// 解析结果
struct ParseResult {
    bool want_help = false;    // --help：打印用法后退出 0
    bool want_version = false; // --version：打印版本后退出 0
    bool ok = false;           // false 表示用法错误，调用方应打印用法并以退出码 2 结束
    std::wstring error;        // 用法错误说明（ok 为 true 时为空）
    Options options;           // 解析出的选项（用法错误时可能部分填充，不应使用）
};

// 解析命令行参数（不含程序名 argv[0]）。纯函数。
ParseResult ParseArgs(const std::vector<std::wstring>& args);

// 用法说明文本（--help 与用法错误共用）
std::wstring UsageText();

}  // namespace ntfscompact
