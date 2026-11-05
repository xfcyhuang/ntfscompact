#pragma once

import std;

#include <windows.h>
#include <cstdlib>

namespace ntfscompact_test {

inline std::string narrow(const std::wstring& str_wide) {
  const int i4_bytes{WideCharToMultiByte(CP_UTF8, 0, str_wide.data(),
                                         static_cast<int>(str_wide.size()),
                                         nullptr, 0, nullptr, nullptr)};
  std::string str_utf8(static_cast<std::size_t>(i4_bytes), '\0');
  if (i4_bytes > 0) {
    WideCharToMultiByte(CP_UTF8, 0, str_wide.data(),
                        static_cast<int>(str_wide.size()), str_utf8.data(),
                        i4_bytes, nullptr, nullptr);
  }
  return str_utf8;
}

inline int run_compact(const std::wstring& str_args) {
  const std::wstring str_cmd{std::format(L"compact {} >nul 2>&1", str_args)};
  return _wsystem(str_cmd.c_str());
}

}
