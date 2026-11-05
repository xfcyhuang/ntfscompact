#include "cli.hpp"
#include "pipeline.hpp"

#include <cstdio>

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
  auto xe_detector{make_real_detector()};
  if (!xe_detector.has_value()) {
    str_detector_error = std::move(xe_detector).error();
    WriteUtf8(stderr, L"ntfscompact: " + str_detector_error + L"\n");
    return 1;
  }
  DetectFn fn_detect{std::move(xe_detector).value()};
  ExecuteFn fn_execute{};
  if (!r.options.dry_run) {
    auto xe_executor{make_real_executor()};
    if (!xe_executor.has_value()) {
      WriteUtf8(stderr, L"ntfscompact: " + xe_executor.error() + L"\n");
      return 1;
    }
    fn_execute = std::move(xe_executor.value());
  }
  auto xe_uncompressor{make_real_uncompressor()};
  if (!xe_uncompressor.has_value()) {
    WriteUtf8(stderr, L"ntfscompact: " + xe_uncompressor.error() + L"\n");
    return 1;
  }
  UncompressFn fn_uncompress{std::move(xe_uncompressor.value())};
  return RunPipeline(r.options, fn_detect, fn_execute, fn_uncompress,
                     stdout, stderr);
}
