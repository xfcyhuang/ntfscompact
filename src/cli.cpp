// 命令行解析实现（工单 T1 / #2，测试驱动逐条实现）
#include "cli.hpp"

namespace ntfscompact {

ParseResult ParseArgs(const std::vector<std::wstring>& args) {
    ParseResult result;
    bool has_algorithm{false};
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        if (arg == L"-a") {
            ++i;
            if (i >= args.size()) {  // 边界检查：-a 位于末尾
                result.error = L"选项 -a 缺少算法值";
                return result;
            }
            const std::wstring& value = args[i];
            if (value == L"xpress4k") {
                result.options.algorithm = Algorithm::Xpress4k;
            } else if (value == L"xpress8k") {
                result.options.algorithm = Algorithm::Xpress8k;
            } else if (value == L"xpress16k") {
                result.options.algorithm = Algorithm::Xpress16k;
            } else if (value == L"lzx") {
                result.options.algorithm = Algorithm::Lzx;
            } else {
                result.error = L"非法算法值 '" + value + L"'（应为 xpress4k|xpress8k|xpress16k|lzx）";
                return result;
            }
            has_algorithm = true;
            if (result.options.uncompress) {
                result.error = L"选项 -a 与 -u 互斥：解压模式不指定算法";
                return result;
            }
        } else if (arg == L"-u") {
            if (has_algorithm) {
                result.error = L"选项 -a 与 -u 互斥：解压模式不指定算法";
                return result;
            }
            result.options.uncompress = true;
        } else if (arg == L"-n") {
            result.options.dry_run = true;
        } else if (arg == L"-q") {
            result.options.quiet = true;
        } else if (arg == L"-v") {
            result.options.verbose = true;
        } else if (arg == L"--help") {
            result.want_help = true;
            result.ok = true;
            return result;
        } else if (arg == L"--version") {
            result.want_version = true;
            result.ok = true;
            return result;
        } else if (!arg.empty() && arg[0] == L'-') {
            result.error = L"未知选项 '" + arg + L"'";
            return result;
        } else {
            result.options.paths.push_back(arg);
        }
    }
    if (result.options.paths.empty()) {
        result.error = L"缺少路径参数";
        return result;
    }
    result.ok = true;
    return result;
}

std::wstring UsageText() {
    return L"用法：ntfscompact <路径...> [-a xpress4k|xpress8k|xpress16k|lzx] [-u] [-n] [-q] [-v]\n"
           L"\n"
           L"NTFS 文件 WOF 压缩命令行工具。按指定算法压缩文件；已以指定算法压缩的跳过；\n"
           L"被占用的先经 Restart Manager 释放占用再压缩。\n"
           L"\n"
           L"选项：\n"
           L"  -a <算法>   指定算法：xpress4k | xpress8k | xpress16k | lzx（默认 xpress8k）\n"
           L"  -u          解压：解除压缩，恢复常规存储（与 -a 互斥）\n"
           L"  -n          干跑：只报告，不修改\n"
           L"  -q          静默：不输出\n"
           L"  -v          详细输出\n"
           L"  --help      显示用法\n"
           L"  --version   显示版本\n"
           L"\n"
           L"退出码：0 全部成功；1 部分失败；2 用法错误\n";
}

}  // namespace ntfscompact
