#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "cli.h"
#include "detect.h"
#include "executor.hpp"
#include "uncompress.hpp"

namespace ntfscompact {

struct FileReport {
  std::wstring display_path;
  std::wstring str_long_path;
  Plan plan;
  bool has_state = false;
  CurrentState state;
  std::wstring error;
  bool b_executed = false;
  ExecuteResult er_exec;
};

struct PipelineResult {
  std::vector<FileReport> reports;
  int failures = 0;
};

inline std::wstring FormatReportLine(const FileReport& r, const Options& options) {
  if (!r.error.empty()) {
    return L"[失败] " + r.display_path + L"：" + r.error;
  }
  if (r.b_executed && r.er_exec.e_outcome == ExecuteResult::Outcome::kError) {
    return L"[失败] " + r.display_path + L"：" + r.er_exec.str_message;
  }
  const wchar_t* ptr_tag = nullptr;
  switch (r.plan.action) {
    case Plan::Action::Compress: ptr_tag = L"压缩"; break;
    case Plan::Action::Recompress: ptr_tag = L"重压"; break;
    case Plan::Action::Uncompress: ptr_tag = L"解压"; break;
    case Plan::Action::Skip: ptr_tag = L"跳过"; break;
  }
  std::wstring line = L"[";
  line += ptr_tag;
  line += L"] ";
  line += r.display_path;
  line += L"：";
  if (r.b_executed) {
    switch (r.er_exec.e_outcome) {
      case ExecuteResult::Outcome::kSuccess:
        line += r.plan.action == Plan::Action::Compress
                    ? L"已压缩为指定算法 "
                    : L"已重压为指定算法 ";
        line += AlgorithmName(options.algorithm);
        break;
      case ExecuteResult::Outcome::kSkipped:
        line += r.er_exec.str_message;
        break;
      case ExecuteResult::Outcome::kError:
        break;
    }
    return line;
  }
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
      line += r.plan.reason;
      break;
  }
  return line;
}

inline std::wstring FormatSummary(const PipelineResult& result) {
  int compress = 0, recompress = 0, uncompress = 0, skip = 0;
  for (const FileReport& r : result.reports) {
    if (!r.error.empty()) continue;
    if (r.b_executed) {
      switch (r.er_exec.e_outcome) {
        case ExecuteResult::Outcome::kSuccess:
          switch (r.plan.action) {
            case Plan::Action::Compress: ++compress; break;
            case Plan::Action::Recompress: ++recompress; break;
            default: break;
          }
          break;
        case ExecuteResult::Outcome::kSkipped: ++skip; break;
        case ExecuteResult::Outcome::kError: break;
      }
      continue;
    }
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

PipelineResult RunScan(const Options& options, IDetector& detector);

void WriteUtf8(std::FILE* stream, const std::wstring& text);

int RunPipeline(const Options& options, IDetector& detector,
                const ExecuteFn& fn_execute, const UncompressFn& fn_uncompress,
                std::FILE* out, std::FILE* err);

}
