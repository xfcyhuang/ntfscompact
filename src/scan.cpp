#include "scan.hpp"

namespace ntfscompact {
namespace {

constexpr std::uint64_t kMinCompressibleSize{4096};

#ifndef IO_REPARSE_TAG_WOF
#define IO_REPARSE_TAG_WOF (0x80000017u)
#endif

bool is_critical_system_file(const std::wstring& str_display_path) {
  const std::size_t u8_sep{str_display_path.find_last_of(L"\\/")};
  const std::wstring str_name{u8_sep == std::wstring::npos
                                  ? str_display_path
                                  : str_display_path.substr(u8_sep + 1)};
  return lstrcmpiW(str_name.c_str(), L"pagefile.sys") == 0 ||
         lstrcmpiW(str_name.c_str(), L"hiberfil.sys") == 0 ||
         lstrcmpiW(str_name.c_str(), L"swapfile.sys") == 0;
}

bool has_winsxs_component(const std::wstring& str_display_path) {
  std::size_t u8_begin{0};
  while (u8_begin <= str_display_path.size()) {
    const std::size_t u8_end{str_display_path.find_first_of(L"\\/", u8_begin)};
    const std::wstring str_comp{
        str_display_path.substr(u8_begin, u8_end == std::wstring::npos
                                             ? std::wstring::npos
                                             : u8_end - u8_begin)};
    if (!str_comp.empty() && lstrcmpiW(str_comp.c_str(), L"WinSxS") == 0) {
      return true;
    }
    if (u8_end == std::wstring::npos) break;
    u8_begin = u8_end + 1;
  }
  return false;
}

std::wstring to_long_path(const std::wstring& str_raw) {
  if (str_raw.rfind(L"\\\\?\\", 0) == 0) return str_raw;
  wchar_t arr_buffer[32768]{};
  const DWORD u4_len{GetFullPathNameW(str_raw.c_str(), 32768, arr_buffer,
                                      nullptr)};
  if (u4_len == 0 || u4_len >= 32768) return str_raw;
  const std::wstring str_full{arr_buffer, u4_len};
  if (str_full.rfind(L"\\\\", 0) == 0) {
    return L"\\\\?\\UNC" + str_full.substr(1);
  }
  return L"\\\\?\\" + str_full;
}

std::wstring to_display_path(const std::wstring& str_long_path) {
  if (str_long_path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
    return L"\\" + str_long_path.substr(7);
  }
  if (str_long_path.rfind(L"\\\\?\\", 0) == 0) {
    return str_long_path.substr(4);
  }
  return str_long_path;
}

bool has_wildcard(const std::wstring& str_path) {
  return str_path.find_first_of(L"*?") != std::wstring::npos;
}

std::uint64_t find_data_size(const WIN32_FIND_DATAW& fd_find) {
  ULARGE_INTEGER u8_size{};
  u8_size.LowPart = fd_find.nFileSizeLow;
  u8_size.HighPart = fd_find.nFileSizeHigh;
  return u8_size.QuadPart;
}

FileMetadata make_file_metadata(const std::wstring& str_display_path,
                                const WIN32_FIND_DATAW& fd_find) {
  FileMetadata fm_meta{};
  fm_meta.str_display_path = str_display_path;
  fm_meta.u4_attributes = fd_find.dwFileAttributes;
  fm_meta.u4_reparse_tag = fd_find.dwReserved0;
  fm_meta.u8_size = find_data_size(fd_find);
  return fm_meta;
}

bool is_directory(const WIN32_FIND_DATAW& fd_find) {
  return (fd_find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_reparse(const WIN32_FIND_DATAW& fd_find) {
  return (fd_find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool is_marked_compressed(const WIN32_FIND_DATAW& fd_find) {
  return (fd_find.dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0;
}

void walk_directory(const std::wstring& str_long_dir,
                    const std::wstring& str_display_dir, ScanOutcome& so_out) {
  const std::wstring str_pattern{str_long_dir + L"\\*"};
  WIN32_FIND_DATAW fd_find{};
  HANDLE h_find{FindFirstFileW(str_pattern.c_str(), &fd_find)};
  if (h_find == INVALID_HANDLE_VALUE) {
    so_out.vec_errors.push_back(
        {str_display_dir, L"无法访问（不存在或权限不足）"});
    return;
  }
  do {
    if (wcscmp(fd_find.cFileName, L".") == 0 ||
        wcscmp(fd_find.cFileName, L"..") == 0) {
      continue;
    }
    const std::wstring str_child_long{str_long_dir + L"\\" + fd_find.cFileName};
    const std::wstring str_child_disp{str_display_dir + L"\\" +
                                      fd_find.cFileName};
    if (is_directory(fd_find)) {
      if (is_reparse(fd_find)) {
        so_out.vec_skipped.push_back(
            {str_child_disp, L"目录为符号链接/junction，不进入"});
      } else if (has_winsxs_component(str_child_disp)) {
        so_out.vec_skipped.push_back({str_child_disp, L"位于 WinSxS 目录"});
      } else {
        if (is_marked_compressed(fd_find)) {
          so_out.vec_flagged_dirs.push_back({str_child_long, str_child_disp});
        }
        walk_directory(str_child_long, str_child_disp, so_out);
      }
    } else {
      const FileMetadata fm_meta{make_file_metadata(str_child_disp, fd_find)};
      std::wstring str_reason{};
      if (check_file_candidate(fm_meta, &str_reason)) {
        so_out.vec_items.push_back(
            {str_child_long, str_child_disp, fm_meta.u8_size});
      } else {
        so_out.vec_skipped.push_back({str_child_disp, str_reason});
      }
    }
  } while (FindNextFileW(h_find, &fd_find));
  FindClose(h_find);
}

void handle_explicit_path(const std::wstring& str_raw, ScanOutcome& so_out) {
  const std::wstring str_long_path{to_long_path(str_raw)};
  const std::wstring str_display{to_display_path(str_long_path)};
  WIN32_FIND_DATAW fd_find{};
  HANDLE h_find{FindFirstFileW(str_long_path.c_str(), &fd_find)};
  if (h_find == INVALID_HANDLE_VALUE) {
    so_out.vec_errors.push_back(
        {str_display, L"无法访问（不存在或权限不足）"});
    return;
  }
  FindClose(h_find);
  if (is_directory(fd_find)) {
    if (is_reparse(fd_find)) {
      so_out.vec_skipped.push_back(
          {str_display, L"目录为符号链接/junction，不进入"});
    } else {
      if (!has_winsxs_component(str_display) && is_marked_compressed(fd_find)) {
        so_out.vec_flagged_dirs.push_back({str_long_path, str_display});
      }
      walk_directory(str_long_path, str_display, so_out);
    }
    return;
  }
  FileMetadata fm_meta{make_file_metadata(str_display, fd_find)};
  std::wstring str_reason{};
  if (check_file_candidate(fm_meta, &str_reason)) {
    so_out.vec_items.push_back({str_long_path, str_display, fm_meta.u8_size});
  } else {
    so_out.vec_skipped.push_back({str_display, str_reason});
  }
}

void handle_wildcard(const std::wstring& str_raw, ScanOutcome& so_out) {
  const std::size_t u8_sep{str_raw.find_last_of(L"\\/")};
  const std::wstring str_file_pattern{
      u8_sep == std::wstring::npos ? str_raw : str_raw.substr(u8_sep + 1)};
  const std::wstring str_dir_part{[&]() -> std::wstring {
    if (u8_sep == std::wstring::npos) return L".";
    if (u8_sep == 0) return L"\\";
    return str_raw.substr(0, u8_sep);
  }()};
  const std::wstring str_parent_long{to_long_path(str_dir_part)};
  const std::wstring str_parent_disp{to_display_path(str_parent_long)};

  WIN32_FIND_DATAW fd_find{};
  HANDLE h_find{
      FindFirstFileW((str_parent_long + L"\\" + str_file_pattern).c_str(),
                     &fd_find)};
  if (h_find == INVALID_HANDLE_VALUE) {
    so_out.vec_errors.push_back(
        {str_parent_disp + L"\\" + str_file_pattern,
         L"无法访问（不存在或权限不足）"});
    return;
  }
  do {
    if (is_directory(fd_find)) continue;
    const std::wstring str_child_disp{str_parent_disp + L"\\" +
                                      fd_find.cFileName};
    const FileMetadata fm_meta{make_file_metadata(str_child_disp, fd_find)};
    std::wstring str_reason{};
    if (check_file_candidate(fm_meta, &str_reason)) {
      so_out.vec_items.push_back(
          {str_parent_long + L"\\" + fd_find.cFileName, str_child_disp,
           fm_meta.u8_size});
    } else {
      so_out.vec_skipped.push_back({str_child_disp, str_reason});
    }
  } while (FindNextFileW(h_find, &fd_find));
  FindClose(h_find);
}

}

bool check_file_candidate(const FileMetadata& fm_metadata,
                          std::wstring* ptr_skip_reason) {
  if (is_critical_system_file(fm_metadata.str_display_path)) {
    const std::size_t u8_sep{fm_metadata.str_display_path.find_last_of(L"\\/")};
    *ptr_skip_reason =
        L"系统关键文件（" +
        (u8_sep == std::wstring::npos
             ? fm_metadata.str_display_path
             : fm_metadata.str_display_path.substr(u8_sep + 1)) +
        L"）";
    return false;
  }
  const std::size_t u8_sep{fm_metadata.str_display_path.find_last_of(L"\\/")};
  const std::wstring str_dir_part{
      u8_sep == std::wstring::npos
          ? std::wstring()
          : fm_metadata.str_display_path.substr(0, u8_sep)};
  if (has_winsxs_component(str_dir_part)) {
    *ptr_skip_reason = L"位于 WinSxS 目录";
    return false;
  }
  if (fm_metadata.u4_attributes & FILE_ATTRIBUTE_ENCRYPTED) {
    *ptr_skip_reason = L"EFS 加密文件";
    return false;
  }
  if (fm_metadata.u4_attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    if (fm_metadata.u4_reparse_tag != IO_REPARSE_TAG_WOF) {
      *ptr_skip_reason = L"非常规 reparse point（符号链接/junction/Cloud 等）";
      return false;
    }
  }
  if (fm_metadata.u8_size <= kMinCompressibleSize) {
    *ptr_skip_reason = L"文件大小 ≤4KB";
    return false;
  }
  return true;
}

void scan_paths(const std::vector<std::wstring>& vec_raw_paths,
                ScanOutcome& so_out) {
  for (const std::wstring& str_raw : vec_raw_paths) {
    if (has_wildcard(str_raw)) {
      handle_wildcard(str_raw, so_out);
    } else {
      handle_explicit_path(str_raw, so_out);
    }
  }
}

}
