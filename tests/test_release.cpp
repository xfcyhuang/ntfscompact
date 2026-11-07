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

bool closed_has(const std::vector<ntfscompact::ClosedProcess>& vec_closed,
                std::uint32_t u4_pid, const wchar_t* ptr_name) {
  for (const ntfscompact::ClosedProcess& cp_item : vec_closed) {
    if (cp_item.u4_pid == u4_pid && cp_item.str_name == ptr_name) {
      return true;
    }
  }
  return false;
}

ntfscompact::ReleaseFn make_released_level(
    ntfscompact::ReleaseOutcome::Level e_level,
    std::vector<ntfscompact::ClosedProcess> vec_closed) {
  return [e_level, vec_closed = std::move(vec_closed)](const std::wstring&) {
    ntfscompact::ReleaseOutcome ro_result{};
    ro_result.e_status = ntfscompact::ReleaseOutcome::Status::kReleased;
    ro_result.e_level = e_level;
    ro_result.vec_closed = vec_closed;
    return ro_result;
  };
}

ntfscompact::ReleaseFn make_busy_level(std::vector<std::wstring>& vec_calls,
                                       std::wstring str_detail,
                                       std::vector<ntfscompact::ClosedProcess> vec_closed = {}) {
  return [&vec_calls, str_detail = std::move(str_detail),
          vec_closed = std::move(vec_closed)](const std::wstring& str_long_path) {
    vec_calls.push_back(str_long_path);
    ntfscompact::ReleaseOutcome ro_result{};
    ro_result.e_status = ntfscompact::ReleaseOutcome::Status::kBusy;
    ro_result.str_detail = str_detail;
    ro_result.vec_closed = vec_closed;
    return ro_result;
  };
}

struct SleepRecorder {
  std::vector<std::uint32_t> vec_ms{};
};

ntfscompact::EscalationBackoff make_test_backoff(SleepRecorder& rec_sleep,
                                                 std::uint32_t u4_budget_ms = 5000) {
  ntfscompact::EscalationBackoff eb_backoff{};
  eb_backoff.u4_budget_ms = u4_budget_ms;
  eb_backoff.fn_sleep = [&rec_sleep](std::uint32_t u4_ms) {
    rec_sleep.vec_ms.push_back(u4_ms);
  };
  return eb_backoff;
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

TEST_CASE("make_escalating_releaser：空闲文件不进链") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"free.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"free.bin", 8192));

  std::vector<std::wstring> vec_calls{};
  SleepRecorder rec_sleep{};
  const auto xe_releaser{ntfscompact::make_escalating_releaser(
      {make_busy_level(vec_calls, L"级一失败（模拟）")}, make_test_backoff(rec_sleep))};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kFree);
  CHECK(vec_calls.empty());
  CHECK(rec_sleep.vec_ms.empty());
}

TEST_CASE("make_escalating_releaser：占用文件首级即释放，无退避等待") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  SleepRecorder rec_sleep{};
  std::vector<std::wstring> vec_calls{};
  const auto xe_releaser{ntfscompact::make_escalating_releaser(
      {make_released_level(ntfscompact::ReleaseOutcome::Level::kRmGraceful,
                           {{4242, L"hold.exe"}})},
      make_test_backoff(rec_sleep))};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CloseHandle(h_lock);

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kRmGraceful);
  CHECK(closed_has(ro_outcome.vec_closed, 4242, L"hold.exe"));
  CHECK(vec_calls.empty());
  CHECK(rec_sleep.vec_ms.empty());
}

TEST_CASE("make_escalating_releaser：级间退避后次级释放，被关进程合并") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  SleepRecorder rec_sleep{};
  std::vector<std::wstring> vec_calls{};
  const auto xe_releaser{ntfscompact::make_escalating_releaser(
      {make_busy_level(vec_calls, L"级一失败（模拟）", {{111, L"a.exe"}}),
       make_released_level(ntfscompact::ReleaseOutcome::Level::kRmForce,
                           {{222, L"b.exe"}})},
      make_test_backoff(rec_sleep))};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CloseHandle(h_lock);

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kRmForce);
  CHECK(closed_has(ro_outcome.vec_closed, 111, L"a.exe"));
  CHECK(closed_has(ro_outcome.vec_closed, 222, L"b.exe"));
  REQUIRE(vec_calls.size() == 1);
  CHECK(rec_sleep.vec_ms == std::vector<std::uint32_t>{100});
}

TEST_CASE("make_escalating_releaser：链尽仍占用报升级链尽文案并带末级原因") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  SleepRecorder rec_sleep{};
  std::vector<std::wstring> vec_calls{};
  const auto xe_releaser{ntfscompact::make_escalating_releaser(
      {make_busy_level(vec_calls, L"级一失败（模拟）"),
       make_busy_level(vec_calls, L"级二失败（模拟）")},
      make_test_backoff(rec_sleep))};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CloseHandle(h_lock);

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK(has(narrow(ro_outcome.str_detail), "升级链尽后仍被占用"));
  CHECK(has(narrow(ro_outcome.str_detail), "级二失败"));
  CHECK(rec_sleep.vec_ms == std::vector<std::uint32_t>{100});
}

TEST_CASE("make_escalating_releaser：空级链占用直接报链尽") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  SleepRecorder rec_sleep{};
  const auto xe_releaser{ntfscompact::make_escalating_releaser(
      {}, make_test_backoff(rec_sleep))};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CloseHandle(h_lock);

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK(has(narrow(ro_outcome.str_detail), "升级链尽后仍被占用"));
  CHECK(rec_sleep.vec_ms.empty());
}

TEST_CASE("make_escalating_releaser：预算注入缩短退避且链仍走全") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  SleepRecorder rec_sleep{};
  std::vector<std::wstring> vec_calls{};
  const auto xe_releaser{ntfscompact::make_escalating_releaser(
      {make_busy_level(vec_calls, L"级一失败（模拟）"),
       make_busy_level(vec_calls, L"级二失败（模拟）"),
       make_busy_level(vec_calls, L"级三失败（模拟）")},
      make_test_backoff(rec_sleep, 150))};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CloseHandle(h_lock);

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK(vec_calls.size() == 3);
  CHECK(rec_sleep.vec_ms == std::vector<std::uint32_t>{100, 50});
}

TEST_CASE("make_escalating_releaser：退避探测发现已释放即止不再晋级") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  SleepRecorder rec_sleep{};
  std::vector<std::wstring> vec_calls{};
  ntfscompact::ReleaseFn fn_late_release{
      [&h_lock, &vec_calls](const std::wstring& str_long_path) {
        vec_calls.push_back(str_long_path);
        if (h_lock != INVALID_HANDLE_VALUE) {
          CloseHandle(h_lock);
          h_lock = INVALID_HANDLE_VALUE;
        }
        ntfscompact::ReleaseOutcome ro_result{};
        ro_result.e_status = ntfscompact::ReleaseOutcome::Status::kBusy;
        ro_result.e_level = ntfscompact::ReleaseOutcome::Level::kRmGraceful;
        ro_result.str_detail = L"级一失败（模拟）";
        return ro_result;
      }};
  const auto xe_releaser{ntfscompact::make_escalating_releaser(
      {std::move(fn_late_release),
       make_busy_level(vec_calls, L"级二失败（模拟）")},
      make_test_backoff(rec_sleep))};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  if (h_lock != INVALID_HANDLE_VALUE) {
    CloseHandle(h_lock);
  }

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kRmGraceful);
  CHECK(vec_calls.size() == 1);
  CHECK(rec_sleep.vec_ms == std::vector<std::uint32_t>{100});
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
