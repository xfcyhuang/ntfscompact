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
