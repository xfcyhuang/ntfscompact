#pragma once

import std;

#include <windows.h>

namespace ntfscompact {

struct ScanItem {
  std::wstring str_long_path{};
  std::wstring str_display_path{};
  std::uint64_t u8_size{};
};

struct ScanSkip {
  std::wstring str_display_path{};
  std::wstring str_reason{};
};

struct ScanError {
  std::wstring str_display_path{};
  std::wstring str_message{};
};

struct FlaggedDir {
  std::wstring str_long_path{};
  std::wstring str_display_path{};
};

struct ScanOutcome {
  std::vector<ScanItem> vec_items{};
  std::vector<ScanSkip> vec_skipped{};
  std::vector<ScanError> vec_errors{};
  std::vector<FlaggedDir> vec_flagged_dirs{};
};

struct FileMetadata {
  std::wstring str_display_path{};
  DWORD u4_attributes{};
  DWORD u4_reparse_tag{};
  std::uint64_t u8_size{};
};

bool check_file_candidate(const FileMetadata& fm_metadata,
                          std::wstring* ptr_skip_reason);

void scan_paths(const std::vector<std::wstring>& vec_raw_paths,
                ScanOutcome& so_out);

}
