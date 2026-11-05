#pragma once

import std;

#include <windows.h>

namespace ntfscompact_test {

struct TempDirState {
  std::wstring str_dir{};
  bool b_ok{false};
};

inline std::wstring temp_long_api_path(const std::wstring& str_absolute) {
  if (str_absolute.rfind(L"\\\\?\\", 0) == 0) return str_absolute;
  return L"\\\\?\\" + str_absolute;
}

inline void temp_remove_recursive(const std::wstring& str_long_dir) {
  const std::wstring str_pattern{str_long_dir + L"\\*"};
  WIN32_FIND_DATAW fd_find{};
  HANDLE h_find{FindFirstFileW(str_pattern.c_str(), &fd_find)};
  if (h_find != INVALID_HANDLE_VALUE) {
    do {
      if (wcscmp(fd_find.cFileName, L".") == 0 ||
          wcscmp(fd_find.cFileName, L"..") == 0) {
        continue;
      }
      const std::wstring str_child{str_long_dir + L"\\" + fd_find.cFileName};
      if ((fd_find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          !(fd_find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        temp_remove_recursive(str_child);
      } else {
        DeleteFileW(str_child.c_str());
        RemoveDirectoryW(str_child.c_str());
      }
    } while (FindNextFileW(h_find, &fd_find));
    FindClose(h_find);
  }
  RemoveDirectoryW(str_long_dir.c_str());
}

struct TempDirReleaser {
  void operator()(TempDirState* ptr_state) const noexcept {
    if (ptr_state != nullptr && ptr_state->b_ok) {
      temp_remove_recursive(temp_long_api_path(ptr_state->str_dir));
    }
    delete ptr_state;
  }
};

using TempDir = std::unique_ptr<TempDirState, TempDirReleaser>;

[[nodiscard]] inline TempDir make_temp_dir() {
  auto up_state{std::make_unique<TempDirState>()};
  wchar_t arr_base[MAX_PATH]{};
  const DWORD u4_len{GetTempPathW(MAX_PATH, arr_base)};
  if (u4_len == 0 || u4_len >= MAX_PATH) {
    return TempDir{up_state.release()};
  }
  static unsigned u4_counter{};
  const std::wstring str_tail{std::format(
      L"ntfscompact-t3-{}-{}", static_cast<unsigned long>(GetCurrentProcessId()),
      u4_counter++)};
  up_state->str_dir = std::wstring(arr_base, u4_len) + str_tail;
  if (!CreateDirectoryW(up_state->str_dir.c_str(), nullptr)) {
    up_state->str_dir.clear();
    return TempDir{up_state.release()};
  }
  up_state->b_ok = true;
  return TempDir{up_state.release()};
}

inline std::wstring temp_path(const TempDir& up_dir) {
  return up_dir->str_dir;
}

inline std::wstring temp_sub(const TempDir& up_dir, const std::wstring& str_name) {
  return up_dir->str_dir + L"\\" + str_name;
}

inline bool temp_make_dir(const TempDir& up_dir, const std::wstring& str_sub) {
  const std::wstring str_full{temp_sub(up_dir, str_sub)};
  if (CreateDirectoryW(temp_long_api_path(str_full).c_str(), nullptr)) {
    return true;
  }
  std::wstring str_cur{};
  std::size_t u8_index{0};
  while (u8_index < str_full.size()) {
    const std::size_t u8_next{str_full.find(L'\\', u8_index)};
    if (u8_next == std::wstring::npos) break;
    if (u8_next > 0) {
      str_cur = str_full.substr(0, u8_next);
      CreateDirectoryW(temp_long_api_path(str_cur).c_str(), nullptr);
    }
    u8_index = u8_next + 1;
  }
  return CreateDirectoryW(temp_long_api_path(str_full).c_str(), nullptr) ||
         ((GetFileAttributesW(temp_long_api_path(str_full).c_str()) &
           FILE_ATTRIBUTE_DIRECTORY) != 0);
}

inline bool temp_write_file(const TempDir& up_dir, const std::wstring& str_sub,
                            unsigned long u4_size) {
  const std::wstring str_full{temp_sub(up_dir, str_sub)};
  HANDLE h_file{CreateFileW(temp_long_api_path(str_full).c_str(),
                            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  static const char kBuffer[65536]{};
  unsigned long u4_left{u4_size};
  while (u4_left > 0) {
    const DWORD u4_chunk{u4_left > sizeof(kBuffer)
                             ? static_cast<DWORD>(sizeof(kBuffer))
                             : static_cast<DWORD>(u4_left)};
    DWORD u4_written{};
    if (!::WriteFile(h_file, kBuffer, u4_chunk, &u4_written, nullptr) ||
        u4_written != u4_chunk) {
      CloseHandle(h_file);
      return false;
    }
    u4_left -= u4_chunk;
  }
  CloseHandle(h_file);
  return true;
}

}
