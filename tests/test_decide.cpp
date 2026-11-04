// 决策表单元测试（工单 #3）
// 缝（seam）：DecideAction 纯函数——文件现状 + 用户目标（指定算法/解压）进、
// 将采取的动作（压缩/重压/解压/跳过）出。无 I/O，不依赖真实压缩状态。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "detect.h"

#include <string>
#include <windows.h>

namespace {

// 宽字符串转 UTF-8 窄字符串（doctest::String 按字节比较，中文文案可精确断言）
doctest::String narrow(const std::wstring& s) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                          static_cast<int>(s.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            utf8.data(), bytes, nullptr, nullptr);
    }
    return doctest::String(utf8.c_str());
}

ntfscompact::Options CompressOptions(ntfscompact::Algorithm a) {
    ntfscompact::Options o;
    o.algorithm = a;
    return o;
}

ntfscompact::Options UncompressOptions() {
    ntfscompact::Options o;
    o.uncompress = true;
    return o;
}

ntfscompact::CurrentState NoneState() { return {}; }

ntfscompact::CurrentState Lznt1State() {
    ntfscompact::CurrentState s;
    s.kind = ntfscompact::CurrentState::Kind::Lznt1;
    return s;
}

ntfscompact::CurrentState WofState(ntfscompact::Algorithm a, int provider) {
    ntfscompact::CurrentState s;
    s.kind = ntfscompact::CurrentState::Kind::Wof;
    s.algorithm = a;
    s.provider = provider;
    return s;
}

bool Contains(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

}  // namespace

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::DecideAction;
using ntfscompact::Options;
using ntfscompact::Plan;
using P = ntfscompact::CurrentState::Kind;

TEST_CASE("压缩模式：现状与指定算法共同决定动作（目标 xpress8k）") {
    const Options opt = CompressOptions(Algorithm::Xpress8k);

    SUBCASE("未压缩 → 压缩") {
        const Plan p = DecideAction(NoneState(), opt);
        CHECK(p.action == Plan::Action::Compress);
    }
    SUBCASE("LZNT1 → 重压（先解 LZNT1 再按指定算法压缩，见 ADR 0001）") {
        const Plan p = DecideAction(Lznt1State(), opt);
        CHECK(p.action == Plan::Action::Recompress);
    }
    SUBCASE("WOF 且已是指定算法 xpress8k → 跳过，原因含「指定算法」") {
        const Plan p = DecideAction(WofState(Algorithm::Xpress8k, CurrentState::kProviderFile), opt);
        CHECK(p.action == Plan::Action::Skip);
        CHECK(Contains(p.reason, L"指定算法"));
    }
    SUBCASE("WOF lzx → 重压") {
        const Plan p = DecideAction(WofState(Algorithm::Lzx, CurrentState::kProviderFile), opt);
        CHECK(p.action == Plan::Action::Recompress);
    }
    SUBCASE("WOF xpress4k → 重压") {
        const Plan p = DecideAction(WofState(Algorithm::Xpress4k, CurrentState::kProviderFile), opt);
        CHECK(p.action == Plan::Action::Recompress);
    }
    SUBCASE("WOF xpress16k → 重压") {
        const Plan p = DecideAction(WofState(Algorithm::Xpress16k, CurrentState::kProviderFile), opt);
        CHECK(p.action == Plan::Action::Recompress);
    }
    SUBCASE("WIM backing → 跳过，原因标注 WIM") {
        const Plan p = DecideAction(WofState(Algorithm::Lzx, CurrentState::kProviderWim), opt);
        CHECK(p.action == Plan::Action::Skip);
        CHECK(Contains(p.reason, L"WIM"));
    }
    SUBCASE("Cloud backing → 跳过，原因标注 Cloud") {
        const Plan p = DecideAction(WofState(Algorithm::Lzx, CurrentState::kProviderCloud), opt);
        CHECK(p.action == Plan::Action::Skip);
        CHECK(Contains(p.reason, L"Cloud"));
    }
}

TEST_CASE("压缩模式：目标算法变化时判定随之变化（目标 lzx）") {
    const Options opt = CompressOptions(Algorithm::Lzx);
    SUBCASE("WOF lzx → 跳过") {
        const Plan p = DecideAction(WofState(Algorithm::Lzx, CurrentState::kProviderFile), opt);
        CHECK(p.action == Plan::Action::Skip);
    }
    SUBCASE("WOF xpress8k → 重压") {
        const Plan p = DecideAction(WofState(Algorithm::Xpress8k, CurrentState::kProviderFile), opt);
        CHECK(p.action == Plan::Action::Recompress);
    }
}

TEST_CASE("解压模式（-u）：有压缩则解压，未压缩则跳过") {
    const Options opt = UncompressOptions();
    SUBCASE("未压缩 → 跳过") {
        const Plan p = DecideAction(NoneState(), opt);
        CHECK(p.action == Plan::Action::Skip);
    }
    SUBCASE("LZNT1 → 解压") {
        const Plan p = DecideAction(Lznt1State(), opt);
        CHECK(p.action == Plan::Action::Uncompress);
    }
    SUBCASE("WOF xpress8k → 解压") {
        const Plan p = DecideAction(WofState(Algorithm::Xpress8k, CurrentState::kProviderFile), opt);
        CHECK(p.action == Plan::Action::Uncompress);
    }
    SUBCASE("WIM backing → 跳过（黑名单同样生效）") {
        const Plan p = DecideAction(WofState(Algorithm::Lzx, CurrentState::kProviderWim), opt);
        CHECK(p.action == Plan::Action::Skip);
    }
    SUBCASE("Cloud backing → 跳过（黑名单同样生效）") {
        const Plan p = DecideAction(WofState(Algorithm::Lzx, CurrentState::kProviderCloud), opt);
        CHECK(p.action == Plan::Action::Skip);
    }
}

TEST_CASE("算法与现状的显示名（GLOSSARY 术语）") {
    CHECK(narrow(ntfscompact::AlgorithmName(Algorithm::Xpress4k)) == "xpress4k");
    CHECK(narrow(ntfscompact::AlgorithmName(Algorithm::Xpress8k)) == "xpress8k");
    CHECK(narrow(ntfscompact::AlgorithmName(Algorithm::Xpress16k)) == "xpress16k");
    CHECK(narrow(ntfscompact::AlgorithmName(Algorithm::Lzx)) == "lzx");

    SUBCASE("现状描述：未压缩 / LZNT1 / WOF 算法") {
        CHECK(narrow(ntfscompact::DescribeState(NoneState())) == "未压缩");
        CHECK(narrow(ntfscompact::DescribeState(Lznt1State())) == "LZNT1");
        CHECK(narrow(ntfscompact::DescribeState(WofState(Algorithm::Lzx, CurrentState::kProviderFile))) ==
              "lzx（WOF）");
    }
}
