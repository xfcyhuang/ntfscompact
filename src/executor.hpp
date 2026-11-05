#pragma once

import std;

#include "cli.hpp"
#include "detect.hpp"

namespace ntfscompact {

struct ExecuteResult {
  enum class Outcome { kSuccess, kSkipped, kError };
  Outcome e_outcome{Outcome::kSuccess};
  std::wstring str_message;
};

using ExecuteFn = std::function<ExecuteResult(const std::wstring& str_long_path,
                                              Plan::Action e_action,
                                              const CurrentState& cs_current,
                                              Algorithm e_target)>;

std::expected<ExecuteFn, std::wstring> make_real_executor();

std::wstring check_compression_meets_target(bool b_is_external, int i4_provider,
                                            Algorithm e_actual, Algorithm e_target);

inline std::wstring check_compression_meets_target(bool b_is_external,
                                                   int i4_provider,
                                                   Algorithm e_actual,
                                                   Algorithm e_target) {
  if (!b_is_external) {
    return L"复核失败：压缩后文件未处于 WOF backing（可能被系统静默跳过）";
  }
  if (i4_provider != CurrentState::kProviderFile) {
    return std::format(L"复核失败：backing provider 为 {}（期望 WOF FILE）",
                       i4_provider);
  }
  if (e_actual != e_target) {
    return std::format(L"复核失败：实际算法 {} ≠ 指定算法 {}",
                       algorithm_name(e_actual), algorithm_name(e_target));
  }
  return L"";
}

}
