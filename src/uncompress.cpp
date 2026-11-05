import std;

#include "uncompress.hpp"

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

auto open_read_write(const std::wstring& str_long_path) -> HandlePtr {
  return HandlePtr{CreateFileW(str_long_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr)};
}

auto run_delete_backing(HANDLE h_file) -> DWORD {
  DWORD u4_returned{};
  const BOOL b_ok{DeviceIoControl(h_file, FSCTL_DELETE_EXTERNAL_BACKING, nullptr, 0,
                                  nullptr, 0, &u4_returned, nullptr)};
  return b_ok ? 0 : GetLastError();
}

auto run_clear_lznt1(HANDLE h_file) -> DWORD {
  USHORT u2_format{COMPRESSION_FORMAT_NONE};
  DWORD u4_returned{};
  const BOOL b_ok{DeviceIoControl(h_file, FSCTL_SET_COMPRESSION, &u2_format,
                                  sizeof(u2_format), nullptr, 0, &u4_returned, nullptr)};
  return b_ok ? 0 : GetLastError();
}

auto describe_recheck(const CurrentState& st_after, DWORD u4_ioctl_err) -> std::wstring {
  return std::format(L"解压后复核不符：仍为 {}（执行错误码 {}）", describe_state(st_after),
                     u4_ioctl_err);
}

}

auto uncompress_file(const std::wstring& str_long_path,
                     const DetectFn& fn_detect) -> std::expected<void, std::wstring> {
  const DetectionResult st_before{fn_detect(str_long_path)};
  if (!st_before.str_error.empty()) {
    return std::unexpected(L"解压前现状检测失败：" + st_before.str_error);
  }
  if (st_before.cs_state.e_kind == CurrentState::Kind::kNone) {
    return {};
  }
  if (st_before.cs_state.e_kind == CurrentState::Kind::kWof &&
      st_before.cs_state.i4_provider != CurrentState::kProviderFile) {
    return std::unexpected(L"WIM/Cloud backing 文件不解除（黑名单）");
  }
  const DWORD u4_attrs{GetFileAttributesW(str_long_path.c_str())};
  if (u4_attrs == INVALID_FILE_ATTRIBUTES) {
    return std::unexpected(std::format(L"无法读取文件属性（错误码 {}）", GetLastError()));
  }
  const bool b_readonly{(u4_attrs & FILE_ATTRIBUTE_READONLY) != 0};
  if (b_readonly &&
      !SetFileAttributesW(str_long_path.c_str(), u4_attrs & ~FILE_ATTRIBUTE_READONLY)) {
    return std::unexpected(std::format(L"临时清除只读属性失败（错误码 {}）", GetLastError()));
  }
  HandlePtr ptr_file{open_read_write(str_long_path)};
  if (ptr_file.get() == INVALID_HANDLE_VALUE) {
    const DWORD u4_open_err{GetLastError()};
    if (b_readonly) {
      SetFileAttributesW(str_long_path.c_str(), u4_attrs);
    }
    return std::unexpected(std::format(L"无法以读写访问打开文件（错误码 {}）", u4_open_err));
  }
  const DWORD u4_ioctl_err{st_before.cs_state.e_kind == CurrentState::Kind::kWof
                               ? run_delete_backing(ptr_file.get())
                               : run_clear_lznt1(ptr_file.get())};
  ptr_file.reset();
  if (b_readonly && !SetFileAttributesW(str_long_path.c_str(), u4_attrs)) {
    return std::unexpected(std::format(L"恢复只读属性失败（错误码 {}）", GetLastError()));
  }
  const DetectionResult st_after{fn_detect(str_long_path)};
  if (!st_after.str_error.empty()) {
    return std::unexpected(std::format(L"解压后复核失败：{}（执行错误码 {}）", st_after.str_error,
                                       u4_ioctl_err));
  }
  if (st_after.cs_state.e_kind != CurrentState::Kind::kNone) {
    return std::unexpected(describe_recheck(st_after.cs_state, u4_ioctl_err));
  }
  return {};
}

auto make_real_uncompressor() -> std::expected<UncompressFn, std::wstring> {
  auto xe_detector{make_real_detector()};
  if (!xe_detector.has_value()) {
    return std::unexpected(std::move(xe_detector).error());
  }
  DetectFn fn_detector{std::move(xe_detector).value()};
  UncompressFn fn_uncompress{
      [fn_detector = std::move(fn_detector)](const std::wstring& str_long_path)
          -> std::expected<void, std::wstring> {
        return uncompress_file(str_long_path, fn_detector);
      }};
  return fn_uncompress;
}

}

