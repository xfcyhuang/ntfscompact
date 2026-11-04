// ntfscompact 入口：仅做命令行分发（工单 T1 / #2）
// 结构缝：解析成功后 r.options 交后续压缩/遍历管线工单消费，本工单不实现。
#include "cli.h"

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

namespace {

// 统一按 UTF-8 输出宽字符串：Git Bash 管道、重定向文件与 Windows Terminal 下中文一致
void Write(std::FILE* stream, const std::wstring& text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            utf8.data(), bytes, nullptr, nullptr);
    }
    std::fwrite(utf8.data(), 1, utf8.size(), stream);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace ntfscompact;

    const ParseResult r = ParseArgs(std::vector<std::wstring>(argv + 1, argv + argc));

    if (r.want_help) {
        Write(stdout, UsageText());
        return 0;
    }
    if (r.want_version) {
        Write(stdout, L"ntfscompact " + std::wstring(kVersion) + L"\n");
        return 0;
    }
    if (!r.ok) {
        Write(stderr, L"ntfscompact: " + r.error + L"\n\n");
        Write(stderr, UsageText());
        return 2;  // 用法错误固定退出码 2
    }

    // 结构缝：T1 只到解析层。后续工单在此接管，例如：
    //   return RunPipeline(r.options);  // 压缩/遍历/占用处理管线（工单 #3 起）
    (void)r;
    return 0;
}
