import std;

#include "dir_flag.hpp"

#include <windows.h>
#include <winioctl.h>

namespace ntfscompact {
namespace {

struct HandleCloser {
  auto operator()(HANDLE h_raw) const noexcept -> void {
    if (h_raw != INVALID_HANDLE_VALUE) {
      CloseHandle(h_raw);
    }
  }
};

using HandlePtr = std::unique_ptr<void, HandleCloser>;

auto open_dir_write(const std::wstring& str_long_path) -> HandlePtr {
  return HandlePtr{CreateFileW(str_long_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                               nullptr)};
}

auto run_clear_flag(HANDLE h_dir) -> DWORD {
  USHORT u2_format{COMPRESSION_FORMAT_NONE};
  DWORD u4_returned{};
  const BOOL b_ok{DeviceIoControl(h_dir, FSCTL_SET_COMPRESSION, &u2_format,
                                  sizeof(u2_format), nullptr, 0, &u4_returned,
                                  nullptr)};
  return b_ok ? 0 : GetLastError();
}

}

auto clear_dir_flag(const std::wstring& str_long_path) -> std::expected<void, std::wstring> {
  HandlePtr ptr_dir{open_dir_write(str_long_path)};
  if (ptr_dir.get() == INVALID_HANDLE_VALUE) {
    const DWORD u4_open_err{GetLastError()};
    return std::unexpected(std::format(L"无法打开目录（错误码 {}）", u4_open_err));
  }
  const DWORD u4_ioctl_err{run_clear_flag(ptr_dir.get())};
  if (u4_ioctl_err != 0) {
    return std::unexpected(std::format(
        L"清除目录压缩标记失败（FSCTL_SET_COMPRESSION 错误码 {}）", u4_ioctl_err));
  }
  return {};
}

auto make_real_dir_flag_clearer() -> std::expected<DirFlagFn, std::wstring> {
  return DirFlagFn{[](const std::wstring& str_long_path)
                       -> std::expected<void, std::wstring> {
    return clear_dir_flag(str_long_path);
  }};
}

}
