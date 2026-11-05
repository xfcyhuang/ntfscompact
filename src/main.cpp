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
    return 2;
  }

  std::wstring str_detector_error{};
  RealDetector detector_real{&str_detector_error};
  if (!str_detector_error.empty()) {
    WriteUtf8(stderr, L"ntfscompact: " + str_detector_error + L"\n");
    return 1;
  }
  ExecuteFn fn_execute{};
  if (!r.options.dry_run) {
    auto xe_executor{make_real_executor()};
    if (!xe_executor.has_value()) {
      WriteUtf8(stderr, L"ntfscompact: " + xe_executor.error() + L"\n");
      return 1;
    }
    fn_execute = std::move(xe_executor.value());
  }
  return RunPipeline(r.options, detector_real, fn_execute, stdout, stderr);
}
