#pragma once

import std;

#include <windows.h>

namespace ntfscompact {

enum class AttrStage { kRead, kClear, kRestore };

struct AttrFailure {
  AttrStage e_stage{AttrStage::kRead};
  DWORD u4_error{0};
};

struct ReadonlyAttrState {
  bool b_readonly{false};
  DWORD u4_attrs{0};
};

inline std::wstring attr_failure_message(AttrStage e_stage, DWORD u4_error) {
  switch (e_stage) {
    case AttrStage::kRead:
      return std::format(L"无法读取文件属性（错误码 {}）", u4_error);
    case AttrStage::kClear:
      return std::format(L"临时清除只读属性失败（错误码 {}）", u4_error);
    case AttrStage::kRestore:
      return std::format(L"恢复只读属性失败（错误码 {}）", u4_error);
  }
  return L"";
}

inline std::expected<ReadonlyAttrState, AttrFailure> begin_readonly_guard(
    const std::wstring& str_long_path) {
  const DWORD u4_attrs{GetFileAttributesW(str_long_path.c_str())};
  if (u4_attrs == INVALID_FILE_ATTRIBUTES) {
    return std::unexpected(AttrFailure{AttrStage::kRead, GetLastError()});
  }
  ReadonlyAttrState ra_attr{};
  ra_attr.u4_attrs = u4_attrs;
  ra_attr.b_readonly = (u4_attrs & FILE_ATTRIBUTE_READONLY) != 0;
  if (ra_attr.b_readonly &&
      !SetFileAttributesW(str_long_path.c_str(),
                          u4_attrs & ~FILE_ATTRIBUTE_READONLY)) {
    return std::unexpected(AttrFailure{AttrStage::kClear, GetLastError()});
  }
  return ra_attr;
}

inline bool end_readonly_guard(const ReadonlyAttrState& ra_attr,
                               const std::wstring& str_long_path,
                               AttrFailure* ptr_failure) {
  if (!ra_attr.b_readonly) {
    return true;
  }
  if (!SetFileAttributesW(str_long_path.c_str(), ra_attr.u4_attrs)) {
    *ptr_failure = AttrFailure{AttrStage::kRestore, GetLastError()};
    return false;
  }
  return true;
}

}
