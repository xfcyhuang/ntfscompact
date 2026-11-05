#pragma once

import std;

#include "cli.hpp"
#include "detect.hpp"
#include "executor.hpp"
#include "uncompress.hpp"

namespace ntfscompact {

struct FileReport {
  std::wstring str_display_path{};
  std::wstring str_long_path{};
  Plan pl_plan{};
  bool b_has_state{false};
  CurrentState cs_state{};
  std::wstring str_error{};
  bool b_executed{false};
  ExecuteResult er_exec{};
};

struct PipelineResult {
  std::vector<FileReport> vec_reports{};
  int i4_failures{};
};

inline std::wstring format_report_line(const FileReport& fr_report,
                                       const Options& opt_options) {
  if (!fr_report.str_error.empty()) {
    return L"[失败] " + fr_report.str_display_path + L"：" + fr_report.str_error;
  }
  if (fr_report.b_executed &&
      fr_report.er_exec.e_outcome == ExecuteResult::Outcome::kError) {
    return L"[失败] " + fr_report.str_display_path + L"：" +
           fr_report.er_exec.str_message;
  }
  const wchar_t* ptr_tag{nullptr};
  switch (fr_report.pl_plan.e_action) {
    case Plan::Action::kCompress: ptr_tag = L"压缩"; break;
    case Plan::Action::kRecompress: ptr_tag = L"重压"; break;
    case Plan::Action::kUncompress: ptr_tag = L"解压"; break;
    case Plan::Action::kSkip: ptr_tag = L"跳过"; break;
  }
  std::wstring str_line{L"["};
  str_line += ptr_tag;
  str_line += L"] ";
  str_line += fr_report.str_display_path;
  str_line += L"：";
  if (fr_report.b_executed) {
    switch (fr_report.er_exec.e_outcome) {
      case ExecuteResult::Outcome::kSuccess:
        str_line += fr_report.pl_plan.e_action == Plan::Action::kCompress
                        ? L"已压缩为指定算法 "
                        : L"已重压为指定算法 ";
        str_line += algorithm_name(opt_options.e_algorithm);
        break;
      case ExecuteResult::Outcome::kSkipped:
        str_line += fr_report.er_exec.str_message;
        break;
      case ExecuteResult::Outcome::kError:
        break;
    }
    return str_line;
  }
  switch (fr_report.pl_plan.e_action) {
    case Plan::Action::kCompress:
      str_line += L"现状 " + describe_state(fr_report.cs_state) +
                  L"，将压缩为指定算法 " +
                  algorithm_name(opt_options.e_algorithm);
      break;
    case Plan::Action::kRecompress:
      str_line += L"现状 " + describe_state(fr_report.cs_state) +
                  L"，将重压为指定算法 " +
                  algorithm_name(opt_options.e_algorithm);
      break;
    case Plan::Action::kUncompress:
      str_line += L"现状 " + describe_state(fr_report.cs_state) + L"，将解压";
      break;
    case Plan::Action::kSkip:
      str_line += fr_report.pl_plan.str_reason;
      break;
  }
  return str_line;
}

inline std::wstring format_summary(const PipelineResult& pl_result) {
  int i4_compress{0};
  int i4_recompress{0};
  int i4_uncompress{0};
  int i4_skip{0};
  for (const FileReport& fr_report : pl_result.vec_reports) {
    if (!fr_report.str_error.empty()) continue;
    if (fr_report.b_executed) {
      switch (fr_report.er_exec.e_outcome) {
        case ExecuteResult::Outcome::kSuccess:
          switch (fr_report.pl_plan.e_action) {
            case Plan::Action::kCompress: ++i4_compress; break;
            case Plan::Action::kRecompress: ++i4_recompress; break;
            default: break;
          }
          break;
        case ExecuteResult::Outcome::kSkipped: ++i4_skip; break;
        case ExecuteResult::Outcome::kError: break;
      }
      continue;
    }
    switch (fr_report.pl_plan.e_action) {
      case Plan::Action::kCompress: ++i4_compress; break;
      case Plan::Action::kRecompress: ++i4_recompress; break;
      case Plan::Action::kUncompress: ++i4_uncompress; break;
      case Plan::Action::kSkip: ++i4_skip; break;
    }
  }
  std::wstring str_line{std::format(L"共 {} 个文件：压缩 {}、重压 {}、解压 {}、跳过 {}",
                                    pl_result.vec_reports.size(), i4_compress,
                                    i4_recompress, i4_uncompress, i4_skip)};
  if (pl_result.i4_failures > 0) {
    str_line += std::format(L"；失败 {}", pl_result.i4_failures);
  }
  return str_line;
}

PipelineResult run_scan(const Options& opt_options, const DetectFn& fn_detect);

void write_utf8(std::FILE* f_stream, const std::wstring& str_text);

int run_pipeline(const Options& opt_options, const DetectFn& fn_detect,
                 const ExecuteFn& fn_execute,
                 const UncompressFn& fn_uncompress, std::FILE* f_out,
                 std::FILE* f_err);

}
