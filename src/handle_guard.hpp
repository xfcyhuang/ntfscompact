#pragma once

import std;

#include <windows.h>
#include <winioctl.h>

namespace ntfscompact {

struct HandleCloser {
  auto operator()(HANDLE h_raw) const noexcept -> void {
    if (h_raw != INVALID_HANDLE_VALUE) {
      CloseHandle(h_raw);
    }
  }
};

using HandlePtr = std::unique_ptr<void, HandleCloser>;

inline DWORD set_compression_none(HANDLE h_target) {
  USHORT u2_format{COMPRESSION_FORMAT_NONE};
  DWORD u4_returned{};
  const BOOL b_ok{DeviceIoControl(h_target, FSCTL_SET_COMPRESSION, &u2_format,
                                  sizeof(u2_format), nullptr, 0, &u4_returned,
                                  nullptr)};
  return b_ok ? 0 : GetLastError();
}

}
