#pragma once

import std;

namespace ntfscompact {

inline constexpr wchar_t kVersion[]{L"0.1.0"};

enum class Algorithm {
  kXpress4k,
  kXpress8k,
  kXpress16k,
  kLzx,
};

struct Options {
  std::vector<std::wstring> vec_paths{};
  Algorithm e_algorithm{Algorithm::kXpress8k};
  bool b_uncompress{false};
  bool b_dry_run{false};
  bool b_quiet{false};
  bool b_verbose{false};

};

struct ParseResult {
  bool b_want_help{false};
  bool b_want_version{false};
  bool b_ok{false};
  std::wstring str_error{};
  Options opt_options{};
};

ParseResult parse_args(const std::vector<std::wstring>& vec_args);

std::wstring usage_text();

}
