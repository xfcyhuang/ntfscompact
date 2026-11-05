#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pipeline.hpp"

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

ntfscompact::Options make_options(ntfscompact::Algorithm e_algorithm,
                                  bool b_uncompress = false) {
  ntfscompact::Options opt_result{};
  opt_result.e_algorithm = e_algorithm;
  opt_result.b_uncompress = b_uncompress;
  return opt_result;
}

}

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::FileReport;
using ntfscompact::PipelineResult;
using ntfscompact::Plan;
using ntfscompact::decide_action;
using ntfscompact::format_report_line;
using ntfscompact::format_summary;

TEST_CASE("干跑行：压缩模式各动作措辞（目标 xpress8k）") {
  const auto opt_case{make_options(Algorithm::kXpress8k)};

  SUBCASE("未压缩 → 压缩") {
    FileReport fr_report{};
    fr_report.str_display_path = L"C:\\d\\a.txt";
    fr_report.cs_state.e_kind = CurrentState::Kind::kNone;
    fr_report.pl_plan = decide_action(fr_report.cs_state, opt_case);
    CHECK(narrow(format_report_line(fr_report, opt_case)) ==
          "[压缩] C:\\d\\a.txt：现状 未压缩，将压缩为指定算法 xpress8k");
  }
  SUBCASE("LZNT1 → 重压") {
    FileReport fr_report{};
    fr_report.str_display_path = L"C:\\d\\b.txt";
    fr_report.cs_state.e_kind = CurrentState::Kind::kLznt1;
    fr_report.pl_plan = decide_action(fr_report.cs_state, opt_case);
    CHECK(narrow(format_report_line(fr_report, opt_case)) ==
          "[重压] C:\\d\\b.txt：现状 LZNT1，将重压为指定算法 xpress8k");
  }
  SUBCASE("WOF lzx → 重压") {
    FileReport fr_report{};
    fr_report.str_display_path = L"C:\\d\\c.txt";
    fr_report.cs_state.e_kind = CurrentState::Kind::kWof;
    fr_report.cs_state.e_algorithm = Algorithm::kLzx;
    fr_report.pl_plan = decide_action(fr_report.cs_state, opt_case);
    CHECK(narrow(format_report_line(fr_report, opt_case)) ==
          "[重压] C:\\d\\c.txt：现状 lzx（WOF），将重压为指定算法 xpress8k");
  }
  SUBCASE("WOF xpress8k 已是指定算法 → 跳过并标注原因") {
    FileReport fr_report{};
    fr_report.str_display_path = L"C:\\d\\d.txt";
    fr_report.cs_state.e_kind = CurrentState::Kind::kWof;
    fr_report.cs_state.e_algorithm = Algorithm::kXpress8k;
    fr_report.pl_plan = decide_action(fr_report.cs_state, opt_case);
    CHECK(narrow(format_report_line(fr_report, opt_case)) ==
          "[跳过] C:\\d\\d.txt：已是指定算法 xpress8k");
  }
  SUBCASE("遍历期跳过（无现状）→ 跳过并标注原因") {
    FileReport fr_report{};
    fr_report.str_display_path = L"C:\\d\\small.bin";
    fr_report.b_has_state = false;
    fr_report.pl_plan.e_action = Plan::Action::kSkip;
    fr_report.pl_plan.str_reason = L"文件大小 ≤4KB";
    CHECK(narrow(format_report_line(fr_report, opt_case)) ==
          "[跳过] C:\\d\\small.bin：文件大小 ≤4KB");
  }
}

TEST_CASE("干跑行：解压模式措辞") {
  const auto opt_case{make_options(Algorithm::kXpress8k, true)};

  SUBCASE("WOF lzx → 解压") {
    FileReport fr_report{};
    fr_report.str_display_path = L"C:\\d\\f.txt";
    fr_report.cs_state.e_kind = CurrentState::Kind::kWof;
    fr_report.cs_state.e_algorithm = Algorithm::kLzx;
    fr_report.pl_plan = decide_action(fr_report.cs_state, opt_case);
    CHECK(narrow(format_report_line(fr_report, opt_case)) ==
          "[解压] C:\\d\\f.txt：现状 lzx（WOF），将解压");
  }
  SUBCASE("未压缩 → 跳过") {
    FileReport fr_report{};
    fr_report.str_display_path = L"C:\\d\\g.txt";
    fr_report.cs_state.e_kind = CurrentState::Kind::kNone;
    fr_report.pl_plan = decide_action(fr_report.cs_state, opt_case);
    CHECK(narrow(format_report_line(fr_report, opt_case)) ==
          "[跳过] C:\\d\\g.txt：未压缩");
  }
}

TEST_CASE("汇总行：各动作计数") {
  PipelineResult pl_result{};
  const auto fn_add{[&pl_result](Plan::Action e_action) {
    FileReport fr_report{};
    fr_report.pl_plan.e_action = e_action;
    pl_result.vec_reports.push_back(fr_report);
  }};
  fn_add(Plan::Action::kCompress);
  fn_add(Plan::Action::kCompress);
  fn_add(Plan::Action::kRecompress);
  fn_add(Plan::Action::kSkip);
  fn_add(Plan::Action::kSkip);
  fn_add(Plan::Action::kSkip);
  fn_add(Plan::Action::kUncompress);
  FileReport fr_first{};
  fr_first.str_display_path = L"C:\\d\\x.bin";
  fr_first.str_error = L"现状检测失败";
  pl_result.vec_reports.push_back(fr_first);
  FileReport fr_second{};
  fr_second.str_display_path = L"C:\\d\\y.bin";
  fr_second.str_error = L"现状检测失败";
  pl_result.vec_reports.push_back(fr_second);
  pl_result.i4_failures = 2;
  CHECK(narrow(format_summary(pl_result)) ==
        "共 9 个文件：压缩 2、重压 1、解压 1、跳过 3；失败 2");
}

TEST_CASE("报告行：检测失败项输出失败行") {
  FileReport fr_report{};
  fr_report.str_display_path = L"C:\\d\\x.bin";
  fr_report.str_error = L"现状检测失败（模拟）";
  CHECK(narrow(format_report_line(fr_report, make_options(Algorithm::kXpress8k))) ==
        "[失败] C:\\d\\x.bin：现状检测失败（模拟）");
}
