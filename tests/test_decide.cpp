#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "detect.hpp"

#include <windows.h>

namespace {

doctest::String narrow(const std::wstring& str_wide) {
  const int i4_bytes{WideCharToMultiByte(CP_UTF8, 0, str_wide.data(),
                                         static_cast<int>(str_wide.size()),
                                         nullptr, 0, nullptr, nullptr)};
  std::string str_utf8(static_cast<std::size_t>(i4_bytes), '\0');
  if (i4_bytes > 0) {
    WideCharToMultiByte(CP_UTF8, 0, str_wide.data(),
                        static_cast<int>(str_wide.size()), str_utf8.data(),
                        i4_bytes, nullptr, nullptr);
  }
  return doctest::String(str_utf8.c_str());
}

ntfscompact::Options compress_options(ntfscompact::Algorithm e_algorithm) {
  ntfscompact::Options opt_result{};
  opt_result.e_algorithm = e_algorithm;
  return opt_result;
}

ntfscompact::Options uncompress_options() {
  ntfscompact::Options opt_result{};
  opt_result.b_uncompress = true;
  return opt_result;
}

ntfscompact::CurrentState none_state() { return {}; }

ntfscompact::CurrentState lznt1_state() {
  ntfscompact::CurrentState cs_result{};
  cs_result.e_kind = ntfscompact::CurrentState::Kind::kLznt1;
  return cs_result;
}

ntfscompact::CurrentState wof_state(ntfscompact::Algorithm e_algorithm,
                                    int i4_provider) {
  ntfscompact::CurrentState cs_result{};
  cs_result.e_kind = ntfscompact::CurrentState::Kind::kWof;
  cs_result.e_algorithm = e_algorithm;
  cs_result.i4_provider = i4_provider;
  return cs_result;
}

bool contains(const std::wstring& str_haystack, const std::wstring& str_needle) {
  return str_haystack.find(str_needle) != std::wstring::npos;
}

}

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::Options;
using ntfscompact::Plan;
using ntfscompact::decide_action;

TEST_CASE("压缩模式：现状与指定算法共同决定动作（目标 xpress8k）") {
  const Options opt_case{compress_options(Algorithm::kXpress8k)};

  SUBCASE("未压缩 → 压缩") {
    const Plan pl_plan{decide_action(none_state(), opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kCompress);
  }
  SUBCASE("LZNT1 → 重压（先解 LZNT1 再按指定算法压缩，见 ADR 0001）") {
    const Plan pl_plan{decide_action(lznt1_state(), opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kRecompress);
  }
  SUBCASE("WOF 且已是指定算法 xpress8k → 跳过，原因含「指定算法」") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kXpress8k, CurrentState::kProviderFile),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kSkip);
    CHECK(contains(pl_plan.str_reason, L"指定算法"));
  }
  SUBCASE("WOF lzx → 重压") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kLzx, CurrentState::kProviderFile),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kRecompress);
  }
  SUBCASE("WOF xpress4k → 重压") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kXpress4k, CurrentState::kProviderFile),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kRecompress);
  }
  SUBCASE("WOF xpress16k → 重压") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kXpress16k, CurrentState::kProviderFile),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kRecompress);
  }
  SUBCASE("WIM backing → 跳过，原因标注 WIM") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kLzx, CurrentState::kProviderWim),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kSkip);
    CHECK(contains(pl_plan.str_reason, L"WIM"));
  }
  SUBCASE("Cloud backing → 跳过，原因标注 Cloud") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kLzx, CurrentState::kProviderCloud),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kSkip);
    CHECK(contains(pl_plan.str_reason, L"Cloud"));
  }
}

TEST_CASE("压缩模式：目标算法变化时判定随之变化（目标 lzx）") {
  const Options opt_case{compress_options(Algorithm::kLzx)};
  SUBCASE("WOF lzx → 跳过") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kLzx, CurrentState::kProviderFile),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kSkip);
  }
  SUBCASE("WOF xpress8k → 重压") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kXpress8k, CurrentState::kProviderFile),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kRecompress);
  }
}

TEST_CASE("解压模式（-u）：有压缩则解压，未压缩则跳过") {
  const Options opt_case{uncompress_options()};
  SUBCASE("未压缩 → 跳过") {
    const Plan pl_plan{decide_action(none_state(), opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kSkip);
  }
  SUBCASE("LZNT1 → 解压") {
    const Plan pl_plan{decide_action(lznt1_state(), opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kUncompress);
  }
  SUBCASE("WOF xpress8k → 解压") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kXpress8k, CurrentState::kProviderFile),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kUncompress);
  }
  SUBCASE("WIM backing → 跳过（黑名单同样生效）") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kLzx, CurrentState::kProviderWim),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kSkip);
  }
  SUBCASE("Cloud backing → 跳过（黑名单同样生效）") {
    const Plan pl_plan{
        decide_action(wof_state(Algorithm::kLzx, CurrentState::kProviderCloud),
                      opt_case)};
    CHECK(pl_plan.e_action == Plan::Action::kSkip);
  }
}

TEST_CASE("算法与现状的显示名（GLOSSARY 术语）") {
  CHECK(narrow(ntfscompact::algorithm_name(Algorithm::kXpress4k)) == "xpress4k");
  CHECK(narrow(ntfscompact::algorithm_name(Algorithm::kXpress8k)) == "xpress8k");
  CHECK(narrow(ntfscompact::algorithm_name(Algorithm::kXpress16k)) == "xpress16k");
  CHECK(narrow(ntfscompact::algorithm_name(Algorithm::kLzx)) == "lzx");

  SUBCASE("现状描述：未压缩 / LZNT1 / WOF 算法") {
    CHECK(narrow(ntfscompact::describe_state(none_state())) == "未压缩");
    CHECK(narrow(ntfscompact::describe_state(lznt1_state())) == "LZNT1");
    CHECK(narrow(ntfscompact::describe_state(
              wof_state(Algorithm::kLzx, CurrentState::kProviderFile))) ==
          "lzx（WOF）");
  }
}
