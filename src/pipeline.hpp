#pragma once

import std;

#include "cli.hpp"
#include "detect.hpp"
#include "dir_flag.hpp"
#include "executor.hpp"
#include "interrupt.hpp"
#include "release.hpp"
#include "scan.hpp"
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
  std::vector<std::wstring> vec_released{};
  std::uint64_t u8_logical_size{0};
  std::uint64_t u8_disk_size{0};
};

struct DirFlagReport {
  std::wstring str_display_path{};
  std::wstring str_long_path{};
  bool b_cleared{false};
  std::wstring str_error{};
};

struct PipelineResult {
  std::vector<FileReport> vec_reports{};
  std::vector<DirFlagReport> vec_dir_flags{};
  int i4_failures{};
  std::uint64_t u8_compressed_original{0};
  std::uint64_t u8_compressed_disk{0};
};

inline std::wstring format_report_line_core(const FileReport& fr_report,
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
    if (opt_options.b_verbose && fr_report.b_has_state) {
      str_line += L"现状 " + describe_state(fr_report.cs_state) + L"，";
    }
    switch (fr_report.er_exec.e_outcome) {
      case ExecuteResult::Outcome::kSuccess:
        if (fr_report.pl_plan.e_action == Plan::Action::kUncompress) {
          str_line += L"已解压";
          if (opt_options.b_verbose) {
            str_line += std::format(L"（{} 字节）",
                                    fr_report.u8_logical_size);
          }
        } else {
          str_line += fr_report.pl_plan.e_action == Plan::Action::kCompress
                          ? L"已压缩为指定算法 "
                          : L"已重压为指定算法 ";
          str_line += algorithm_name(opt_options.e_algorithm);
          if (opt_options.b_verbose) {
            str_line += std::format(L"（{} → {} 字节）",
                                    fr_report.u8_logical_size,
                                    fr_report.u8_disk_size);
          }
        }
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
  if (opt_options.b_verbose && opt_options.b_dry_run &&
      fr_report.pl_plan.e_action != Plan::Action::kSkip) {
    str_line += std::format(L"（{} 字节）", fr_report.u8_logical_size);
  }
  return str_line;
}

inline std::wstring format_report_line(const FileReport& fr_report,
                                       const Options& opt_options) {
  std::wstring str_line{format_report_line_core(fr_report, opt_options)};
  if (!fr_report.vec_released.empty()) {
    str_line += L"；升级释放：";
    for (std::size_t u8_index{0}; u8_index < fr_report.vec_released.size();
         ++u8_index) {
      if (u8_index > 0) {
        str_line += L"、";
      }
      str_line += fr_report.vec_released[u8_index];
    }
  }
  return str_line;
}

inline std::wstring format_dir_flag_line(const DirFlagReport& df_report,
                                         const Options& opt_options) {
  if (!df_report.str_error.empty()) {
    return L"[失败] " + df_report.str_display_path + L"：" + df_report.str_error;
  }
  return std::format(L"[目录标记] {}：{}",
                     df_report.str_display_path,
                     opt_options.b_dry_run ? L"将清除标记" : L"已清除标记");
}

inline bool counted_as_compressed(const FileReport& fr_report) {
  if (!fr_report.str_error.empty()) {
    return false;
  }
  if (fr_report.b_executed &&
      fr_report.er_exec.e_outcome != ExecuteResult::Outcome::kSuccess) {
    return false;
  }
  return fr_report.pl_plan.e_action == Plan::Action::kCompress ||
         fr_report.pl_plan.e_action == Plan::Action::kRecompress;
}

inline std::wstring format_summary(const PipelineResult& pl_result,
                                   const Options& opt_options) {
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
            case Plan::Action::kUncompress: ++i4_uncompress; break;
            case Plan::Action::kSkip: break;
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
  if (!opt_options.b_dry_run && i4_compress + i4_recompress > 0) {
    const std::uint64_t u8_original{pl_result.u8_compressed_original};
    const std::uint64_t u8_disk{pl_result.u8_compressed_disk};
    const std::uint64_t u8_saved{u8_original > u8_disk ? u8_original - u8_disk
                                                       : 0};
    const std::uint64_t u8_percent{
        u8_original > 0 ? u8_saved * 100 / u8_original : 0};
    str_line += std::format(L"；压缩/重压 {} → {} 字节，节省 {}%", u8_original,
                            u8_disk, u8_percent);
  }
  if (pl_result.i4_failures > 0) {
    str_line += std::format(L"；失败 {}", pl_result.i4_failures);
  }
  if (opt_options.b_dry_run) {
    if (!pl_result.vec_dir_flags.empty()) {
      str_line += std::format(L"；将清除目录标记 {} 个",
                              pl_result.vec_dir_flags.size());
    }
  } else {
    int i4_dir_flags_cleared{0};
    for (const DirFlagReport& df_report : pl_result.vec_dir_flags) {
      if (df_report.b_cleared) {
        ++i4_dir_flags_cleared;
      }
    }
    if (i4_dir_flags_cleared > 0) {
      str_line += std::format(L"；已清除目录标记 {} 个", i4_dir_flags_cleared);
    }
  }
  return str_line;
}

ScanOutcome run_scan(const Options& opt_options,
                     const InterruptPoller& fn_poll = InterruptPoller{});

void collect_byte_stats(PipelineResult& pl_result);

void write_utf8(std::FILE* f_stream, const std::wstring& str_text);

int run_pipeline(const Options& opt_options, const DetectFn& fn_detect,
                 const ExecuteFn& fn_execute,
                 const UncompressFn& fn_uncompress, const ReleaseFn& fn_release,
                 const DirFlagFn& fn_dir_flag, std::FILE* f_out,
                 std::FILE* f_err,
                 const InterruptPoller& fn_poll = InterruptPoller{});

}
