// ntfscompact 入口：命令行分发 + 管线接线（工单 T1 / #2 与 T2 / #3）
// 只读边界：本工单管线只扫描、检测、报告，不做任何修改（压缩/解压/占用处理
// 是 T3/T4/T5 的接缝——在 RunPipeline 内按 Plan::Action 分发处接入）。
#include "cli.h"
#include "pipeline.h"

#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    using namespace ntfscompact;

    const ParseResult r = ParseArgs(std::vector<std::wstring>(argv + 1, argv + argc));

    if (r.want_help) {
        WriteUtf8(stdout, UsageText());
        return 0;
    }
    if (r.want_version) {
        WriteUtf8(stdout, L"ntfscompact " + std::wstring(kVersion) + L"\n");
        return 0;
    }
    if (!r.ok) {
        WriteUtf8(stderr, L"ntfscompact: " + r.error + L"\n\n");
        WriteUtf8(stderr, UsageText());
        return 2;  // 用法错误固定退出码 2
    }

    // 生产检测器：wofutil.dll 动态加载，失败明确报错终止（无法判 WOF 现状会误压）
    std::wstring load_error;
    RealDetector detector(&load_error);
    if (!load_error.empty()) {
        WriteUtf8(stderr, L"ntfscompact: " + load_error + L"\n");
        return 1;
    }
    return RunPipeline(r.options, detector, stdout, stderr);
}
