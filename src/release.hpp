#pragma once

import std;

#include "dynload.hpp"

namespace ntfscompact {

struct ClosedProcess {
  std::uint32_t u4_pid{0};
  std::wstring str_name{};
};

struct ReleaseOutcome {
  enum class Status { kFree, kReleased, kBusy };
  enum class Level { kNone, kOplock, kRmGraceful, kRmForce, kHandle };
  Status e_status{Status::kFree};
  Level e_level{Level::kNone};
  std::vector<ClosedProcess> vec_closed{};
  std::wstring str_detail{};
};

using ReleaseFn = std::function<ReleaseOutcome(const std::wstring& str_long_path)>;

struct EscalationBackoff {
  std::vector<std::uint32_t> vec_delays_ms{100, 300, 1000};
  std::uint32_t u4_budget_ms{5000};
  std::function<void(std::uint32_t)> fn_sleep{};
};

bool probe_file_occupied(const std::wstring& str_long_path);

bool process_is_elevated();

enum class ProcessRisk { kLow, kHigh, kUnknown };

ProcessRisk classify_process_risk(const std::wstring& str_process_name);

struct HandleOwner {
  std::uint32_t u4_pid{0};
  std::uintptr_t u8_handle{0};
  std::uint32_t u4_granted_access{0};
};

struct HandleForceDeps {
  std::function<bool()> fn_is_elevated{};
  std::function<std::vector<HandleOwner>(const std::wstring&)>
      fn_find_handles{};
  std::function<std::wstring(std::uint32_t)> fn_process_name{};
  std::function<bool(std::uint32_t, std::uintptr_t)> fn_close_handle{};
};

ReleaseFn make_handle_force_level(bool b_allow_all,
                                  const HandleForceDeps& deps = {});

ReleaseFn make_oplock_level(
    std::uint32_t u4_wait_timeout_ms = 2000,
    std::function<bool(const std::wstring&)> fn_probe = {});

std::wstring level_display_name(ReleaseOutcome::Level e_level);

std::wstring release_audit_entry(const ReleaseOutcome& ro_outcome);

std::expected<ReleaseFn, std::wstring> make_escalating_releaser(
    std::vector<ReleaseFn> vec_levels, const EscalationBackoff& eb_backoff);

std::expected<ReleaseFn, std::wstring> make_rm_graceful_level();

std::expected<ReleaseFn, std::wstring> make_rm_force_level();

std::expected<ReleaseFn, std::wstring> make_real_releaser(
    bool b_force_close_all = false);

std::expected<ReleaseFn, std::wstring> make_real_releaser(
    bool b_force_close_all, std::expected<RstrtmgrApi, std::wstring> xe_rm);

}
