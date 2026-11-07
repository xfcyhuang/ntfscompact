#include "release.hpp"

#include <windows.h>

namespace ntfscompact {

bool probe_file_occupied(const std::wstring& str_long_path) {
  HANDLE h_probe{CreateFileW(
      str_long_path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_ATTRIBUTE_NORMAL,
      nullptr)};
  if (h_probe != INVALID_HANDLE_VALUE) {
    CloseHandle(h_probe);
    return false;
  }
  return GetLastError() == ERROR_SHARING_VIOLATION;
}

bool dismount_volume_for(const std::wstring& str_long_path) {
  wchar_t arr_volume[MAX_PATH]{};
  if (!GetVolumePathNameW(str_long_path.c_str(), arr_volume, MAX_PATH)) {
    return false;
  }
  std::wstring str_volume{arr_volume};
  if (str_volume.size() < 2 || str_volume[1] != L':') {
    return false;
  }
  const std::wstring str_root{str_volume.substr(0, 2)};
  const DWORD u4_attrs{GetFileAttributesW(str_root.c_str())};
  if (u4_attrs == INVALID_FILE_ATTRIBUTES ||
      (u4_attrs & FILE_ATTRIBUTE_READONLY) != 0) {
    return false;
  }
  const std::wstring str_device{L"\\.\\" + str_root};
  HANDLE h_volume{CreateFileW(str_device.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr)};
  if (h_volume == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD u4_bytes{0};
  const BOOL b_ok{DeviceIoControl(h_volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0,
                                  nullptr, 0, &u4_bytes, nullptr)};
  CloseHandle(h_volume);
  return b_ok != FALSE;
}

bool process_is_elevated() {
  HANDLE h_token{nullptr};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h_token)) {
    return false;
  }
  TOKEN_ELEVATION te_elevation{};
  DWORD u4_len{0};
  const BOOL b_ok{GetTokenInformation(h_token, TokenElevation, &te_elevation,
                                      sizeof(te_elevation), &u4_len)};
  CloseHandle(h_token);
  return b_ok != FALSE && te_elevation.TokenIsElevated != 0;
}

namespace {

std::uint32_t escalation_delay_ms(const EscalationBackoff& eb_backoff,
                                  std::size_t u8_escalation,
                                  std::uint32_t u4_spent_ms) {
  if (eb_backoff.vec_delays_ms.empty()) {
    return 0;
  }
  const std::size_t u8_index{
      u8_escalation < eb_backoff.vec_delays_ms.size()
          ? u8_escalation
          : eb_backoff.vec_delays_ms.size() - 1};
  if (u4_spent_ms >= eb_backoff.u4_budget_ms) {
    return 0;
  }
  const std::uint32_t u4_remaining{eb_backoff.u4_budget_ms - u4_spent_ms};
  const std::uint32_t u4_delay{eb_backoff.vec_delays_ms[u8_index]};
  return u4_delay > u4_remaining ? u4_remaining : u4_delay;
}

ReleaseOutcome run_release_chain(const std::vector<ReleaseFn>& vec_levels,
                                 const std::wstring& str_long_path,
                                 const EscalationBackoff& eb_backoff) {
  const std::function<void(std::uint32_t)> fn_sleep_default{
      [](std::uint32_t u4_ms) { Sleep(u4_ms); }};
  const std::function<void(std::uint32_t)>& fn_sleep{
      eb_backoff.fn_sleep ? eb_backoff.fn_sleep : fn_sleep_default};
  ReleaseOutcome ro_merged{};
  ReleaseOutcome::Level e_last_level{ReleaseOutcome::Level::kNone};
  std::wstring str_last_detail{};
  std::uint32_t u4_spent{0};
  for (std::size_t u8_index{0}; u8_index < vec_levels.size(); ++u8_index) {
    if (u8_index > 0) {
      const std::uint32_t u4_delay{
          escalation_delay_ms(eb_backoff, u8_index - 1, u4_spent)};
      if (u4_delay > 0) {
        fn_sleep(u4_delay);
        u4_spent += u4_delay;
      }
      if (!probe_file_occupied(str_long_path)) {
        ReleaseOutcome ro_landed{std::move(ro_merged)};
        ro_landed.e_status = ReleaseOutcome::Status::kReleased;
        ro_landed.e_level = e_last_level;
        ro_landed.str_detail = L"经退避探测确认释放";
        return ro_landed;
      }
    }
    const ReleaseOutcome ro_step{vec_levels[u8_index](str_long_path)};
    ro_merged.vec_closed.insert(ro_merged.vec_closed.end(),
                                ro_step.vec_closed.begin(),
                                ro_step.vec_closed.end());
    if (ro_step.e_level != ReleaseOutcome::Level::kNone) {
      e_last_level = ro_step.e_level;
    }
    if (!ro_step.str_detail.empty()) {
      str_last_detail = ro_step.str_detail;
    }
    if (ro_step.e_status != ReleaseOutcome::Status::kBusy) {
      ReleaseOutcome ro_done{std::move(ro_merged)};
      ro_done.e_status = ReleaseOutcome::Status::kReleased;
      ro_done.e_level = e_last_level;
      ro_done.str_detail = ro_step.str_detail;
      return ro_done;
    }
  }
  ReleaseOutcome ro_busy{std::move(ro_merged)};
  ro_busy.e_status = ReleaseOutcome::Status::kBusy;
  ro_busy.e_level = e_last_level;
  ro_busy.str_detail = std::wstring{L"占用不可释放：升级链尽后仍被占用"};
  if (!str_last_detail.empty()) {
    ro_busy.str_detail += L"（" + str_last_detail + L"）";
  }
  return ro_busy;
}

}

std::wstring level_display_name(ReleaseOutcome::Level e_level) {
  switch (e_level) {
    case ReleaseOutcome::Level::kOplock:
      return L"Oplock 协商";
    case ReleaseOutcome::Level::kRmGraceful:
      return L"RM 优雅关";
    case ReleaseOutcome::Level::kRmForce:
      return L"RM 强制关";
    case ReleaseOutcome::Level::kHandle:
      return L"句柄强关";
    case ReleaseOutcome::Level::kNone:
      return L"探测释放";
  }
  return L"探测释放";
}

std::expected<ReleaseFn, std::wstring> make_escalating_releaser(
    std::vector<ReleaseFn> vec_levels, const EscalationBackoff& eb_backoff) {
  return ReleaseFn{
      [vec_levels = std::move(vec_levels), eb_backoff](
          const std::wstring& str_long_path) -> ReleaseOutcome {
        if (!probe_file_occupied(str_long_path)) {
          return ReleaseOutcome{};
        }
        return run_release_chain(vec_levels, str_long_path, eb_backoff);
      }};
}

std::expected<ReleaseFn, std::wstring> make_real_releaser(
    const DismountFn& fn_dismount) {
  if (!fn_dismount) {
    return std::unexpected(L"卷卸载函数未接线");
  }
  return ReleaseFn{[fn_dismount](
                       const std::wstring& str_long_path) -> ReleaseOutcome {
    if (!probe_file_occupied(str_long_path)) {
      ReleaseOutcome ro_free{};
      ro_free.e_status = ReleaseOutcome::Status::kFree;
      return ro_free;
    }
    if (!process_is_elevated()) {
      ReleaseOutcome ro_busy{};
      ro_busy.e_status = ReleaseOutcome::Status::kBusy;
      ro_busy.str_detail = L"占用不可释放：直接卷卸载需管理员权限";
      return ro_busy;
    }
    if (fn_dismount(str_long_path) && !probe_file_occupied(str_long_path)) {
      ReleaseOutcome ro_ok{};
      ro_ok.e_status = ReleaseOutcome::Status::kReleased;
      ro_ok.b_dismounted = true;
      ro_ok.str_detail = L"经卷卸载释放";
      return ro_ok;
    }
    ReleaseOutcome ro_busy{};
    ro_busy.e_status = ReleaseOutcome::Status::kBusy;
    ro_busy.str_detail = L"占用不可释放：卷卸载失败或卸载后仍被占用";
    return ro_busy;
  }};
}

std::expected<ReleaseFn, std::wstring> make_real_releaser() {
  return make_real_releaser(&dismount_volume_for);
}

}
