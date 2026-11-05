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
    r.str_long_path = item.long_path;
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
                const ExecuteFn& fn_execute, std::FILE* out, std::FILE* err) {
  PipelineResult result = RunScan(options, detector);

  if (!options.dry_run) {
    for (FileReport& fr_report : result.reports) {
      if (!fr_report.error.empty()) continue;
      if (fr_report.plan.action != Plan::Action::Compress &&
          fr_report.plan.action != Plan::Action::Recompress) {
        continue;
      }
      fr_report.er_exec = fn_execute(fr_report.str_long_path,
                                     fr_report.plan.action, fr_report.state,
                                     options.algorithm);
      fr_report.b_executed = true;
      if (fr_report.er_exec.e_outcome == ExecuteResult::Outcome::kError) {
        ++result.failures;
      }
    }
  }

  if (!options.quiet) {
    for (const FileReport& r : result.reports) {
      const bool b_to_err =
          !r.error.empty() ||
          (r.b_executed && r.er_exec.e_outcome == ExecuteResult::Outcome::kError);
      WriteUtf8(b_to_err ? err : out, FormatReportLine(r, options) + L"\n");
    }
    WriteUtf8(out, FormatSummary(result) + L"\n");
  }
  return result.failures > 0 ? 1 : 0;
}

}
