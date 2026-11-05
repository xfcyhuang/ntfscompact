#pragma once

import std;

namespace ntfscompact {

struct ReleaseOutcome {
  enum class Status { kFree, kReleased, kBusy };
  Status e_status{Status::kFree};
  bool b_dismounted{false};
  std::wstring str_detail{};
};

using DismountFn = std::function<bool(const std::wstring& str_long_path)>;
using ReleaseFn = std::function<ReleaseOutcome(const std::wstring& str_long_path)>;

bool probe_file_occupied(const std::wstring& str_long_path);

bool dismount_volume_for(const std::wstring& str_long_path);

bool process_is_elevated();

std::expected<ReleaseFn, std::wstring> make_real_releaser(
    const DismountFn& fn_dismount);

std::expected<ReleaseFn, std::wstring> make_real_releaser();

}
