#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pipeline.hpp"
#include "temp_dir.hpp"

#include <windows.h>

namespace {

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::DetectionResult;
using ntfscompact::ExecuteFn;
using ntfscompact::ExecuteResult;
using ntfscompact::FileReport;
using ntfscompact::DetectFn;
using ntfscompact::Options;
using ntfscompact::PipelineResult;
using ntfscompact::Plan;
using ntfscompact::run_pipeline;
using ntfscompact::run_scan;
using ntfscompact::UncompressFn;

DetectionResult fake_detect(const std::wstring& str_long_path) {
    DetectionResult dr_result{};
    if (str_long_path.find(L"failme.bin") != std::wstring::npos) {
      dr_result.str_error = L"现状检测失败（模拟）";
      return dr_result;
    }
    const std::wstring str_name{
        str_long_path.substr(str_long_path.find_last_of(L"\\") + 1)};
    if (str_name == L"wof8k.bin") {
      dr_result.cs_state.e_kind = CurrentState::Kind::kWof;
      dr_result.cs_state.e_algorithm = Algorithm::kXpress8k;
      dr_result.cs_state.i4_provider = CurrentState::kProviderFile;
    } else if (str_name == L"woflzx.bin") {
      dr_result.cs_state.e_kind = CurrentState::Kind::kWof;
      dr_result.cs_state.e_algorithm = Algorithm::kLzx;
      dr_result.cs_state.i4_provider = CurrentState::kProviderFile;
    } else if (str_name == L"lznt1.bin") {
      dr_result.cs_state.e_kind = CurrentState::Kind::kLznt1;
    } else if (str_name == L"wim.bin") {
      dr_result.cs_state.e_kind = CurrentState::Kind::kWof;
      dr_result.cs_state.e_algorithm = Algorithm::kXpress4k;
      dr_result.cs_state.i4_provider = CurrentState::kProviderWim;
    } else if (str_name == L"cloud.bin") {
      dr_result.cs_state.e_kind = CurrentState::Kind::kWof;
      dr_result.cs_state.e_algorithm = Algorithm::kXpress4k;
      dr_result.cs_state.i4_provider = CurrentState::kProviderCloud;
    }
    return dr_result;
  }

struct ExecuteCall {
  std::wstring str_long_path;
  Plan::Action e_action{};
  CurrentState cs_current{};
};

struct ExecuteRecorder {
  std::vector<ExecuteCall> vec_calls{};

  ExecuteFn fn() {
    return [this](const std::wstring& str_long_path, Plan::Action e_action,
                  const CurrentState& cs_current, Algorithm) {
      vec_calls.push_back({str_long_path, e_action, cs_current});
      ExecuteResult er_result{};
      const std::wstring str_name{
          str_long_path.substr(str_long_path.find_last_of(L"\\") + 1)};
      if (str_name == L"skipme.bin") {
        er_result.e_outcome = ExecuteResult::Outcome::kSkipped;
        er_result.str_message = L"无收益（内核判定压缩无容量收益），文件保持原样";
      } else if (str_name == L"execfail.bin") {
        er_result.e_outcome = ExecuteResult::Outcome::kError;
        er_result.str_message = L"压缩失败（模拟）";
      }
      return er_result;
    };
  }
};

Options compress_options(Algorithm e_algorithm = Algorithm::kXpress8k) {
  Options opt_result{};
  opt_result.e_algorithm = e_algorithm;
  return opt_result;
}

std::wstring make_fixture(ntfscompact_test::TempDir& tmp_dir) {
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"wof8k.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"woflzx.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"lznt1.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"wim.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"cloud.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"failme.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"tiny.bin", 100));
  return tmp_dir.path();
}

const FileReport* find_report(const PipelineResult& result, const wchar_t* ptr_name) {
  for (const FileReport& fr_item : result.vec_reports) {
    if (fr_item.str_display_path.find(ptr_name) != std::wstring::npos) {
      return &fr_item;
    }
  }
  return nullptr;
}

std::wstring read_all(std::FILE* f_stream) {
  std::rewind(f_stream);
  std::wstring str_content{};
  char buf_bytes[4096]{};
  std::size_t n_bytes{0};
  while ((n_bytes = std::fread(buf_bytes, 1, sizeof(buf_bytes), f_stream)) > 0) {
    const int i4_wchars{MultiByteToWideChar(CP_UTF8, 0, buf_bytes,
                                            static_cast<int>(n_bytes), nullptr,
                                            0)};
    std::wstring str_chunk(static_cast<std::size_t>(i4_wchars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf_bytes, static_cast<int>(n_bytes),
                        str_chunk.data(), i4_wchars);
    str_content += str_chunk;
  }
  return str_content;
}

bool contains(const std::wstring& str_haystack, const std::wstring& str_needle) {
  return str_haystack.find(str_needle) != std::wstring::npos;
}

}

TEST_CASE("run_scan：各现状文件得到正确动作（注入假检测器）") {
  ntfscompact_test::TempDir tmp_dir;
  const std::wstring str_dir{make_fixture(tmp_dir)};

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(str_dir);
  const DetectFn fn_detect{fake_detect};
  const PipelineResult result{run_scan(opt_case, fn_detect)};

  REQUIRE(result.vec_reports.size() == 8);
  CHECK(result.i4_failures == 1);

  const FileReport* ptr_plain{find_report(result, L"plain.bin")};
  REQUIRE(ptr_plain != nullptr);
  CHECK(ptr_plain->str_error.empty());
  CHECK(ptr_plain->pl_plan.e_action == Plan::Action::kCompress);
  CHECK(ptr_plain->b_has_state);
  CHECK(ptr_plain->cs_state.e_kind == CurrentState::Kind::kNone);

  const FileReport* ptr_wof8k{find_report(result, L"wof8k.bin")};
  REQUIRE(ptr_wof8k != nullptr);
  CHECK(ptr_wof8k->pl_plan.e_action == Plan::Action::kSkip);
  CHECK(contains(ptr_wof8k->pl_plan.str_reason, L"指定算法"));

  const FileReport* ptr_woflzx{find_report(result, L"woflzx.bin")};
  REQUIRE(ptr_woflzx != nullptr);
  CHECK(ptr_woflzx->pl_plan.e_action == Plan::Action::kRecompress);
  CHECK(ptr_woflzx->cs_state.e_algorithm == Algorithm::kLzx);

  const FileReport* ptr_lznt1{find_report(result, L"lznt1.bin")};
  REQUIRE(ptr_lznt1 != nullptr);
  CHECK(ptr_lznt1->pl_plan.e_action == Plan::Action::kRecompress);

  const FileReport* ptr_wim{find_report(result, L"wim.bin")};
  REQUIRE(ptr_wim != nullptr);
  CHECK(ptr_wim->pl_plan.e_action == Plan::Action::kSkip);
  CHECK(contains(ptr_wim->pl_plan.str_reason, L"WIM"));

  const FileReport* ptr_cloud{find_report(result, L"cloud.bin")};
  REQUIRE(ptr_cloud != nullptr);
  CHECK(ptr_cloud->pl_plan.e_action == Plan::Action::kSkip);
  CHECK(contains(ptr_cloud->pl_plan.str_reason, L"Cloud"));

  const FileReport* ptr_failme{find_report(result, L"failme.bin")};
  REQUIRE(ptr_failme != nullptr);
  CHECK_FALSE(ptr_failme->str_error.empty());
  CHECK(contains(ptr_failme->str_error, L"现状检测失败"));

  const FileReport* ptr_tiny{find_report(result, L"tiny.bin")};
  REQUIRE(ptr_tiny != nullptr);
  CHECK(ptr_tiny->pl_plan.e_action == Plan::Action::kSkip);
  CHECK(contains(ptr_tiny->pl_plan.str_reason, L"4KB"));
  CHECK_FALSE(ptr_tiny->b_has_state);
}

TEST_CASE("run_scan：解压模式下各文件动作") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"woflzx.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"lznt1.bin", 8192));

  Options opt_case{};
  opt_case.b_uncompress = true;
  opt_case.vec_paths.push_back(tmp_dir.path());
  const DetectFn fn_detect{fake_detect};
  const PipelineResult result{run_scan(opt_case, fn_detect)};

  const FileReport* ptr_plain{find_report(result, L"plain.bin")};
  REQUIRE(ptr_plain != nullptr);
  CHECK(ptr_plain->pl_plan.e_action == Plan::Action::kSkip);
  const FileReport* ptr_woflzx{find_report(result, L"woflzx.bin")};
  REQUIRE(ptr_woflzx != nullptr);
  CHECK(ptr_woflzx->pl_plan.e_action == Plan::Action::kUncompress);
  const FileReport* ptr_lznt1{find_report(result, L"lznt1.bin")};
  REQUIRE(ptr_lznt1 != nullptr);
  CHECK(ptr_lznt1->pl_plan.e_action == Plan::Action::kUncompress);
}

TEST_CASE("run_pipeline：干跑输出每文件计划行与汇总，不执行，退出码 0") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"wof8k.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"woflzx.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.b_dry_run = true;
  opt_case.vec_paths.push_back(tmp_dir.path());
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, recorder.fn(),
                                UncompressFn{}, f_out, f_out)};
  CHECK(i4_code == 0);

  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  CHECK(contains(str_text, L"[压缩] "));
  CHECK(contains(str_text, L"plain.bin"));
  CHECK(contains(str_text, L"将压缩为指定算法 xpress8k"));
  CHECK(contains(str_text, L"[重压] "));
  CHECK(contains(str_text, L"[跳过] "));
  CHECK(contains(str_text, L"已是指定算法 xpress8k"));
  CHECK(contains(str_text, L"共 3 个文件：压缩 1、重压 1、解压 0、跳过 1"));
  CHECK(recorder.vec_calls.empty());
}

TEST_CASE("run_pipeline：真实模式执行压缩，每文件结果行与汇总落账") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"skipme.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"execfail.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(tmp_dir.path());
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, recorder.fn(),
                                UncompressFn{}, f_out, f_err)};
  CHECK(i4_code == 1);

  const std::wstring str_text{read_all(f_out)};
  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);
  CHECK(contains(str_text, L"[压缩] "));
  CHECK(contains(str_text, L"plain.bin：已压缩为指定算法 xpress8k"));
  CHECK(contains(str_text, L"skipme.bin：无收益"));
  CHECK(contains(str_err_text, L"execfail.bin：压缩失败（模拟）"));
  CHECK(contains(str_text, L"共 3 个文件：压缩 1、重压 0、解压 0、跳过 1；失败 1"));

  REQUIRE(recorder.vec_calls.size() == 3);
  for (const ExecuteCall& call_item : recorder.vec_calls) {
    CHECK(contains(call_item.str_long_path, L"\\\\?\\"));
    CHECK(call_item.e_action == Plan::Action::kCompress);
  }
}

TEST_CASE("run_pipeline：真实模式重压——LZNT1 与异算法 WOF 传 Recompress") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"lznt1.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"woflzx.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(tmp_dir.path());
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  run_pipeline(opt_case, fn_detect, recorder.fn(), UncompressFn{}, f_out,
              f_out);

  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  CHECK(contains(str_text, L"lznt1.bin：已重压为指定算法 xpress8k"));
  CHECK(contains(str_text, L"woflzx.bin：已重压为指定算法 xpress8k"));
  CHECK(contains(str_text, L"共 2 个文件：压缩 0、重压 2、解压 0、跳过 0"));
  REQUIRE(recorder.vec_calls.size() == 2);
  for (const ExecuteCall& call_item : recorder.vec_calls) {
    CHECK(call_item.e_action == Plan::Action::kRecompress);
  }
}

TEST_CASE("run_pipeline：真实模式失败后继续跑完后续文件") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"a_execfail.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"b_plain.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(tmp_dir.path());
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  run_pipeline(opt_case, fn_detect, recorder.fn(), UncompressFn{}, f_out,
              f_err);

  CHECK(recorder.vec_calls.size() == 2);
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  std::fclose(f_err);
  CHECK(contains(str_text, L"b_plain.bin：已压缩为指定算法 xpress8k"));
}

TEST_CASE("run_pipeline：Skip 与解压分支分派解压执行器，不碰压缩执行器") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"wof8k.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"wim.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"lznt1.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.b_uncompress = true;
  opt_case.vec_paths.push_back(tmp_dir.path());
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::vector<std::wstring> vec_uncompress_calls{};
  const UncompressFn fn_uncompress{
      [&vec_uncompress_calls](const std::wstring& str_long_path)
          -> std::expected<void, std::wstring> {
        vec_uncompress_calls.push_back(str_long_path);
        return {};
      }};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, recorder.fn(),
                                fn_uncompress, f_out, f_out)};
  CHECK(i4_code == 0);
  std::fclose(f_out);
  CHECK(recorder.vec_calls.empty());
  REQUIRE(vec_uncompress_calls.size() == 2);
  bool b_has_wof8k{false};
  bool b_has_lznt1{false};
  for (const std::wstring& str_call : vec_uncompress_calls) {
    b_has_wof8k = b_has_wof8k || contains(str_call, L"wof8k.bin");
    b_has_lznt1 = b_has_lznt1 || contains(str_call, L"lznt1.bin");
  }
  CHECK(b_has_wof8k);
  CHECK(b_has_lznt1);
}

TEST_CASE("run_pipeline：普通模式输出每文件结果行与汇总") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"wof8k.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(tmp_dir.path());
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, recorder.fn(),
                                UncompressFn{}, f_out, f_out)};
  CHECK(i4_code == 0);

  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  CHECK(contains(str_text, L"[压缩] "));
  CHECK(contains(str_text, L"plain.bin：已压缩为指定算法 xpress8k"));
  CHECK(contains(str_text, L"[跳过] "));
  CHECK(contains(str_text, L"共 2 个文件：压缩 1、重压 0、解压 0、跳过 1"));
}

TEST_CASE("run_pipeline：失败路径计入退出码 1，失败行照打") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.MakeDir(L"sub"));
  REQUIRE(tmp_dir.WriteFile(L"sub\\ghost.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(tmp_dir.path());
  opt_case.vec_paths.push_back(tmp_dir.Sub(L"ghost.bin"));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, recorder.fn(),
                                UncompressFn{}, f_out, f_err)};
  CHECK(i4_code == 1);

  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);
  CHECK(contains(str_err_text, L"[失败] "));
  CHECK(contains(str_err_text, L"ghost.bin"));
}

TEST_CASE("run_pipeline：-q 静默不输出") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.b_quiet = true;
  opt_case.vec_paths.push_back(tmp_dir.path());
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, recorder.fn(),
                                UncompressFn{}, f_out, f_out)};
  std::fclose(f_out);
  CHECK(i4_code == 0);
  CHECK(recorder.vec_calls.size() == 1);
}
