#pragma once

import std;

#include "cli.hpp"
#include "detect.hpp"

namespace ntfscompact {

struct ExecuteResult {
  enum class Outcome { kSuccess, kSkipped, kError };
  Outcome e_outcome{Outcome::kSuccess};
  std::wstring str_message{};
  unsigned int u4_error{0};
};

using ExecuteFn = std::function<ExecuteResult(const std::wstring& str_long_path,
                                              Plan::Action e_action,
                                              const CurrentState& cs_current,
                                              Algorithm e_target)>;

std::expected<ExecuteFn, std::wstring> make_real_executor();

std::wstring check_compression_meets_target(const CurrentState& cs_actual,
                                            Algorithm e_target);

inline std::wstring check_compression_meets_target(const CurrentState& cs_actual,
                                                   Algorithm e_target) {
  if (cs_actual.e_kind != CurrentState::Kind::kWof) {
    return L"复核失败：压缩后文件未处于 WOF backing（可能被系统静默跳过）";
  }
  if (cs_actual.i4_provider != CurrentState::kProviderFile) {
    return std::format(L"复核失败：backing provider 为 {}（期望 WOF FILE）",
                       cs_actual.i4_provider);
  }
  if (cs_actual.e_algorithm != e_target) {
    return std::format(L"复核失败：实际算法 {} ≠ 指定算法 {}",
                       algorithm_name(cs_actual.e_algorithm),
                       algorithm_name(e_target));
  }
  return L"";
}

}
