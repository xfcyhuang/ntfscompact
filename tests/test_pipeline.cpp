// 管线输出格式化单元测试（工单 #3）
// 缝（seam）：format_report_line / format_summary 纯函数——FileReport 进、干跑行出。
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
    o.e_algorithm = a;
    o.b_uncompress = uncompress;
    return o;
}

}  // namespace

using namespace ntfscompact;

TEST_CASE("干跑行：压缩模式各动作措辞（目标 xpress8k）") {
    const Options opt = Opt(Algorithm::kXpress8k);

    SUBCASE("未压缩 → 压缩") {
        FileReport r;
        r.str_display_path = L"C:\\d\\a.txt";
        r.cs_state.e_kind = CurrentState::Kind::kNone;
        r.pl_plan = decide_action(r.cs_state, opt);
        CHECK(narrow(format_report_line(r, opt)) ==
              "[压缩] C:\\d\\a.txt：现状 未压缩，将压缩为指定算法 xpress8k");
    }
    SUBCASE("LZNT1 → 重压") {
        FileReport r;
        r.str_display_path = L"C:\\d\\b.txt";
        r.cs_state.e_kind = CurrentState::Kind::kLznt1;
        r.pl_plan = decide_action(r.cs_state, opt);
        CHECK(narrow(format_report_line(r, opt)) ==
              "[重压] C:\\d\\b.txt：现状 LZNT1，将重压为指定算法 xpress8k");
    }
    SUBCASE("WOF lzx → 重压") {
        FileReport r;
        r.str_display_path = L"C:\\d\\c.txt";
        r.cs_state.e_kind = CurrentState::Kind::kWof;
        r.cs_state.e_algorithm = Algorithm::kLzx;
        r.pl_plan = decide_action(r.cs_state, opt);
        CHECK(narrow(format_report_line(r, opt)) ==
              "[重压] C:\\d\\c.txt：现状 lzx（WOF），将重压为指定算法 xpress8k");
    }
    SUBCASE("WOF xpress8k 已是指定算法 → 跳过并标注原因") {
        FileReport r;
        r.str_display_path = L"C:\\d\\d.txt";
        r.cs_state.e_kind = CurrentState::Kind::kWof;
        r.cs_state.e_algorithm = Algorithm::kXpress8k;
        r.pl_plan = decide_action(r.cs_state, opt);
        CHECK(narrow(format_report_line(r, opt)) ==
              "[跳过] C:\\d\\d.txt：已是指定算法 xpress8k");
    }
    SUBCASE("遍历期跳过（无现状）→ 跳过并标注原因") {
        FileReport r;
        r.str_display_path = L"C:\\d\\small.bin";
        r.b_has_state = false;
        r.pl_plan.e_action = Plan::Action::kSkip;
        r.pl_plan.str_reason = L"文件大小 ≤4KB";
        CHECK(narrow(format_report_line(r, opt)) ==
              "[跳过] C:\\d\\small.bin：文件大小 ≤4KB");
    }
}

TEST_CASE("干跑行：解压模式措辞") {
    const Options opt = Opt(Algorithm::kXpress8k, /*uncompress=*/true);

    SUBCASE("WOF lzx → 解压") {
        FileReport r;
        r.str_display_path = L"C:\\d\\f.txt";
        r.cs_state.e_kind = CurrentState::Kind::kWof;
        r.cs_state.e_algorithm = Algorithm::kLzx;
        r.pl_plan = decide_action(r.cs_state, opt);
        CHECK(narrow(format_report_line(r, opt)) ==
              "[解压] C:\\d\\f.txt：现状 lzx（WOF），将解压");
    }
    SUBCASE("未压缩 → 跳过") {
        FileReport r;
        r.str_display_path = L"C:\\d\\g.txt";
        r.cs_state.e_kind = CurrentState::Kind::kNone;
        r.pl_plan = decide_action(r.cs_state, opt);
        CHECK(narrow(format_report_line(r, opt)) == "[跳过] C:\\d\\g.txt：未压缩");
    }
}

TEST_CASE("汇总行：各动作计数") {
    PipelineResult result;
    auto add = [&](Plan::Action a) {
        FileReport r;
        r.pl_plan.e_action = a;
        result.vec_reports.push_back(r);
    };
    add(Plan::Action::kCompress);
    add(Plan::Action::kCompress);
    add(Plan::Action::kRecompress);
    add(Plan::Action::kSkip);
    add(Plan::Action::kSkip);
    add(Plan::Action::kSkip);
    add(Plan::Action::kUncompress);
    // 两个失败项也在报告中（不计入动作计数，单独计失败）
    FileReport e1;
    e1.str_display_path = L"C:\\d\\x.bin";
    e1.str_error = L"现状检测失败";
    result.vec_reports.push_back(e1);
    FileReport e2;
    e2.str_display_path = L"C:\\d\\y.bin";
    e2.str_error = L"现状检测失败";
    result.vec_reports.push_back(e2);
    result.i4_failures = 2;
    CHECK(narrow(format_summary(result)) ==
          "共 9 个文件：压缩 2、重压 1、解压 1、跳过 3；失败 2");
}

TEST_CASE("报告行：检测失败项输出失败行") {
    FileReport r;
    r.str_display_path = L"C:\\d\\x.bin";
    r.str_error = L"现状检测失败（模拟）";
    CHECK(narrow(format_report_line(r, Opt(Algorithm::kXpress8k))) ==
          "[失败] C:\\d\\x.bin：现状检测失败（模拟）");
}
