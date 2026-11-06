#include "cli.hpp"
#include "pipeline.hpp"

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
  const ntfscompact::ParseResult pr_parse{
      ntfscompact::parse_args(std::vector<std::wstring>(argv + 1, argv + argc))};

  if (pr_parse.b_want_help) {
    ntfscompact::write_utf8(stdout, ntfscompact::usage_text());
    return 0;
  }
  if (pr_parse.b_want_version) {
    ntfscompact::write_utf8(stdout,
                            L"ntfscompact " + std::wstring(ntfscompact::kVersion) +
                                L"\n");
    return 0;
  }
  if (!pr_parse.b_ok) {
    ntfscompact::write_utf8(stderr,
                            L"ntfscompact: " + pr_parse.str_error + L"\n\n");
    ntfscompact::write_utf8(stderr, ntfscompact::usage_text());
    return 2;
  }

  auto xe_detector{ntfscompact::make_real_detector()};
  if (!xe_detector.has_value()) {
    ntfscompact::write_utf8(stderr,
                            L"ntfscompact: " + std::move(xe_detector).error() +
                                L"\n");
    return 1;
  }
  ntfscompact::DetectFn fn_detect{std::move(xe_detector).value()};
  ntfscompact::ExecuteFn fn_execute{};
  if (!pr_parse.opt_options.b_dry_run) {
    auto xe_executor{ntfscompact::make_real_executor()};
    if (!xe_executor.has_value()) {
      ntfscompact::write_utf8(
          stderr, L"ntfscompact: " + std::move(xe_executor).error() + L"\n");
      return 1;
    }
    fn_execute = std::move(xe_executor).value();
  }
  auto xe_uncompressor{ntfscompact::make_real_uncompressor()};
  if (!xe_uncompressor.has_value()) {
    ntfscompact::write_utf8(
        stderr, L"ntfscompact: " + std::move(xe_uncompressor).error() + L"\n");
    return 1;
  }
  ntfscompact::UncompressFn fn_uncompress{std::move(xe_uncompressor).value()};
  ntfscompact::ReleaseFn fn_release{};
  if (!pr_parse.opt_options.b_dry_run) {
    auto xe_releaser{ntfscompact::make_real_releaser()};
    if (!xe_releaser.has_value()) {
      ntfscompact::write_utf8(
          stderr, L"ntfscompact: " + std::move(xe_releaser).error() + L"\n");
      return 1;
    }
    fn_release = std::move(xe_releaser).value();
  }
  ntfscompact::DirFlagFn fn_dir_flag{};
  if (!pr_parse.opt_options.b_dry_run) {
    auto xe_dir_flag_clearer{ntfscompact::make_real_dir_flag_clearer()};
    if (!xe_dir_flag_clearer.has_value()) {
      ntfscompact::write_utf8(stderr, L"ntfscompact: " +
                                          std::move(xe_dir_flag_clearer).error() +
                                          L"\n");
      return 1;
    }
    fn_dir_flag = std::move(xe_dir_flag_clearer).value();
  }
  return ntfscompact::run_pipeline(pr_parse.opt_options, fn_detect, fn_execute,
                                   fn_uncompress, fn_release, fn_dir_flag,
                                   stdout, stderr);
}
