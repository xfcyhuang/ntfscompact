import std;

#include "dir_flag.hpp"
#include "handle_guard.hpp"

#include <windows.h>

namespace ntfscompact {
namespace {

auto open_dir_read_write(const std::wstring& str_long_path) -> HandlePtr {
  return HandlePtr{CreateFileW(
      str_long_path.c_str(), FILE_WRITE_DATA | FILE_LIST_DIRECTORY | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
}

}

auto clear_dir_flag(const std::wstring& str_long_path) -> std::expected<void, std::wstring> {
  HandlePtr ptr_dir{open_dir_read_write(str_long_path)};
  if (ptr_dir.get() == INVALID_HANDLE_VALUE) {
    const DWORD u4_open_err{GetLastError()};
    return std::unexpected(std::format(L"无法打开目录（错误码 {}）", u4_open_err));
  }
  const DWORD u4_ioctl_err{set_compression_none(ptr_dir.get())};
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
