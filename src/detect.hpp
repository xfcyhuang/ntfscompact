// 现状检测与动作决策（工单 #3）
// DecideAction/DescribeState 为纯函数（决策表）；IDetector 是检测缝——
// 生产用 RealDetector（wofutil.dll 动态加载 + FSCTL），单测注入假实现，
// 不依赖真实压缩状态。
#pragma once

import std;

#include "cli.hpp"

namespace ntfscompact {

// 文件现状：当前压缩形态
struct CurrentState {
    enum class Kind { None, Lznt1, Wof };
    Kind kind = Kind::None;
    Algorithm algorithm = Algorithm::Xpress8k;  // kind==Wof 时有效：现有 WOF 算法

    // WOF backing provider（kind==Wof 时有效），取值对齐 wofapi.h
    static constexpr int kProviderWim = 1;    // WOF_PROVIDER_WIM
    static constexpr int kProviderFile = 2;   // WOF_PROVIDER_FILE（本项目唯一压缩路径）
    static constexpr int kProviderCloud = 3;  // WOF_PROVIDER_CLOUD
    int provider = kProviderFile;
};

// 检测结果：error 非空表示无法检测（文件不存在/权限不足等），此时 state 无效
struct DetectionResult {
    CurrentState state;
    std::wstring error;
};

// 检测缝：给出文件的现状压缩形态。
// 生产实现 make_real_detector（wofutil.dll 动态加载 + FSCTL_GET_COMPRESSION）；
// 单测注入假函数，不依赖真实压缩状态。
using DetectFn = std::function<DetectionResult(const std::wstring& str_long_path)>;

// 生产检测器工厂。动态加载 wofutil.dll；失败时返回错误说明——
// 调用方应打印说明并终止（无法判 WOF 现状会误压）。
[[nodiscard]] auto make_real_detector() -> std::expected<DetectFn, std::wstring>;

// 将采取的动作（GLOSSARY：压缩 / 重压 / 解压 / 跳过）
struct Plan {
    enum class Action { Skip, Compress, Recompress, Uncompress };
    Action action = Action::Skip;
    std::wstring reason;  // Skip 时的跳过原因，其余动作为空
};

// 算法命令行显示名（与 -a 取值一致）
inline std::wstring AlgorithmName(Algorithm a) {
    switch (a) {
        case Algorithm::Xpress4k: return L"xpress4k";
        case Algorithm::Xpress8k: return L"xpress8k";
        case Algorithm::Xpress16k: return L"xpress16k";
        case Algorithm::Lzx: return L"lzx";
    }
    return L"xpress8k";  // 不可达：算法四值全覆盖
}

// 纯决策表：现状 + 用户目标（指定算法 / 解压模式）→ 动作。
// WIM/Cloud backing 属黑名单，两种模式下都跳过。
inline Plan DecideAction(const CurrentState& current, const Options& options) {
    Plan plan;
    if (current.kind == CurrentState::Kind::Wof &&
        current.provider != CurrentState::kProviderFile) {
        plan.action = Plan::Action::Skip;
        plan.reason = current.provider == CurrentState::kProviderWim
                          ? L"WIM backing 文件（黑名单）"
                          : L"Cloud backing 文件（黑名单）";
        return plan;
    }
    if (options.uncompress) {
        switch (current.kind) {
            case CurrentState::Kind::None:
                plan.action = Plan::Action::Skip;
                plan.reason = L"未压缩";
                return plan;
            case CurrentState::Kind::Lznt1:
            case CurrentState::Kind::Wof:
                plan.action = Plan::Action::Uncompress;
                return plan;
        }
    }
    switch (current.kind) {
        case CurrentState::Kind::None:
            plan.action = Plan::Action::Compress;
            return plan;
        case CurrentState::Kind::Lznt1:
            // LZNT1 老文件不算「已压缩」：先解除再按指定算法重压（ADR 0001）
            plan.action = Plan::Action::Recompress;
            return plan;
        case CurrentState::Kind::Wof:
            if (current.algorithm == options.algorithm) {
                plan.action = Plan::Action::Skip;
                plan.reason = L"已是指定算法 " + AlgorithmName(options.algorithm);
            } else {
                plan.action = Plan::Action::Recompress;
            }
            return plan;
    }
    return plan;  // 不可达：Kind 三值全覆盖
}

// 现状描述（干跑输出用）
inline std::wstring DescribeState(const CurrentState& s) {
    switch (s.kind) {
        case CurrentState::Kind::None: return L"未压缩";
        case CurrentState::Kind::Lznt1: return L"LZNT1";
        case CurrentState::Kind::Wof: return AlgorithmName(s.algorithm) + L"（WOF）";
    }
    return L"未压缩";  // 不可达
}

}  // namespace ntfscompact
