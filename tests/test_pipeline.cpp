#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pipeline.hpp"
#include "temp_dir.hpp"
#include "util.hpp"

#include <windows.h>

namespace {

using ntfscompact_test::narrow;

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
using ntfscompact::DirFlagReport;
using ntfscompact::FileReport;
using ntfscompact::PipelineResult;
using ntfscompact::Plan;
using ntfscompact::collect_byte_stats;
using ntfscompact::decide_action;
using ntfscompact::format_dir_flag_line;
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

TEST_CASE("汇总行：各动作计数与压缩/重压字节段") {
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
  SUBCASE("未采集字节（0 → 0，节省 0%）") {
    CHECK(narrow(format_summary(pl_result, make_options(Algorithm::kXpress8k))) ==
          "共 9 个文件：压缩 2、重压 1、解压 1、跳过 3；压缩/重压 0 → 0 字节，"
          "节省 0%；失败 2");
  }
  SUBCASE("已采集字节：整数截断百分比") {
    pl_result.u8_compressed_original = 3000;
    pl_result.u8_compressed_disk = 1000;
    CHECK(narrow(format_summary(pl_result, make_options(Algorithm::kXpress8k))) ==
          "共 9 个文件：压缩 2、重压 1、解压 1、跳过 3；压缩/重压 3000 → 1000 "
          "字节，节省 66%；失败 2");
  }
  SUBCASE("干跑不输出字节段") {
    ntfscompact::Options opt_dry{make_options(Algorithm::kXpress8k)};
    opt_dry.b_dry_run = true;
    pl_result.u8_compressed_original = 3000;
    pl_result.u8_compressed_disk = 1000;
    CHECK(narrow(format_summary(pl_result, opt_dry)) ==
          "共 9 个文件：压缩 2、重压 1、解压 1、跳过 3；失败 2");
  }
}

TEST_CASE("汇总行：仅解压/跳过时不输出字节段") {
  PipelineResult pl_result{};
  FileReport fr_uncompress{};
  fr_uncompress.pl_plan.e_action = Plan::Action::kUncompress;
  FileReport fr_skip{};
  fr_skip.pl_plan.e_action = Plan::Action::kSkip;
  pl_result.vec_reports.push_back(fr_uncompress);
  pl_result.vec_reports.push_back(fr_skip);
  pl_result.u8_compressed_original = 4096;
  pl_result.u8_compressed_disk = 1024;
  CHECK(narrow(format_summary(pl_result, make_options(Algorithm::kXpress8k))) ==
        "共 2 个文件：压缩 0、重压 0、解压 1、跳过 1");
}

TEST_CASE("汇总行：执行失败与占用跳过的压缩项不计入字节段计数") {
  PipelineResult pl_result{};
  FileReport fr_ok{};
  fr_ok.pl_plan.e_action = Plan::Action::kCompress;
  fr_ok.b_executed = true;
  fr_ok.u8_logical_size = 100;
  FileReport fr_error{};
  fr_error.pl_plan.e_action = Plan::Action::kCompress;
  fr_error.b_executed = true;
  fr_error.er_exec.e_outcome = ntfscompact::ExecuteResult::Outcome::kError;
  fr_error.er_exec.str_message = L"复核失败（模拟）";
  fr_error.u8_logical_size = 200;
  FileReport fr_busy{};
  fr_busy.pl_plan.e_action = Plan::Action::kSkip;
  fr_busy.pl_plan.str_reason = L"占用不可释放";
  fr_busy.u8_logical_size = 400;
  pl_result.vec_reports.push_back(fr_ok);
  pl_result.vec_reports.push_back(fr_error);
  pl_result.vec_reports.push_back(fr_busy);
  pl_result.i4_failures = 1;
  pl_result.u8_compressed_original = 100;
  pl_result.u8_compressed_disk = 50;
  CHECK(narrow(format_summary(pl_result, make_options(Algorithm::kXpress8k))) ==
        "共 3 个文件：压缩 1、重压 0、解压 0、跳过 1；压缩/重压 100 → 50 字节，"
        "节省 50%；失败 1");
}

TEST_CASE("collect_byte_stats：真实临时文件磁盘占用与失败回退") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));

  PipelineResult pl_result{};
  FileReport fr_real{};
  fr_real.pl_plan.e_action = Plan::Action::kCompress;
  fr_real.str_long_path = ntfscompact_test::temp_long_api_path(
      ntfscompact_test::temp_sub(tmp_dir, L"plain.bin"));
  fr_real.u8_logical_size = 8192;
  FileReport fr_ghost{};
  fr_ghost.pl_plan.e_action = Plan::Action::kCompress;
  fr_ghost.str_long_path = L"";
  fr_ghost.u8_logical_size = 4096;
  pl_result.vec_reports.push_back(fr_real);
  pl_result.vec_reports.push_back(fr_ghost);
  collect_byte_stats(pl_result);
  CHECK(pl_result.u8_compressed_original == 12288);
  CHECK(pl_result.u8_compressed_disk == 8192 + 4096);
}

TEST_CASE("报告行：检测失败项输出失败行") {
  FileReport fr_report{};
  fr_report.str_display_path = L"C:\\d\\x.bin";
  fr_report.str_error = L"现状检测失败（模拟）";
  CHECK(narrow(format_report_line(fr_report, make_options(Algorithm::kXpress8k))) ==
        "[失败] C:\\d\\x.bin：现状检测失败（模拟）");
}

TEST_CASE("目录标记行：干跑、实跑与失败措辞") {
  const ntfscompact::Options opt_real{make_options(Algorithm::kXpress8k)};
  ntfscompact::Options opt_dry{make_options(Algorithm::kXpress8k)};
  opt_dry.b_dry_run = true;

  SUBCASE("干跑：将清除标记") {
    DirFlagReport df_report{};
    df_report.str_display_path = L"C:\\d\\sub";
    CHECK(narrow(format_dir_flag_line(df_report, opt_dry)) ==
          "[目录标记] C:\\d\\sub：将清除标记");
  }
  SUBCASE("实跑成功：已清除标记") {
    DirFlagReport df_report{};
    df_report.str_display_path = L"C:\\d\\sub";
    df_report.b_cleared = true;
    CHECK(narrow(format_dir_flag_line(df_report, opt_real)) ==
          "[目录标记] C:\\d\\sub：已清除标记");
  }
  SUBCASE("失败：错误行带清除器错误消息") {
    DirFlagReport df_report{};
    df_report.str_display_path = L"C:\\d\\sub";
    df_report.str_error = L"无法打开目录（错误码 5）";
    CHECK(narrow(format_dir_flag_line(df_report, opt_real)) ==
          "[失败] C:\\d\\sub：无法打开目录（错误码 5）");
  }
}

TEST_CASE("汇总行：目录标记计数段追加尾部") {
  SUBCASE("干跑：将清除目录标记 N 个") {
    PipelineResult pl_result{};
    DirFlagReport df_first{};
    df_first.str_display_path = L"C:\\d\\a";
    DirFlagReport df_second{};
    df_second.str_display_path = L"C:\\d\\b";
    pl_result.vec_dir_flags.push_back(df_first);
    pl_result.vec_dir_flags.push_back(df_second);
    ntfscompact::Options opt_dry{make_options(Algorithm::kXpress8k)};
    opt_dry.b_dry_run = true;
    CHECK(narrow(format_summary(pl_result, opt_dry)) ==
          "共 0 个文件：压缩 0、重压 0、解压 0、跳过 0；将清除目录标记 2 个");
  }
  SUBCASE("实跑：已清除目录标记仅计成功，失败计入失败段") {
    PipelineResult pl_result{};
    DirFlagReport df_ok{};
    df_ok.str_display_path = L"C:\\d\\a";
    df_ok.b_cleared = true;
    DirFlagReport df_fail{};
    df_fail.str_display_path = L"C:\\d\\b";
    df_fail.str_error = L"无法打开目录（错误码 5）";
    pl_result.vec_dir_flags.push_back(df_ok);
    pl_result.vec_dir_flags.push_back(df_fail);
    pl_result.i4_failures = 1;
    CHECK(narrow(format_summary(pl_result, make_options(Algorithm::kXpress8k))) ==
          "共 0 个文件：压缩 0、重压 0、解压 0、跳过 0；失败 1；已清除目录标记 1 个");
  }
  SUBCASE("无标记目录零计数段") {
    PipelineResult pl_result{};
    CHECK(narrow(format_summary(pl_result, make_options(Algorithm::kXpress8k))) ==
          "共 0 个文件：压缩 0、重压 0、解压 0、跳过 0");
    CHECK_FALSE(narrow(format_summary(pl_result, make_options(Algorithm::kXpress8k)))
                    .find("目录标记") != std::string::npos);
  }
}
