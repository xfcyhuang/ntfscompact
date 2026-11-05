#pragma once

import std;

#include "cli.hpp"

namespace ntfscompact {

struct CurrentState {
  enum class Kind { kNone, kLznt1, kWof };
  Kind e_kind{Kind::kNone};
  Algorithm e_algorithm{Algorithm::kXpress8k};

  static constexpr int kProviderWim{1};
  static constexpr int kProviderFile{2};
  static constexpr int kProviderCloud{3};
  int i4_provider{kProviderFile};
};

struct DetectionResult {
  CurrentState cs_state{};
  std::wstring str_error{};
};

using DetectFn = std::function<DetectionResult(const std::wstring& str_long_path)>;

[[nodiscard]] auto make_real_detector() -> std::expected<DetectFn, std::wstring>;

struct Plan {
  enum class Action { kSkip, kCompress, kRecompress, kUncompress };
  Action e_action{Action::kSkip};
  std::wstring str_reason{};
};

inline std::wstring algorithm_name(Algorithm e_algorithm) {
  switch (e_algorithm) {
    case Algorithm::kXpress4k: return L"xpress4k";
    case Algorithm::kXpress8k: return L"xpress8k";
    case Algorithm::kXpress16k: return L"xpress16k";
    case Algorithm::kLzx: return L"lzx";
  }
  return L"xpress8k";
}

inline Plan decide_action(const CurrentState& cs_current, const Options& opt_options) {
  Plan pl_plan{};
  if (cs_current.e_kind == CurrentState::Kind::kWof &&
      cs_current.i4_provider != CurrentState::kProviderFile) {
    pl_plan.e_action = Plan::Action::kSkip;
    pl_plan.str_reason = cs_current.i4_provider == CurrentState::kProviderWim
                            ? L"WIM backing 文件（黑名单）"
                            : L"Cloud backing 文件（黑名单）";
    return pl_plan;
  }
  if (opt_options.b_uncompress) {
    switch (cs_current.e_kind) {
      case CurrentState::Kind::kNone:
        pl_plan.e_action = Plan::Action::kSkip;
        pl_plan.str_reason = L"未压缩";
        return pl_plan;
      case CurrentState::Kind::kLznt1:
      case CurrentState::Kind::kWof:
        pl_plan.e_action = Plan::Action::kUncompress;
        return pl_plan;
    }
  }
  switch (cs_current.e_kind) {
    case CurrentState::Kind::kNone:
      pl_plan.e_action = Plan::Action::kCompress;
      return pl_plan;
    case CurrentState::Kind::kLznt1:
      pl_plan.e_action = Plan::Action::kRecompress;
      return pl_plan;
    case CurrentState::Kind::kWof:
      if (cs_current.e_algorithm == opt_options.e_algorithm) {
        pl_plan.e_action = Plan::Action::kSkip;
        pl_plan.str_reason =
            L"已是指定算法 " + algorithm_name(opt_options.e_algorithm);
      } else {
        pl_plan.e_action = Plan::Action::kRecompress;
      }
      return pl_plan;
  }
  return pl_plan;
}

inline std::wstring describe_state(const CurrentState& cs_state) {
  switch (cs_state.e_kind) {
    case CurrentState::Kind::kNone: return L"未压缩";
    case CurrentState::Kind::kLznt1: return L"LZNT1";
    case CurrentState::Kind::kWof:
      return algorithm_name(cs_state.e_algorithm) + L"（WOF）";
  }
  return L"未压缩";
}

}
