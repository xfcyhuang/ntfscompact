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

namespace {

constexpr unsigned int kWin32SharingViolation{32};

struct ReleaseGate {
  bool b_busy{false};
  std::wstring str_busy_reason{};
  std::vector<std::wstring> vec_released{};
};

ReleaseGate gate_release(const std::wstring& str_long_path,
                         const ReleaseFn& fn_release) {
  ReleaseGate gate_result{};
  const ReleaseOutcome ro_release{fn_release(str_long_path)};
  switch (ro_release.e_status) {
    case ReleaseOutcome::Status::kFree:
      break;
    case ReleaseOutcome::Status::kReleased:
      if (ro_release.b_dismounted) {
        gate_result.vec_released.push_back(L"已卸载所在卷");
      }
      break;
    case ReleaseOutcome::Status::kBusy:
      gate_result.b_busy = true;
      gate_result.str_busy_reason = ro_release.str_detail.empty()
                                        ? std::wstring{L"占用不可释放"}
                                        : ro_release.str_detail;
      break;
  }
  return gate_result;
}

ReleaseGate ensure_released(const std::wstring& str_long_path,
                            const ReleaseFn& fn_release) {
  if (!fn_release || !probe_file_occupied(str_long_path)) {
    return ReleaseGate{};
  }
  return gate_release(str_long_path, fn_release);
}

void mark_busy_skip(FileReport& fr_report, const std::wstring& str_reason) {
  fr_report.b_executed = false;
  fr_report.er_exec = ExecuteResult{};
  fr_report.pl_plan.e_action = Plan::Action::kSkip;
  fr_report.pl_plan.str_reason = str_reason;
}

void append_released(std::vector<std::wstring>& vec_target,
                     const std::vector<std::wstring>& vec_extra) {
  vec_target.insert(vec_target.end(), vec_extra.begin(), vec_extra.end());
}

std::uint64_t query_disk_usage(const std::wstring& str_long_path,
                               std::uint64_t u8_fallback) {
  DWORD u4_high{0};
  SetLastError(ERROR_SUCCESS);
  const DWORD u4_low{GetCompressedFileSizeW(str_long_path.c_str(), &u4_high)};
  if (u4_low == INVALID_FILE_SIZE && GetLastError() != ERROR_SUCCESS) {
    return u8_fallback;
  }
  return (static_cast<std::uint64_t>(u4_high) << 32) |
         static_cast<std::uint64_t>(u4_low);
}

bool report_is_failure(const FileReport& fr_report) {
  return !fr_report.str_error.empty() ||
         (fr_report.b_executed &&
          fr_report.er_exec.e_outcome == ExecuteResult::Outcome::kError);
}

}

void collect_byte_stats(PipelineResult& pl_result) {
  pl_result.u8_compressed_original = 0;
  pl_result.u8_compressed_disk = 0;
  for (FileReport& fr_report : pl_result.vec_reports) {
    if (!counted_as_compressed(fr_report)) {
      continue;
    }
    fr_report.u8_disk_size =
        query_disk_usage(fr_report.str_long_path, fr_report.u8_logical_size);
    pl_result.u8_compressed_original += fr_report.u8_logical_size;
    pl_result.u8_compressed_disk += fr_report.u8_disk_size;
  }
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
    fr_report.u8_logical_size = si_item.u8_size;
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
                 const UncompressFn& fn_uncompress, const ReleaseFn& fn_release,
                 std::FILE* f_out, std::FILE* f_err) {
  PipelineResult pl_result{run_scan(opt_options, fn_detect)};

  if (!opt_options.b_dry_run) {
    for (FileReport& fr_report : pl_result.vec_reports) {
      if (!fr_report.str_error.empty()) continue;
      switch (fr_report.pl_plan.e_action) {
        case Plan::Action::kCompress:
        case Plan::Action::kRecompress: {
          ReleaseGate gate_ensure{
              ensure_released(fr_report.str_long_path, fn_release)};
          if (!gate_ensure.b_busy) {
            append_released(fr_report.vec_released, gate_ensure.vec_released);
            fr_report.er_exec =
                fn_execute(fr_report.str_long_path,
                           fr_report.pl_plan.e_action, fr_report.cs_state,
                           opt_options.e_algorithm);
            fr_report.b_executed = true;
            if (fn_release && fr_report.b_executed &&
                fr_report.er_exec.e_outcome ==
                    ExecuteResult::Outcome::kError &&
                fr_report.er_exec.u4_error == kWin32SharingViolation) {
              ReleaseGate gate_retry{
                  gate_release(fr_report.str_long_path, fn_release)};
              if (gate_retry.b_busy) {
                mark_busy_skip(fr_report, gate_retry.str_busy_reason);
              } else {
                append_released(fr_report.vec_released,
                                gate_retry.vec_released);
                fr_report.er_exec =
                    fn_execute(fr_report.str_long_path,
                               fr_report.pl_plan.e_action,
                               fr_report.cs_state,
                               opt_options.e_algorithm);
                if (fr_report.er_exec.e_outcome ==
                        ExecuteResult::Outcome::kError &&
                    fr_report.er_exec.u4_error == kWin32SharingViolation) {
                  mark_busy_skip(
                      fr_report,
                      L"占用不可释放：卷卸载后重试执行仍报占用（错误码 32）");
                }
              }
            }
          } else {
            mark_busy_skip(fr_report, gate_ensure.str_busy_reason);
          }
          if (fr_report.b_executed &&
              fr_report.er_exec.e_outcome == ExecuteResult::Outcome::kError) {
            ++pl_result.i4_failures;
          }
          break;
        }
        case Plan::Action::kUncompress: {
          if (!fn_uncompress) {
            fr_report.str_error = L"解压器未接线";
            ++pl_result.i4_failures;
            break;
          }
          ReleaseGate gate_ensure{
              ensure_released(fr_report.str_long_path, fn_release)};
          if (gate_ensure.b_busy) {
            mark_busy_skip(fr_report, gate_ensure.str_busy_reason);
            break;
          }
          append_released(fr_report.vec_released, gate_ensure.vec_released);
          const std::expected<void, std::wstring> xe_uncompress{
              fn_uncompress(fr_report.str_long_path)};
          fr_report.b_executed = true;
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

  collect_byte_stats(pl_result);

  for (const FileReport& fr_report : pl_result.vec_reports) {
    if (report_is_failure(fr_report)) {
      write_utf8(f_err, format_report_line(fr_report, opt_options) + L"\n");
    } else if (!opt_options.b_quiet) {
      write_utf8(f_out, format_report_line(fr_report, opt_options) + L"\n");
    }
  }
  write_utf8(f_out, format_summary(pl_result, opt_options) + L"\n");
  return pl_result.i4_failures > 0 ? 1 : 0;
}

}
