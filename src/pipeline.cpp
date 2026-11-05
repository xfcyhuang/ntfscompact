// 只读管线实现（工单 #3）：扫描 → 检测 → 决策 → 报告输出。
// 不做任何修改操作——真实压缩/解压/占用处理是后续工单（T3/T4/T5）。
#include "pipeline.h"

#include "scan.h"

#include <string>
#include <windows.h>

namespace ntfscompact {

void WriteUtf8(std::FILE* stream, const std::wstring& text) {
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

PipelineResult RunScan(const Options& options, IDetector& detector) {
    PipelineResult result;
    ScanOutcome scan;
    ScanPaths(options.paths, scan);

    for (const ScanSkip& s : scan.skipped) {
        FileReport r;
        r.display_path = s.display_path;
        r.has_state = false;
        r.plan.action = Plan::Action::Skip;
        r.plan.reason = s.reason;
        result.reports.push_back(r);
    }
    for (const ScanItem& item : scan.items) {
        FileReport r;
        r.display_path = item.display_path;
        r.long_path = item.long_path;
        const DetectionResult d = detector.Detect(item.long_path);
        if (!d.error.empty()) {
            r.error = d.error;
            ++result.failures;
            result.reports.push_back(r);
            continue;
        }
        r.has_state = true;
        r.state = d.state;
        r.plan = DecideAction(d.state, options);
        result.reports.push_back(r);
    }
    for (const ScanError& e : scan.errors) {
        FileReport r;
        r.display_path = e.display_path;
        r.error = e.message;
        ++result.failures;
        result.reports.push_back(r);
    }
    return result;
}

int RunPipeline(const Options& options, IDetector& detector,
                const UncompressFn& fn_uncompress, std::FILE* out, std::FILE* err) {
    PipelineResult result = RunScan(options, detector);
    if (options.uncompress && !options.dry_run) {
        for (FileReport& r : result.reports) {
            if (!r.error.empty() || r.plan.action != Plan::Action::Uncompress) continue;
            if (!fn_uncompress) {
                r.error = L"解压器未接线";
                ++result.failures;
                continue;
            }
            const std::expected<void, std::wstring> exp{
                fn_uncompress(r.long_path)};
            if (!exp.has_value()) {
                r.error = exp.error();
                ++result.failures;
            }
        }
    }
    if (!options.quiet) {
        if (options.dry_run || options.verbose) {
            for (const FileReport& r : result.reports) {
                if (!r.error.empty()) {
                    WriteUtf8(err, FormatReportLine(r, options) + L"\n");
                } else {
                    WriteUtf8(out, FormatReportLine(r, options) + L"\n");
                }
            }
        }
        WriteUtf8(out, FormatSummary(result) + L"\n");
    }
    return result.failures > 0 ? 1 : 0;
}

}  // namespace ntfscompact
