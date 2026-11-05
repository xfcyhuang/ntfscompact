// 只读管线：扫描 → 检测 → 决策 → 干跑报告（工单 #3）
// RunScan 组装报告（检测经 IDetector 注入），格式化为纯函数，RunPipeline 做输出粘合。
// 真实压缩/解压/占用处理是后续工单（T3/T4/T5），本层只读。
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "cli.h"
#include "detect.h"
#include "uncompress.hpp"

namespace ntfscompact {

// 单文件报告：干跑输出的数据源
struct FileReport {
    std::wstring display_path;  // 显示路径（不含 \\?\ 前缀）
    std::wstring long_path;
    Plan plan;                  // 将采取的动作（含跳过原因）
    bool has_state = false;     // false：遍历期即跳过（黑名单/≤4KB 等），无现状
    CurrentState state;         // has_state 时有效：现状压缩形态
    std::wstring error;         // 非空：检测/访问失败，plan 无效，计失败
};

// 管线汇总结果
struct PipelineResult {
    std::vector<FileReport> reports;  // 每文件一条（含跳过项）
    int failures = 0;                 // 无法访问/检测失败的路径数（部分失败 → 退出码 1）
};

// 干跑单行（无换行）。措辞对齐 GLOSSARY：压缩/重压/解压/跳过/指定算法。
inline std::wstring FormatReportLine(const FileReport& r, const Options& options) {
    if (!r.error.empty()) {
        return L"[失败] " + r.display_path + L"：" + r.error;
    }
    const wchar_t* tag = nullptr;
    switch (r.plan.action) {
        case Plan::Action::Compress: tag = L"压缩"; break;
        case Plan::Action::Recompress: tag = L"重压"; break;
        case Plan::Action::Uncompress: tag = L"解压"; break;
        case Plan::Action::Skip: tag = L"跳过"; break;
    }
    std::wstring line = L"[";
    line += tag;
    line += L"] ";
    line += r.display_path;
    line += L"：";
    switch (r.plan.action) {
        case Plan::Action::Compress:
            line += L"现状 " + DescribeState(r.state) + L"，将压缩为指定算法 " +
                    AlgorithmName(options.algorithm);
            break;
        case Plan::Action::Recompress:
            line += L"现状 " + DescribeState(r.state) + L"，将重压为指定算法 " +
                    AlgorithmName(options.algorithm);
            break;
        case Plan::Action::Uncompress:
            line += L"现状 " + DescribeState(r.state) + L"，将解压";
            break;
        case Plan::Action::Skip:
            line += r.plan.reason;  // 跳过原因已含「已是指定算法 x」等完整说明
            break;
    }
    return line;
}

// 汇总行（无换行）。失败项计入总数与「失败」，不计入动作计数。
inline std::wstring FormatSummary(const PipelineResult& result) {
    int compress = 0, recompress = 0, uncompress = 0, skip = 0;
    for (const FileReport& r : result.reports) {
        if (!r.error.empty()) continue;
        switch (r.plan.action) {
            case Plan::Action::Compress: ++compress; break;
            case Plan::Action::Recompress: ++recompress; break;
            case Plan::Action::Uncompress: ++uncompress; break;
            case Plan::Action::Skip: ++skip; break;
        }
    }
    std::wstring line = L"共 " + std::to_wstring(result.reports.size()) +
                        L" 个文件：压缩 " + std::to_wstring(compress) +
                        L"、重压 " + std::to_wstring(recompress) +
                        L"、解压 " + std::to_wstring(uncompress) +
                        L"、跳过 " + std::to_wstring(skip);
    if (result.failures > 0) {
        line += L"；失败 " + std::to_wstring(result.failures);
    }
    return line;
}

// 扫描 + 检测 + 决策：options.paths 展开（文件/目录/通配符/长路径），过滤黑名单
// 与 ≤4KB，经 detector 检测现状并决策。不做任何修改。
PipelineResult RunScan(const Options& options, IDetector& detector);

// 宽字符串按 UTF-8 写流（管道/重定向下中文一致）
void WriteUtf8(std::FILE* stream, const std::wstring& text);

// 管线粘合：扫描 + 按选项输出（干跑/详细打每文件行，普通模式打汇总）。
// 返回进程退出码：0 全部成功，1 部分失败。
int RunPipeline(const Options& options, IDetector& detector,
                const UncompressFn& fn_uncompress, std::FILE* out, std::FILE* err);

}  // namespace ntfscompact
