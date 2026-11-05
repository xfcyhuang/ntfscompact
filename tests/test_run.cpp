#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pipeline.h"
#include "temp_dir.h"

#include <windows.h>

namespace {

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::DetectionResult;
using ntfscompact::ExecuteFn;
using ntfscompact::ExecuteResult;
using ntfscompact::FileReport;
using ntfscompact::IDetector;
using ntfscompact::Options;
using ntfscompact::PipelineResult;
using ntfscompact::Plan;
using ntfscompact::RunPipeline;
using ntfscompact::RunScan;

class FakeDetector final : public IDetector {
public:
  DetectionResult Detect(const std::wstring& str_long_path) override {
    DetectionResult dr_result{};
    if (str_long_path.find(L"failme.bin") != std::wstring::npos) {
      dr_result.error = L"现状检测失败（模拟）";
      return dr_result;
    }
    const std::wstring str_name{
        str_long_path.substr(str_long_path.find_last_of(L"\\") + 1)};
    if (str_name == L"wof8k.bin") {
      dr_result.state.kind = CurrentState::Kind::Wof;
      dr_result.state.algorithm = Algorithm::Xpress8k;
      dr_result.state.provider = CurrentState::kProviderFile;
    } else if (str_name == L"woflzx.bin") {
      dr_result.state.kind = CurrentState::Kind::Wof;
      dr_result.state.algorithm = Algorithm::Lzx;
      dr_result.state.provider = CurrentState::kProviderFile;
    } else if (str_name == L"lznt1.bin") {
      dr_result.state.kind = CurrentState::Kind::Lznt1;
    } else if (str_name == L"wim.bin") {
      dr_result.state.kind = CurrentState::Kind::Wof;
      dr_result.state.algorithm = Algorithm::Xpress4k;
      dr_result.state.provider = CurrentState::kProviderWim;
    } else if (str_name == L"cloud.bin") {
      dr_result.state.kind = CurrentState::Kind::Wof;
      dr_result.state.algorithm = Algorithm::Xpress4k;
      dr_result.state.provider = CurrentState::kProviderCloud;
    }
    return dr_result;
  }
};

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

Options compress_options(Algorithm e_algorithm = Algorithm::Xpress8k) {
  Options opt_result{};
  opt_result.algorithm = e_algorithm;
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
  for (const FileReport& fr_item : result.reports) {
    if (fr_item.display_path.find(ptr_name) != std::wstring::npos) {
      return &fr_item;
    }
  }
  return nullptr;
}

std::wstring read_all(std::FILE* f_stream) {
  std::rewind(f_stream);
  std::wstring str_content{};
  char buf_bytes[4096]{};
  size_t n_bytes{0};
  while ((n_bytes = std::fread(buf_bytes, 1, sizeof(buf_bytes), f_stream)) > 0) {
    const int i4_wchars{MultiByteToWideChar(CP_UTF8, 0, buf_bytes,
                                            static_cast<int>(n_bytes), nullptr,
                                            0)};
    std::wstring str_chunk(static_cast<size_t>(i4_wchars), L'\0');
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

TEST_CASE("RunScan：各现状文件得到正确动作（注入假检测器）") {
  ntfscompact_test::TempDir tmp_dir;
  const std::wstring str_dir{make_fixture(tmp_dir)};

  Options opt_case{compress_options()};
  opt_case.paths.push_back(str_dir);
  FakeDetector detector_fake{};
  const PipelineResult result{RunScan(opt_case, detector_fake)};

  REQUIRE(result.reports.size() == 8);
  CHECK(result.failures == 1);

  const FileReport* ptr_plain{find_report(result, L"plain.bin")};
  REQUIRE(ptr_plain != nullptr);
  CHECK(ptr_plain->error.empty());
  CHECK(ptr_plain->plan.action == Plan::Action::Compress);
  CHECK(ptr_plain->has_state);
  CHECK(ptr_plain->state.kind == CurrentState::Kind::None);

  const FileReport* ptr_wof8k{find_report(result, L"wof8k.bin")};
  REQUIRE(ptr_wof8k != nullptr);
  CHECK(ptr_wof8k->plan.action == Plan::Action::Skip);
  CHECK(contains(ptr_wof8k->plan.reason, L"指定算法"));

  const FileReport* ptr_woflzx{find_report(result, L"woflzx.bin")};
  REQUIRE(ptr_woflzx != nullptr);
  CHECK(ptr_woflzx->plan.action == Plan::Action::Recompress);
  CHECK(ptr_woflzx->state.algorithm == Algorithm::Lzx);

  const FileReport* ptr_lznt1{find_report(result, L"lznt1.bin")};
  REQUIRE(ptr_lznt1 != nullptr);
  CHECK(ptr_lznt1->plan.action == Plan::Action::Recompress);

  const FileReport* ptr_wim{find_report(result, L"wim.bin")};
  REQUIRE(ptr_wim != nullptr);
  CHECK(ptr_wim->plan.action == Plan::Action::Skip);
  CHECK(contains(ptr_wim->plan.reason, L"WIM"));

  const FileReport* ptr_cloud{find_report(result, L"cloud.bin")};
  REQUIRE(ptr_cloud != nullptr);
  CHECK(ptr_cloud->plan.action == Plan::Action::Skip);
  CHECK(contains(ptr_cloud->plan.reason, L"Cloud"));

  const FileReport* ptr_failme{find_report(result, L"failme.bin")};
  REQUIRE(ptr_failme != nullptr);
  CHECK_FALSE(ptr_failme->error.empty());
  CHECK(contains(ptr_failme->error, L"现状检测失败"));

  const FileReport* ptr_tiny{find_report(result, L"tiny.bin")};
  REQUIRE(ptr_tiny != nullptr);
  CHECK(ptr_tiny->plan.action == Plan::Action::Skip);
  CHECK(contains(ptr_tiny->plan.reason, L"4KB"));
  CHECK_FALSE(ptr_tiny->has_state);
}

TEST_CASE("RunScan：解压模式下各文件动作") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"woflzx.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"lznt1.bin", 8192));

  Options opt_case{};
  opt_case.uncompress = true;
  opt_case.paths.push_back(tmp_dir.path());
  FakeDetector detector_fake{};
  const PipelineResult result{RunScan(opt_case, detector_fake)};

  const FileReport* ptr_plain{find_report(result, L"plain.bin")};
  REQUIRE(ptr_plain != nullptr);
  CHECK(ptr_plain->plan.action == Plan::Action::Skip);
  const FileReport* ptr_woflzx{find_report(result, L"woflzx.bin")};
  REQUIRE(ptr_woflzx != nullptr);
  CHECK(ptr_woflzx->plan.action == Plan::Action::Uncompress);
  const FileReport* ptr_lznt1{find_report(result, L"lznt1.bin")};
  REQUIRE(ptr_lznt1 != nullptr);
  CHECK(ptr_lznt1->plan.action == Plan::Action::Uncompress);
}

TEST_CASE("RunPipeline：干跑输出每文件计划行与汇总，不执行，退出码 0") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"wof8k.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"woflzx.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.dry_run = true;
  opt_case.paths.push_back(tmp_dir.path());
  FakeDetector detector_fake{};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{RunPipeline(opt_case, detector_fake, recorder.fn(), f_out,
                                f_out)};
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

TEST_CASE("RunPipeline：真实模式执行压缩，每文件结果行与汇总落账") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"skipme.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"execfail.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.paths.push_back(tmp_dir.path());
  FakeDetector detector_fake{};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{RunPipeline(opt_case, detector_fake, recorder.fn(), f_out,
                                f_err)};
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
    CHECK(call_item.e_action == Plan::Action::Compress);
  }
}

TEST_CASE("RunPipeline：真实模式重压——LZNT1 与异算法 WOF 传 Recompress") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"lznt1.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"woflzx.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.paths.push_back(tmp_dir.path());
  FakeDetector detector_fake{};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  RunPipeline(opt_case, detector_fake, recorder.fn(), f_out, f_out);

  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  CHECK(contains(str_text, L"lznt1.bin：已重压为指定算法 xpress8k"));
  CHECK(contains(str_text, L"woflzx.bin：已重压为指定算法 xpress8k"));
  CHECK(contains(str_text, L"共 2 个文件：压缩 0、重压 2、解压 0、跳过 0"));
  REQUIRE(recorder.vec_calls.size() == 2);
  for (const ExecuteCall& call_item : recorder.vec_calls) {
    CHECK(call_item.e_action == Plan::Action::Recompress);
  }
}

TEST_CASE("RunPipeline：真实模式失败后继续跑完后续文件") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"a_execfail.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"b_plain.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.paths.push_back(tmp_dir.path());
  FakeDetector detector_fake{};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  RunPipeline(opt_case, detector_fake, recorder.fn(), f_out, f_err);

  CHECK(recorder.vec_calls.size() == 2);
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  std::fclose(f_err);
  CHECK(contains(str_text, L"b_plain.bin：已压缩为指定算法 xpress8k"));
}

TEST_CASE("RunPipeline：Skip 与解压分支不调用执行器（解压属工单 T4）") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"wof8k.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"wim.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"lznt1.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.uncompress = true;
  opt_case.paths.push_back(tmp_dir.path());
  FakeDetector detector_fake{};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{RunPipeline(opt_case, detector_fake, recorder.fn(), f_out,
                                f_out)};
  CHECK(i4_code == 0);
  std::fclose(f_out);
  CHECK(recorder.vec_calls.empty());
}

TEST_CASE("RunPipeline：普通模式输出每文件结果行与汇总") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));
  REQUIRE(tmp_dir.WriteFile(L"wof8k.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.paths.push_back(tmp_dir.path());
  FakeDetector detector_fake{};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{RunPipeline(opt_case, detector_fake, recorder.fn(), f_out,
                                f_out)};
  CHECK(i4_code == 0);

  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  CHECK(contains(str_text, L"[压缩] "));
  CHECK(contains(str_text, L"plain.bin：已压缩为指定算法 xpress8k"));
  CHECK(contains(str_text, L"[跳过] "));
  CHECK(contains(str_text, L"共 2 个文件：压缩 1、重压 0、解压 0、跳过 1"));
}

TEST_CASE("RunPipeline：失败路径计入退出码 1，失败行照打") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.MakeDir(L"sub"));
  REQUIRE(tmp_dir.WriteFile(L"sub\\ghost.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.paths.push_back(tmp_dir.path());
  opt_case.paths.push_back(tmp_dir.Sub(L"ghost.bin"));
  FakeDetector detector_fake{};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{RunPipeline(opt_case, detector_fake, recorder.fn(), f_out,
                                f_err)};
  CHECK(i4_code == 1);

  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);
  CHECK(contains(str_err_text, L"[失败] "));
  CHECK(contains(str_err_text, L"ghost.bin"));
}

TEST_CASE("RunPipeline：-q 静默不输出") {
  ntfscompact_test::TempDir tmp_dir;
  REQUIRE(tmp_dir.ok());
  REQUIRE(tmp_dir.WriteFile(L"plain.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.quiet = true;
  opt_case.paths.push_back(tmp_dir.path());
  FakeDetector detector_fake{};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{RunPipeline(opt_case, detector_fake, recorder.fn(), f_out,
                                f_out)};
  std::fclose(f_out);
  CHECK(i4_code == 0);
  CHECK(recorder.vec_calls.size() == 1);
}
