#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pipeline.hpp"
#include "release.hpp"
#include "temp_dir.hpp"
#include "util.hpp"

#include <windows.h>

namespace {

using ntfscompact_test::narrow;

bool has(const std::string& str_haystack, const char* ptr_needle) {
  return str_haystack.find(ptr_needle) != std::string::npos;
}

bool write_compressible(const std::wstring& str_long_path) {
  HANDLE h_file{CreateFileW(str_long_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  static const char kSentence[]{"The quick brown fox jumps over the lazy dog. "};
  bool b_ok{true};
  for (int i4_line{0}; b_ok && i4_line < 4000; ++i4_line) {
    DWORD u4_written{};
    b_ok = ::WriteFile(h_file, kSentence, sizeof(kSentence) - 1, &u4_written,
                       nullptr) &&
           u4_written == sizeof(kSentence) - 1;
  }
  CloseHandle(h_file);
  return b_ok;
}

}

TEST_CASE("probe_file_occupied：空闲不占用，零共享句柄报占用") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"probe.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"probe.bin", 8192));

  CHECK_FALSE(ntfscompact::probe_file_occupied(str_file));

  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);
  CHECK(ntfscompact::probe_file_occupied(str_file));
  CloseHandle(h_lock);
  CHECK_FALSE(ntfscompact::probe_file_occupied(str_file));
}

TEST_CASE("make_real_releaser：假卸载注入与缺省真实卷卸载") {
  ntfscompact::DismountFn fn_fake{[](const std::wstring&) { return true; }};
  const auto xe_fake{ntfscompact::make_real_releaser(fn_fake)};
  REQUIRE(xe_fake.has_value());
  CHECK(xe_fake.value());

  const auto xe_real{ntfscompact::make_real_releaser()};
  REQUIRE(xe_real.has_value());
  CHECK(xe_real.value());

  const auto xe_none{ntfscompact::make_real_releaser(ntfscompact::DismountFn{})};
  CHECK_FALSE(xe_none.has_value());
}

TEST_CASE("直接卷卸载：空闲文件不动卷") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"free.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"free.bin", 8192));

  bool b_called{false};
  ntfscompact::DismountFn fn_fake{
      [&b_called](const std::wstring&) { b_called = true; return true; }};
  const auto xe_releaser{ntfscompact::make_real_releaser(fn_fake)};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kFree);
  CHECK_FALSE(b_called);
}

TEST_CASE("直接卷卸载：非管理员遇占用计跳过，不调卸载") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));

  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  bool b_called{false};
  ntfscompact::DismountFn fn_fake{
      [&b_called](const std::wstring&) { b_called = true; return true; }};
  const auto xe_releaser{ntfscompact::make_real_releaser(fn_fake)};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CloseHandle(h_lock);

  if (ntfscompact::process_is_elevated()) {
    return;
  }
  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK_FALSE(b_called);
  CHECK(has(narrow(ro_outcome.str_detail), "管理员"));
}

TEST_CASE("直接卷卸载：提权后经注入卸载释放") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held2.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held2.bin", 8192));

  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  ntfscompact::DismountFn fn_fake{[&h_lock](const std::wstring&) {
    CloseHandle(h_lock);
    h_lock = INVALID_HANDLE_VALUE;
    return true;
  }};
  const auto xe_releaser{ntfscompact::make_real_releaser(fn_fake)};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  if (h_lock != INVALID_HANDLE_VALUE) {
    CloseHandle(h_lock);
  }

  if (!ntfscompact::process_is_elevated()) {
    CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
    return;
  }
  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.b_dismounted);
}

TEST_CASE("dismount_volume_for：网络卷与不存在路径安全拒绝") {
  CHECK_FALSE(ntfscompact::dismount_volume_for(
      L"\\\\?\\UNC\\192.168.1.3\\mypool_ds0\\no\\such.bin"));
  CHECK_FALSE(ntfscompact::dismount_volume_for(
      L"\\\\?\\D:\\definitely\\not\\existing\\file.bin"));
}

TEST_CASE("run_pipeline：注入假释放后压缩继续") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_sub(tmp_dir, L"ok.bin")};
  REQUIRE(write_compressible(
      ntfscompact_test::temp_long_api_path(str_file)));

  auto xe_detector{ntfscompact::make_real_detector()};
  REQUIRE(xe_detector.has_value());
  ntfscompact::DetectFn fn_detect{std::move(xe_detector).value()};
  auto xe_executor{ntfscompact::make_real_executor()};
  REQUIRE(xe_executor.has_value());
  ntfscompact::ExecuteFn fn_execute{std::move(xe_executor).value()};
  auto xe_uncompressor{ntfscompact::make_real_uncompressor()};
  REQUIRE(xe_uncompressor.has_value());
  ntfscompact::UncompressFn fn_uncompress{std::move(xe_uncompressor).value()};
  ntfscompact::ReleaseFn fn_release{[](const std::wstring&) {
    ntfscompact::ReleaseOutcome ro{};
    ro.e_status = ntfscompact::ReleaseOutcome::Status::kFree;
    return ro;
  }};

  ntfscompact::Options opt_case{};
  opt_case.b_verbose = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{ntfscompact::run_pipeline(
      opt_case, fn_detect, fn_execute, fn_uncompress, fn_release, ntfscompact::DirFlagFn{}, f_out,
      f_err)};
  std::fclose(f_out);
  std::fclose(f_err);
  CHECK(i4_code == 0);

  const ntfscompact::DetectionResult dr_verified{fn_detect(str_file)};
  CHECK(dr_verified.str_error.empty());
  CHECK(dr_verified.cs_state.e_kind == ntfscompact::CurrentState::Kind::kWof);
  CHECK(dr_verified.cs_state.e_algorithm == ntfscompact::Algorithm::kXpress8k);
}
