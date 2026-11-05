#include "pipeline.hpp"

#include "scan.hpp"

#include <windows.h>

namespace ntfscompact {

void write_utf8(std::FILE* f_stream, const std::wstring& str_text) {
  const int i4_bytes{WideCharToMultiByte(CP_UTF8, 0, str_text.data(),
                                         static_cast<int>(str_text.size()),
                                         nullptr, 0, nullptr, nullptr)};
  std::string str_utf8(static_cast<std::size_t>(i4_bytes), '\0');
  if (i4_bytes > 0) {
    WideCharToMultiByte(CP_UTF8, 0, str_text.data(),
                        static_cast<int>(str_text.size()), str_utf8.data(),
                        i4_bytes, nullptr, nullptr);
  }
  std::print(f_stream, "{}", std::string_view{str_utf8});
}

PipelineResult run_scan(const Options& opt_options, const DetectFn& fn_detect) {
  PipelineResult pl_result{};
  ScanOutcome so_scan{};
  scan_paths(opt_options.vec_paths, so_scan);

  for (const ScanSkip& sk_skip : so_scan.vec_skipped) {
    FileReport fr_report{};
    fr_report.str_display_path = sk_skip.str_display_path;
    fr_report.b_has_state = false;
    fr_report.pl_plan.e_action = Plan::Action::kSkip;
    fr_report.pl_plan.str_reason = sk_skip.str_reason;
    pl_result.vec_reports.push_back(fr_report);
  }
  for (const ScanItem& si_item : so_scan.vec_items) {
    FileReport fr_report{};
    fr_report.str_display_path = si_item.str_display_path;
    fr_report.str_long_path = si_item.str_long_path;
    const DetectionResult dr_detect{fn_detect(si_item.str_long_path)};
    if (!dr_detect.str_error.empty()) {
      fr_report.str_error = dr_detect.str_error;
      ++pl_result.i4_failures;
      pl_result.vec_reports.push_back(fr_report);
      continue;
    }
    fr_report.b_has_state = true;
    fr_report.cs_state = dr_detect.cs_state;
    fr_report.pl_plan = decide_action(dr_detect.cs_state, opt_options);
    pl_result.vec_reports.push_back(fr_report);
  }
  for (const ScanError& se_error : so_scan.vec_errors) {
    FileReport fr_report{};
    fr_report.str_display_path = se_error.str_display_path;
    fr_report.str_error = se_error.str_message;
    ++pl_result.i4_failures;
    pl_result.vec_reports.push_back(fr_report);
  }
  return pl_result;
}

int run_pipeline(const Options& opt_options, const DetectFn& fn_detect,
                 const ExecuteFn& fn_execute,
                 const UncompressFn& fn_uncompress, std::FILE* f_out,
                 std::FILE* f_err) {
  PipelineResult pl_result{run_scan(opt_options, fn_detect)};

  if (!opt_options.b_dry_run) {
    for (FileReport& fr_report : pl_result.vec_reports) {
      if (!fr_report.str_error.empty()) continue;
      switch (fr_report.pl_plan.e_action) {
        case Plan::Action::kCompress:
        case Plan::Action::kRecompress:
          fr_report.er_exec =
              fn_execute(fr_report.str_long_path, fr_report.pl_plan.e_action,
                         fr_report.cs_state, opt_options.e_algorithm);
          fr_report.b_executed = true;
          if (fr_report.er_exec.e_outcome == ExecuteResult::Outcome::kError) {
            ++pl_result.i4_failures;
          }
          break;
        case Plan::Action::kUncompress: {
          if (!fn_uncompress) {
            fr_report.str_error = L"解压器未接线";
            ++pl_result.i4_failures;
            break;
          }
          const std::expected<void, std::wstring> xe_uncompress{
              fn_uncompress(fr_report.str_long_path)};
          if (!xe_uncompress.has_value()) {
            fr_report.str_error = xe_uncompress.error();
            ++pl_result.i4_failures;
          }
          break;
        }
        case Plan::Action::kSkip:
          break;
      }
    }
  }

  if (!opt_options.b_quiet) {
    if (opt_options.b_dry_run || opt_options.b_verbose ||
        !opt_options.b_uncompress) {
      for (const FileReport& fr_report : pl_result.vec_reports) {
        const bool b_to_err{
            !fr_report.str_error.empty() ||
            (fr_report.b_executed &&
             fr_report.er_exec.e_outcome == ExecuteResult::Outcome::kError)};
        write_utf8(b_to_err ? f_err : f_out,
                   format_report_line(fr_report, opt_options) + L"\n");
      }
    }
    write_utf8(f_out, format_summary(pl_result) + L"\n");
  }
  return pl_result.i4_failures > 0 ? 1 : 0;
}

}
