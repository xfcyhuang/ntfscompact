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

struct HoldChild {
  HANDLE h_process{INVALID_HANDLE_VALUE};
  HANDLE h_thread{INVALID_HANDLE_VALUE};
  DWORD u4_pid{0};
};

struct HoldChildReleaser {
  void operator()(HoldChild* ptr_child) const {
    if (ptr_child != nullptr) {
      if (ptr_child->h_process != INVALID_HANDLE_VALUE &&
          WaitForSingleObject(ptr_child->h_process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(ptr_child->h_process, 1);
        WaitForSingleObject(ptr_child->h_process, 5000);
      }
      if (ptr_child->h_process != INVALID_HANDLE_VALUE) {
        CloseHandle(ptr_child->h_process);
      }
      if (ptr_child->h_thread != INVALID_HANDLE_VALUE) {
        CloseHandle(ptr_child->h_thread);
      }
      delete ptr_child;
    }
  }
};

using HoldChildPtr = std::unique_ptr<HoldChild, HoldChildReleaser>;

std::wstring hold_exe_path() {
  wchar_t arr_module[MAX_PATH]{};
  const DWORD u4_len{GetModuleFileNameW(nullptr, arr_module, MAX_PATH)};
  const std::wstring str_module{arr_module, u4_len};
  const std::size_t u8_slash{str_module.find_last_of(L"\\")};
  return str_module.substr(0, u8_slash) + L"\\ntfscompact_hold.exe";
}

HoldChildPtr spawn_hold_child_ex(const std::wstring& str_hold_exe,
                                 const std::wstring& str_file, bool b_stubborn,
                                 bool b_coop, bool b_lock0,
                                 std::uint32_t u4_close_after_ms) {
  std::wstring str_cmd{L"\"" + str_hold_exe + L"\" \"" + str_file + L"\""};
  if (b_stubborn) {
    str_cmd += L" --stubborn";
  }
  if (b_coop) {
    str_cmd += L" --coop";
  }
  if (b_lock0) {
    str_cmd += L" --lock0";
  }
  if (u4_close_after_ms > 0) {
    str_cmd +=
        L" --close-after-ms " + std::to_wstring(u4_close_after_ms);
  }
  STARTUPINFOW si_start{};
  si_start.cb = sizeof(si_start);
  PROCESS_INFORMATION pi_process{};
  const BOOL b_ok{CreateProcessW(str_hold_exe.c_str(), str_cmd.data(), nullptr,
                                 nullptr, FALSE, 0, nullptr, nullptr, &si_start,
                                 &pi_process)};
  if (b_ok == FALSE) {
    return HoldChildPtr{};
  }
  HoldChildPtr up_child{new HoldChild{}};
  up_child->h_process = pi_process.hProcess;
  up_child->h_thread = pi_process.hThread;
  up_child->u4_pid = pi_process.dwProcessId;
  return up_child;
}

HoldChildPtr spawn_hold_child(const std::wstring& str_hold_exe,
                              const std::wstring& str_file, bool b_stubborn) {
  return spawn_hold_child_ex(str_hold_exe, str_file, b_stubborn, false, false,
                             0);
}

bool wait_file_occupied(const std::wstring& str_file) {
  for (int i4_attempt{0}; i4_attempt < 100; ++i4_attempt) {
    if (ntfscompact::probe_file_occupied(str_file)) {
      return true;
    }
    Sleep(50);
  }
  return false;
}

bool wait_child_exit(HANDLE h_process, DWORD u4_timeout_ms, DWORD& u4_code) {
  if (WaitForSingleObject(h_process, u4_timeout_ms) != WAIT_OBJECT_0) {
    return false;
  }
  return GetExitCodeProcess(h_process, &u4_code) != FALSE;
}

std::wstring closed_pid_name(const std::vector<ntfscompact::ClosedProcess>& vec_closed,
                             std::uint32_t u4_pid) {
  for (const ntfscompact::ClosedProcess& cp_item : vec_closed) {
    if (cp_item.u4_pid == u4_pid) {
      return cp_item.str_name;
    }
  }
  return L"";
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

struct HandleCloseCall {
  std::uint32_t u4_pid{0};
  std::uintptr_t u8_handle{0};
};

constexpr std::uint32_t kDataAccess{FILE_READ_DATA | FILE_WRITE_DATA};

struct FakeHandleDeps {
  bool b_elevated{true};
  std::vector<ntfscompact::HandleOwner> vec_owners{};
  std::vector<std::wstring> vec_find_calls{};
  std::vector<HandleCloseCall> vec_close_calls{};
  std::map<std::uint32_t, std::wstring> map_names{};

  ntfscompact::HandleForceDeps deps() {
    ntfscompact::HandleForceDeps deps_out{};
    deps_out.fn_is_elevated = [this] { return b_elevated; };
    deps_out.fn_find_handles = [this](const std::wstring& str_long_path) {
      vec_find_calls.push_back(str_long_path);
      return vec_owners;
    };
    deps_out.fn_process_name = [this](std::uint32_t u4_pid) {
      const auto it_find{map_names.find(u4_pid)};
      return it_find != map_names.end() ? it_find->second : std::wstring{};
    };
    deps_out.fn_close_handle = [this](std::uint32_t u4_pid,
                                      std::uintptr_t u8_handle) {
      vec_close_calls.push_back({u4_pid, u8_handle});
      return true;
    };
    return deps_out;
  }
};

}

TEST_CASE("classify_process_risk：低风险占用者入表") {
  using ntfscompact::ProcessRisk;
  CHECK(ntfscompact::classify_process_risk(L"SearchIndexer.exe") ==
        ProcessRisk::kLow);
  CHECK(ntfscompact::classify_process_risk(L"searchprotocolhost.exe") ==
        ProcessRisk::kLow);
  CHECK(ntfscompact::classify_process_risk(L"SEARCHFILTERHOST.EXE") ==
        ProcessRisk::kLow);
  CHECK(ntfscompact::classify_process_risk(L"MsMpEng.exe") == ProcessRisk::kLow);
  CHECK(ntfscompact::classify_process_risk(L"NisSrv.exe") == ProcessRisk::kLow);
}

TEST_CASE("classify_process_risk：explorer 与数据库/虚拟机/编辑器/系统关键进程高风险") {
  using ntfscompact::ProcessRisk;
  CHECK(ntfscompact::classify_process_risk(L"explorer.exe") ==
        ProcessRisk::kHigh);
  CHECK(ntfscompact::classify_process_risk(L"sqlservr.exe") ==
        ProcessRisk::kHigh);
  CHECK(ntfscompact::classify_process_risk(L"mysqld.exe") == ProcessRisk::kHigh);
  CHECK(ntfscompact::classify_process_risk(L"vmware-vmx.exe") ==
        ProcessRisk::kHigh);
  CHECK(ntfscompact::classify_process_risk(L"VirtualBox.exe") ==
        ProcessRisk::kHigh);
  CHECK(ntfscompact::classify_process_risk(L"WINWORD.EXE") ==
        ProcessRisk::kHigh);
  CHECK(ntfscompact::classify_process_risk(L"Code.exe") == ProcessRisk::kHigh);
  CHECK(ntfscompact::classify_process_risk(L"lsass.exe") == ProcessRisk::kHigh);
  CHECK(ntfscompact::classify_process_risk(L"svchost.exe") ==
        ProcessRisk::kHigh);
  CHECK(ntfscompact::classify_process_risk(L"winlogon.exe") ==
        ProcessRisk::kHigh);
}

TEST_CASE("classify_process_risk：不在表内为未知，不冒充低风险") {
  using ntfscompact::ProcessRisk;
  CHECK(ntfscompact::classify_process_risk(L"unknown_app.exe") ==
        ProcessRisk::kUnknown);
  CHECK(ntfscompact::classify_process_risk(L"") == ProcessRisk::kUnknown);
  CHECK(ntfscompact::classify_process_risk(L"explorer_backup.exe") ==
        ProcessRisk::kUnknown);
  CHECK(ntfscompact::classify_process_risk(L"searchindexer_tool.exe") ==
        ProcessRisk::kUnknown);
  CHECK(ntfscompact::classify_process_risk(L"ntfscompact_hold.exe") ==
        ProcessRisk::kUnknown);
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

TEST_CASE("make_real_releaser：RM 优雅关释放自有进程占用（无需提权）") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  HoldChildPtr up_child{spawn_hold_child(str_hold_exe, str_file, false)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(str_file));

  const auto xe_releaser{ntfscompact::make_real_releaser()};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kRmGraceful);
  CHECK_FALSE(closed_pid_name(ro_outcome.vec_closed, up_child->u4_pid).empty());
  DWORD u4_code{0};
  CHECK(wait_child_exit(up_child->h_process, 15000, u4_code));
  CHECK(u4_code == 0);
  CHECK_FALSE(ntfscompact::probe_file_occupied(str_file));
}

TEST_CASE("make_rm_force_level：假优雅失败注入升级后强制关可达") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  HoldChildPtr up_child{spawn_hold_child(str_hold_exe, str_file, false)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(str_file));

  ntfscompact::ReleaseFn fn_fake_graceful{
      [](const std::wstring&) {
        ntfscompact::ReleaseOutcome ro_busy{};
        ro_busy.e_status = ntfscompact::ReleaseOutcome::Status::kBusy;
        ro_busy.e_level = ntfscompact::ReleaseOutcome::Level::kRmGraceful;
        ro_busy.str_detail = L"RM 优雅关失败（模拟）";
        return ro_busy;
      }};
  const auto xe_force{ntfscompact::make_rm_force_level()};
  REQUIRE(xe_force.has_value());
  const auto xe_chain{ntfscompact::make_escalating_releaser(
      {std::move(fn_fake_graceful), std::move(xe_force).value()},
      ntfscompact::EscalationBackoff{})};
  REQUIRE(xe_chain.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_chain.value()(str_file)};

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kRmForce);
  CHECK_FALSE(closed_pid_name(ro_outcome.vec_closed, up_child->u4_pid).empty());
  DWORD u4_code{0};
  CHECK(wait_child_exit(up_child->h_process, 15000, u4_code));
  CHECK(u4_code == 0);
  CHECK_FALSE(ntfscompact::probe_file_occupied(str_file));
}

TEST_CASE("make_real_releaser：优雅关未生效自然升级强制关") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  HoldChildPtr up_child{spawn_hold_child(str_hold_exe, str_file, true)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(str_file));

  const auto xe_releaser{ntfscompact::make_real_releaser()};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kRmForce);
  CHECK_FALSE(closed_pid_name(ro_outcome.vec_closed, up_child->u4_pid).empty());
  DWORD u4_code{0};
  CHECK(wait_child_exit(up_child->h_process, 40000, u4_code));
  CHECK_FALSE(ntfscompact::probe_file_occupied(str_file));
}

TEST_CASE("make_handle_force_level：非提权直接止步并如实报告，不做枚举") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));

  FakeHandleDeps fake_deps{};
  fake_deps.b_elevated = false;
  fake_deps.vec_owners = {{4321, 100, kDataAccess}};
  const auto fn_level{
      ntfscompact::make_handle_force_level(false, fake_deps.deps())};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kHandle);
  CHECK(ro_outcome.vec_closed.empty());
  CHECK(ro_outcome.str_detail.find(L"管理员") != std::wstring::npos);
  CHECK(fake_deps.vec_find_calls.empty());
  CHECK(fake_deps.vec_close_calls.empty());
}

TEST_CASE("make_handle_force_level：默认闸门只关低风险占用者，高风险跳过并提示") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  FakeHandleDeps fake_deps{};
  fake_deps.vec_owners = {
      {101, 11, kDataAccess}, {202, 22, kDataAccess}, {101, 12, kDataAccess}};
  fake_deps.map_names = {{101, L"SearchIndexer.exe"}, {202, L"explorer.exe"}};
  const auto fn_level{
      ntfscompact::make_handle_force_level(false, fake_deps.deps())};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};
  CloseHandle(h_lock);

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kHandle);
  REQUIRE(fake_deps.vec_close_calls.size() == 2);
  CHECK(fake_deps.vec_close_calls[0].u4_pid == 101);
  CHECK(fake_deps.vec_close_calls[0].u8_handle == 11);
  CHECK(fake_deps.vec_close_calls[1].u4_pid == 101);
  CHECK(fake_deps.vec_close_calls[1].u8_handle == 12);
  CHECK(ro_outcome.vec_closed.size() == 1);
  CHECK(ro_outcome.vec_closed[0].u4_pid == 101);
  CHECK(ro_outcome.vec_closed[0].str_name == L"SearchIndexer.exe");
  CHECK(ro_outcome.str_detail.find(L"explorer.exe") != std::wstring::npos);
  CHECK(ro_outcome.str_detail.find(L"--force-close-all") != std::wstring::npos);
}

TEST_CASE("make_handle_force_level：--force-close-all 放行全部占用者") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  FakeHandleDeps fake_deps{};
  fake_deps.vec_owners = {{101, 11, kDataAccess}, {202, 22, kDataAccess}};
  fake_deps.map_names = {{101, L"SearchIndexer.exe"}, {202, L"explorer.exe"}};
  const auto fn_level{
      ntfscompact::make_handle_force_level(true, fake_deps.deps())};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};
  CloseHandle(h_lock);

  CHECK(fake_deps.vec_close_calls.size() == 2);
  CHECK(fake_deps.vec_close_calls[0].u4_pid == 101);
  CHECK(fake_deps.vec_close_calls[1].u4_pid == 202);
  CHECK(ro_outcome.vec_closed.size() == 2);
  CHECK(ro_outcome.str_detail.find(L"闸门") == std::wstring::npos);
}

TEST_CASE("make_handle_force_level：低风险强关后文件释放报成功带审计") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));

  FakeHandleDeps fake_deps{};
  fake_deps.vec_owners = {{303, 33, kDataAccess}};
  fake_deps.map_names = {{303, L"searchfilterhost.exe"}};
  const auto fn_level{
      ntfscompact::make_handle_force_level(false, fake_deps.deps())};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kHandle);
  REQUIRE(ro_outcome.vec_closed.size() == 1);
  CHECK(ro_outcome.vec_closed[0].u4_pid == 303);
  CHECK(ro_outcome.vec_closed[0].str_name == L"searchfilterhost.exe");
  CHECK_FALSE(ro_outcome.str_detail.empty());
}

TEST_CASE("make_handle_force_level：进程名取不到即自然止步不崩") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  FakeHandleDeps fake_deps{};
  fake_deps.vec_owners = {{404, 44, kDataAccess}, {505, 55, kDataAccess}};
  fake_deps.map_names = {{404, L""}, {505, L"msmpeng.exe"}};
  const auto fn_level{
      ntfscompact::make_handle_force_level(false, fake_deps.deps())};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};
  CloseHandle(h_lock);

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  REQUIRE(fake_deps.vec_close_calls.size() == 1);
  CHECK(fake_deps.vec_close_calls[0].u4_pid == 505);
  CHECK(ro_outcome.vec_closed.size() == 1);
  CHECK(ro_outcome.vec_closed[0].u4_pid == 505);
}

TEST_CASE("make_handle_force_level：句柄关不动（拒绝访问）自然止步不报已关") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  FakeHandleDeps fake_deps{};
  fake_deps.vec_owners = {{606, 66, kDataAccess}};
  fake_deps.map_names = {{606, L"lsass.exe"}};
  ntfscompact::HandleForceDeps deps_handle{fake_deps.deps()};
  deps_handle.fn_close_handle = [](std::uint32_t, std::uintptr_t) {
    return false;
  };
  const auto fn_level{ntfscompact::make_handle_force_level(true, deps_handle)};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};
  CloseHandle(h_lock);

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK(ro_outcome.vec_closed.empty());
  CHECK(ro_outcome.str_detail.find(L"句柄强关") != std::wstring::npos);
}

TEST_CASE("make_handle_force_level：自身 PID 句柄被过滤不强关自己") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  FakeHandleDeps fake_deps{};
  fake_deps.vec_owners = {{GetCurrentProcessId(), 77, kDataAccess}};
  const auto fn_level{
      ntfscompact::make_handle_force_level(true, fake_deps.deps())};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};
  CloseHandle(h_lock);

  CHECK(fake_deps.vec_close_calls.empty());
  CHECK(ro_outcome.vec_closed.empty());
  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
}

TEST_CASE("make_handle_force_level：无数据访问句柄不入强关集合") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));

  FakeHandleDeps fake_deps{};
  fake_deps.vec_owners = {{707, 88, kDataAccess},
                          {707, 99, FILE_READ_ATTRIBUTES},
                          {808, 101, 0}};
  fake_deps.map_names = {{707, L"searchindexer.exe"},
                         {808, L"searchprotocolhost.exe"}};
  const auto fn_level{
      ntfscompact::make_handle_force_level(false, fake_deps.deps())};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};

  REQUIRE(fake_deps.vec_close_calls.size() == 1);
  CHECK(fake_deps.vec_close_calls[0].u4_pid == 707);
  CHECK(fake_deps.vec_close_calls[0].u8_handle == 88);
  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kHandle);
  REQUIRE(ro_outcome.vec_closed.size() == 1);
  CHECK(ro_outcome.vec_closed[0].u4_pid == 707);
  CHECK(ro_outcome.vec_closed[0].str_name == L"searchindexer.exe");
}

TEST_CASE("make_oplock_level：独占持有者不参与 oplock 协议，快速降级不拖预算") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  HoldChildPtr up_child{spawn_hold_child(str_hold_exe, str_file, false)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(str_file));

  const std::uint64_t u8_start_ms{GetTickCount64()};
  const auto fn_level{ntfscompact::make_oplock_level(2000)};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};
  const std::uint64_t u8_elapsed_ms{GetTickCount64() - u8_start_ms};

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kOplock);
  CHECK(ro_outcome.vec_closed.empty());
  CHECK(ro_outcome.str_detail.find(L"oplock") != std::wstring::npos);
  CHECK(u8_elapsed_ms < 1500);
}

TEST_CASE("make_oplock_level：注入短超时被动等待后降级，不真等默认预算") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));

  const std::uint64_t u8_start_ms{GetTickCount64()};
  const auto fn_level{ntfscompact::make_oplock_level(
      60, [](const std::wstring&) { return true; })};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};
  const std::uint64_t u8_elapsed_ms{GetTickCount64() - u8_start_ms};

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kOplock);
  CHECK(ro_outcome.vec_closed.empty());
  CHECK(ro_outcome.str_detail.find(L"超时") != std::wstring::npos);
  CHECK(u8_elapsed_ms < 1500);
}

TEST_CASE("make_oplock_level：参与 oplock 协议的持有者响应中断让位即生效") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  HoldChildPtr up_child{
      spawn_hold_child_ex(str_hold_exe, str_file, false, true, false, 0)};
  REQUIRE(up_child);
  Sleep(300);
  REQUIRE(WaitForSingleObject(up_child->h_process, 0) == WAIT_TIMEOUT);

  const auto fn_level{ntfscompact::make_oplock_level(500)};
  const ntfscompact::ReleaseOutcome ro_outcome{fn_level(str_file)};

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kReleased);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kOplock);
  CHECK_FALSE(ntfscompact::probe_file_occupied(str_file));
  DWORD u4_code{0};
  CHECK(wait_child_exit(up_child->h_process, 15000, u4_code));
  CHECK(u4_code == 0);
}

TEST_CASE("make_escalating_releaser：RM 两级假败后句柄强关级可达（提权真实强关自启进程，非提权如实止步）") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  HoldChildPtr up_child{spawn_hold_child(str_hold_exe, str_file, false)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(str_file));

  ntfscompact::ReleaseFn fn_fake_graceful{
      [](const std::wstring&) {
        ntfscompact::ReleaseOutcome ro_busy{};
        ro_busy.e_status = ntfscompact::ReleaseOutcome::Status::kBusy;
        ro_busy.e_level = ntfscompact::ReleaseOutcome::Level::kRmGraceful;
        ro_busy.str_detail = L"RM 优雅关失败（模拟）";
        return ro_busy;
      }};
  ntfscompact::ReleaseFn fn_fake_force{
      [](const std::wstring&) {
        ntfscompact::ReleaseOutcome ro_busy{};
        ro_busy.e_status = ntfscompact::ReleaseOutcome::Status::kBusy;
        ro_busy.e_level = ntfscompact::ReleaseOutcome::Level::kRmForce;
        ro_busy.str_detail = L"RM 强制关失败（模拟）";
        return ro_busy;
      }};
  const auto xe_handle{ntfscompact::make_handle_force_level(true)};
  const auto xe_chain{ntfscompact::make_escalating_releaser(
      {ntfscompact::make_oplock_level(100), std::move(fn_fake_graceful),
       std::move(fn_fake_force), std::move(xe_handle)},
      ntfscompact::EscalationBackoff{})};
  REQUIRE(xe_chain.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_chain.value()(str_file)};

  if (ntfscompact::process_is_elevated()) {
    CHECK(ro_outcome.e_status ==
          ntfscompact::ReleaseOutcome::Status::kReleased);
    CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kHandle);
    CHECK_FALSE(closed_pid_name(ro_outcome.vec_closed, up_child->u4_pid)
                    .empty());
    CHECK_FALSE(ntfscompact::probe_file_occupied(str_file));
  } else {
    CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
    CHECK(has(narrow(ro_outcome.str_detail), "管理员"));
    CHECK(ro_outcome.vec_closed.empty());
    CHECK(ntfscompact::probe_file_occupied(str_file));
  }
}

TEST_CASE("make_real_releaser：空闲文件不进链且开关参数可传导") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"free.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"free.bin", 8192));

  const auto xe_releaser{ntfscompact::make_real_releaser(true)};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kFree);
}

TEST_CASE("make_real_releaser：rstrtmgr 加载失败注入 → 构造成功且链降级为两级，链尽如实报 RM 不可用已跳过") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"held.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"held.bin", 8192));
  HANDLE h_lock{CreateFileW(str_file.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  const auto xe_releaser{ntfscompact::make_real_releaser(
      false,
      std::unexpected(std::wstring{L"无法加载 rstrtmgr.dll（错误码 126）"}))};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CloseHandle(h_lock);

  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kBusy);
  CHECK(ro_outcome.e_level == ntfscompact::ReleaseOutcome::Level::kHandle);
  CHECK(has(narrow(ro_outcome.str_detail), "升级链尽后仍被占用"));
  CHECK(has(narrow(ro_outcome.str_detail), "RM 不可用已跳过"));
  CHECK(narrow(ro_outcome.str_detail).find("RM 优雅关") == std::string::npos);
  CHECK(narrow(ro_outcome.str_detail).find("RM 强制关") == std::string::npos);
}

TEST_CASE("make_real_releaser：rstrtmgr 加载失败注入 → 空闲文件照常 kFree 且成功路径不多话") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"free.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"free.bin", 8192));

  const auto xe_releaser{ntfscompact::make_real_releaser(
      false,
      std::unexpected(std::wstring{L"无法加载 rstrtmgr.dll（错误码 126）"}))};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kFree);
  CHECK(ro_outcome.str_detail.empty());
}

TEST_CASE("make_real_releaser：rstrtmgr 产物注入成功 → RM 两级入链且链尽不多话") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"free.bin"))};
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"free.bin", 8192));

  const auto xe_releaser{ntfscompact::make_real_releaser(
      false, ntfscompact::load_rstrtmgr_api())};
  REQUIRE(xe_releaser.has_value());
  const ntfscompact::ReleaseOutcome ro_outcome{xe_releaser.value()(str_file)};
  CHECK(ro_outcome.e_status == ntfscompact::ReleaseOutcome::Status::kFree);
  CHECK(ro_outcome.str_detail.empty());
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
