// 管线输出格式化单元测试（工单 #3）
// 缝（seam）：FormatReportLine / FormatSummary 纯函数——FileReport 进、干跑行出。
// 措辞对齐 GLOSSARY（指定算法/跳过/重压/解压/压缩）。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pipeline.hpp"

#include <windows.h>

namespace {

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

ntfscompact::Options Opt(ntfscompact::Algorithm a, bool uncompress = false) {
    ntfscompact::Options o;
    o.algorithm = a;
    o.uncompress = uncompress;
    return o;
}

}  // namespace

using namespace ntfscompact;

TEST_CASE("干跑行：压缩模式各动作措辞（目标 xpress8k）") {
    const Options opt = Opt(Algorithm::Xpress8k);

    SUBCASE("未压缩 → 压缩") {
        FileReport r;
        r.display_path = L"C:\\d\\a.txt";
        r.state.kind = CurrentState::Kind::None;
        r.plan = DecideAction(r.state, opt);
        CHECK(narrow(FormatReportLine(r, opt)) ==
              "[压缩] C:\\d\\a.txt：现状 未压缩，将压缩为指定算法 xpress8k");
    }
    SUBCASE("LZNT1 → 重压") {
        FileReport r;
        r.display_path = L"C:\\d\\b.txt";
        r.state.kind = CurrentState::Kind::Lznt1;
        r.plan = DecideAction(r.state, opt);
        CHECK(narrow(FormatReportLine(r, opt)) ==
              "[重压] C:\\d\\b.txt：现状 LZNT1，将重压为指定算法 xpress8k");
    }
    SUBCASE("WOF lzx → 重压") {
        FileReport r;
        r.display_path = L"C:\\d\\c.txt";
        r.state.kind = CurrentState::Kind::Wof;
        r.state.algorithm = Algorithm::Lzx;
        r.plan = DecideAction(r.state, opt);
        CHECK(narrow(FormatReportLine(r, opt)) ==
              "[重压] C:\\d\\c.txt：现状 lzx（WOF），将重压为指定算法 xpress8k");
    }
    SUBCASE("WOF xpress8k 已是指定算法 → 跳过并标注原因") {
        FileReport r;
        r.display_path = L"C:\\d\\d.txt";
        r.state.kind = CurrentState::Kind::Wof;
        r.state.algorithm = Algorithm::Xpress8k;
        r.plan = DecideAction(r.state, opt);
        CHECK(narrow(FormatReportLine(r, opt)) ==
              "[跳过] C:\\d\\d.txt：已是指定算法 xpress8k");
    }
    SUBCASE("遍历期跳过（无现状）→ 跳过并标注原因") {
        FileReport r;
        r.display_path = L"C:\\d\\small.bin";
        r.has_state = false;
        r.plan.action = Plan::Action::Skip;
        r.plan.reason = L"文件大小 ≤4KB";
        CHECK(narrow(FormatReportLine(r, opt)) ==
              "[跳过] C:\\d\\small.bin：文件大小 ≤4KB");
    }
}

TEST_CASE("干跑行：解压模式措辞") {
    const Options opt = Opt(Algorithm::Xpress8k, /*uncompress=*/true);

    SUBCASE("WOF lzx → 解压") {
        FileReport r;
        r.display_path = L"C:\\d\\f.txt";
        r.state.kind = CurrentState::Kind::Wof;
        r.state.algorithm = Algorithm::Lzx;
        r.plan = DecideAction(r.state, opt);
        CHECK(narrow(FormatReportLine(r, opt)) ==
              "[解压] C:\\d\\f.txt：现状 lzx（WOF），将解压");
    }
    SUBCASE("未压缩 → 跳过") {
        FileReport r;
        r.display_path = L"C:\\d\\g.txt";
        r.state.kind = CurrentState::Kind::None;
        r.plan = DecideAction(r.state, opt);
        CHECK(narrow(FormatReportLine(r, opt)) == "[跳过] C:\\d\\g.txt：未压缩");
    }
}

TEST_CASE("汇总行：各动作计数") {
    PipelineResult result;
    auto add = [&](Plan::Action a) {
        FileReport r;
        r.plan.action = a;
        result.reports.push_back(r);
    };
    add(Plan::Action::Compress);
    add(Plan::Action::Compress);
    add(Plan::Action::Recompress);
    add(Plan::Action::Skip);
    add(Plan::Action::Skip);
    add(Plan::Action::Skip);
    add(Plan::Action::Uncompress);
    // 两个失败项也在报告中（不计入动作计数，单独计失败）
    FileReport e1;
    e1.display_path = L"C:\\d\\x.bin";
    e1.error = L"现状检测失败";
    result.reports.push_back(e1);
    FileReport e2;
    e2.display_path = L"C:\\d\\y.bin";
    e2.error = L"现状检测失败";
    result.reports.push_back(e2);
    result.failures = 2;
    CHECK(narrow(FormatSummary(result)) ==
          "共 9 个文件：压缩 2、重压 1、解压 1、跳过 3；失败 2");
}

TEST_CASE("报告行：检测失败项输出失败行") {
    FileReport r;
    r.display_path = L"C:\\d\\x.bin";
    r.error = L"现状检测失败（模拟）";
    CHECK(narrow(FormatReportLine(r, Opt(Algorithm::Xpress8k))) ==
          "[失败] C:\\d\\x.bin：现状检测失败（模拟）");
}
