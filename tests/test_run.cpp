#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "pipeline.hpp"
#include "release.hpp"
#include "temp_dir.hpp"
#include "util.hpp"

#include <windows.h>

namespace {

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::DetectionResult;
using ntfscompact::DirFlagFn;
using ntfscompact::ExecuteFn;
using ntfscompact::ExecuteResult;
using ntfscompact::FileReport;
using ntfscompact::DetectFn;
using ntfscompact::Options;
using ntfscompact::PipelineResult;
using ntfscompact::Plan;
using ntfscompact::ReleaseFn;
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
  std::wstring str_long_path{};
  Plan::Action e_action{};
  CurrentState cs_current{};
};

struct ExecuteRecorder {
  std::vector<ExecuteCall> vec_calls{};
};

ExecuteFn make_record_executor(ExecuteRecorder& rec_record) {
  return [&rec_record](const std::wstring& str_long_path, Plan::Action e_action,
                  const CurrentState& cs_current, Algorithm) {
      rec_record.vec_calls.push_back({str_long_path, e_action, cs_current});
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

Options compress_options(Algorithm e_algorithm = Algorithm::kXpress8k) {
  Options opt_result{};
  opt_result.e_algorithm = e_algorithm;
  return opt_result;
}

struct TestLockReleaser {
  void operator()(HANDLE h_lock) const {
    if (h_lock != INVALID_HANDLE_VALUE) {
      CloseHandle(h_lock);
    }
  }
};

using TestFileLock =
    std::unique_ptr<std::remove_pointer_t<HANDLE>, TestLockReleaser>;

TestFileLock lock_test_file(const std::wstring& str_long_path) {
  HANDLE h_lock{CreateFileW(str_long_path.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr)};
  if (h_lock == INVALID_HANDLE_VALUE) {
    return TestFileLock{};
  }
  return TestFileLock{h_lock};
}

struct ReleaseRecorder {
  std::vector<std::wstring> vec_calls{};
  ntfscompact::ReleaseOutcome ro_result{};
};

ReleaseRecorder make_release_recorder() {
  ReleaseRecorder rec_result{};
  rec_result.ro_result.e_status =
      ntfscompact::ReleaseOutcome::Status::kReleased;
  rec_result.ro_result.e_level = ntfscompact::ReleaseOutcome::Level::kRmForce;
  rec_result.ro_result.vec_closed = {{4242, L"hold_app.exe"}};
  rec_result.ro_result.str_detail = L"经 RM 强制关释放";
  return rec_result;
}

ntfscompact::ReleaseFn make_record_releaser(ReleaseRecorder& rec_record) {
  return [&rec_record](const std::wstring& str_long_path) {
    rec_record.vec_calls.push_back(str_long_path);
    return rec_record.ro_result;
  };
}

ntfscompact::ReleaseFn make_busy_releaser(std::vector<std::wstring>& vec_calls) {
  return [&vec_calls](const std::wstring& str_long_path) {
    vec_calls.push_back(str_long_path);
    ntfscompact::ReleaseOutcome ro_busy{};
    ro_busy.e_status = ntfscompact::ReleaseOutcome::Status::kBusy;
    ro_busy.str_detail = L"占用不可释放：升级链尽后仍被占用（模拟）";
    return ro_busy;
  };
}

std::wstring make_fixture(ntfscompact_test::TempDir& tmp_dir) {
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"wof8k.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"woflzx.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"lznt1.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"wim.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"cloud.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"failme.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"tiny.bin", 100));
  return ntfscompact_test::temp_path(tmp_dir);
}

const FileReport* find_report(const PipelineResult& pr_result,
                              const wchar_t* ptr_name) {
  for (const FileReport& fr_item : pr_result.vec_reports) {
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

std::size_t count_lines(const std::wstring& str_text) {
  if (str_text.empty()) {
    return 0;
  }
  std::size_t u8_lines{1};
  for (wchar_t ch_wide : str_text) {
    if (ch_wide == L'\n') {
      ++u8_lines;
    }
  }
  if (str_text.back() == L'\n') {
    --u8_lines;
  }
  return u8_lines;
}

bool contains(const std::wstring& str_haystack, const std::wstring& str_needle) {
  return str_haystack.find(str_needle) != std::wstring::npos;
}

}

TEST_CASE("run_scan：各现状文件得到正确动作（注入假检测器）") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  const std::wstring str_dir{make_fixture(tmp_dir)};

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(str_dir);
  const DetectFn fn_detect{fake_detect};
  const PipelineResult pr_result{run_scan(opt_case, fn_detect)};

  REQUIRE(pr_result.vec_reports.size() == 8);
  CHECK(pr_result.i4_failures == 1);

  const FileReport* ptr_plain{find_report(pr_result, L"plain.bin")};
  REQUIRE(ptr_plain != nullptr);
  CHECK(ptr_plain->str_error.empty());
  CHECK(ptr_plain->pl_plan.e_action == Plan::Action::kCompress);
  CHECK(ptr_plain->b_has_state);
  CHECK(ptr_plain->cs_state.e_kind == CurrentState::Kind::kNone);

  const FileReport* ptr_wof8k{find_report(pr_result, L"wof8k.bin")};
  REQUIRE(ptr_wof8k != nullptr);
  CHECK(ptr_wof8k->pl_plan.e_action == Plan::Action::kSkip);
  CHECK(contains(ptr_wof8k->pl_plan.str_reason, L"指定算法"));

  const FileReport* ptr_woflzx{find_report(pr_result, L"woflzx.bin")};
  REQUIRE(ptr_woflzx != nullptr);
  CHECK(ptr_woflzx->pl_plan.e_action == Plan::Action::kRecompress);
  CHECK(ptr_woflzx->cs_state.e_algorithm == Algorithm::kLzx);

  const FileReport* ptr_lznt1{find_report(pr_result, L"lznt1.bin")};
  REQUIRE(ptr_lznt1 != nullptr);
  CHECK(ptr_lznt1->pl_plan.e_action == Plan::Action::kRecompress);

  const FileReport* ptr_wim{find_report(pr_result, L"wim.bin")};
  REQUIRE(ptr_wim != nullptr);
  CHECK(ptr_wim->pl_plan.e_action == Plan::Action::kSkip);
  CHECK(contains(ptr_wim->pl_plan.str_reason, L"WIM"));

  const FileReport* ptr_cloud{find_report(pr_result, L"cloud.bin")};
  REQUIRE(ptr_cloud != nullptr);
  CHECK(ptr_cloud->pl_plan.e_action == Plan::Action::kSkip);
  CHECK(contains(ptr_cloud->pl_plan.str_reason, L"Cloud"));

  const FileReport* ptr_failme{find_report(pr_result, L"failme.bin")};
  REQUIRE(ptr_failme != nullptr);
  CHECK_FALSE(ptr_failme->str_error.empty());
  CHECK(contains(ptr_failme->str_error, L"现状检测失败"));

  const FileReport* ptr_tiny{find_report(pr_result, L"tiny.bin")};
  REQUIRE(ptr_tiny != nullptr);
  CHECK(ptr_tiny->pl_plan.e_action == Plan::Action::kSkip);
  CHECK(contains(ptr_tiny->pl_plan.str_reason, L"4KB"));
  CHECK_FALSE(ptr_tiny->b_has_state);
}

TEST_CASE("run_scan：解压模式下各文件动作") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"woflzx.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"lznt1.bin", 8192));

  Options opt_case{};
  opt_case.b_uncompress = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  const PipelineResult pr_result{run_scan(opt_case, fn_detect)};

  const FileReport* ptr_plain{find_report(pr_result, L"plain.bin")};
  REQUIRE(ptr_plain != nullptr);
  CHECK(ptr_plain->pl_plan.e_action == Plan::Action::kSkip);
  const FileReport* ptr_woflzx{find_report(pr_result, L"woflzx.bin")};
  REQUIRE(ptr_woflzx != nullptr);
  CHECK(ptr_woflzx->pl_plan.e_action == Plan::Action::kUncompress);
  const FileReport* ptr_lznt1{find_report(pr_result, L"lznt1.bin")};
  REQUIRE(ptr_lznt1 != nullptr);
  CHECK(ptr_lznt1->pl_plan.e_action == Plan::Action::kUncompress);
}

TEST_CASE("run_pipeline：干跑输出每文件计划行与汇总，不执行，退出码 0") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"wof8k.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"woflzx.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.b_dry_run = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, DirFlagFn{}, f_out, f_out)};
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
  CHECK_FALSE(contains(str_text, L"压缩/重压"));
  CHECK(recorder.vec_calls.empty());
}

TEST_CASE("run_pipeline：干跑遇占用文件不触发释放") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  TestFileLock lock_file{
      lock_test_file(ntfscompact_test::temp_long_api_path(
          ntfscompact_test::temp_sub(tmp_dir, L"plain.bin")))};
  REQUIRE(lock_file);

  Options opt_case{compress_options()};
  opt_case.b_dry_run = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  ReleaseRecorder rec_release{make_release_recorder()};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, make_record_releaser(rec_release), DirFlagFn{},
                                f_out, f_out)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  CHECK(rec_release.vec_calls.empty());
  CHECK(recorder.vec_calls.empty());
  CHECK_FALSE(contains(str_text, L"升级释放"));
  CHECK(contains(str_text, L"[压缩] "));
}

TEST_CASE("run_pipeline：真实模式执行压缩，每文件结果行与汇总落账") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"skipme.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"execfail.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, DirFlagFn{}, f_out, f_err)};
  CHECK(i4_code == 1);

  const std::wstring str_text{read_all(f_out)};
  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);
  CHECK(contains(str_text, L"[压缩] "));
  CHECK(contains(str_text, L"plain.bin：已压缩为指定算法 xpress8k"));
  CHECK(contains(str_text, L"skipme.bin：无收益"));
  CHECK(contains(str_err_text, L"execfail.bin：压缩失败（模拟）"));
  CHECK(contains(str_text, L"共 3 个文件：压缩 1、重压 0、解压 0、跳过 1"));
  CHECK(contains(str_text, L"；压缩/重压 8192 → 8192 字节，节省 0%"));
  CHECK(contains(str_text, L"；失败 1"));

  REQUIRE(recorder.vec_calls.size() == 3);
  for (const ExecuteCall& call_item : recorder.vec_calls) {
    CHECK(contains(call_item.str_long_path, L"\\\\?\\"));
    CHECK(call_item.e_action == Plan::Action::kCompress);
  }
}

TEST_CASE("run_pipeline：真实模式重压——LZNT1 与异算法 WOF 传 Recompress") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"lznt1.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"woflzx.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  run_pipeline(opt_case, fn_detect, make_record_executor(recorder), UncompressFn{},
              ReleaseFn{}, DirFlagFn{}, f_out, f_out);

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
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"a_execfail.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"b_plain.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  run_pipeline(opt_case, fn_detect, make_record_executor(recorder), UncompressFn{},
              ReleaseFn{}, DirFlagFn{}, f_out, f_err);

  CHECK(recorder.vec_calls.size() == 2);
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  std::fclose(f_err);
  CHECK(contains(str_text, L"b_plain.bin：已压缩为指定算法 xpress8k"));
}

TEST_CASE("run_pipeline：Skip 与解压分支分派解压执行器，不碰压缩执行器") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"wof8k.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"wim.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"lznt1.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.b_uncompress = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
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
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                fn_uncompress, ReleaseFn{}, DirFlagFn{}, f_out, f_out)};
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
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"wof8k.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, DirFlagFn{}, f_out, f_out)};
  CHECK(i4_code == 0);

  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  CHECK(contains(str_text, L"[压缩] "));
  CHECK(contains(str_text, L"plain.bin：已压缩为指定算法 xpress8k"));
  CHECK(contains(str_text, L"[跳过] "));
  CHECK(contains(str_text, L"共 2 个文件：压缩 1、重压 0、解压 0、跳过 1"));
}

TEST_CASE("run_pipeline：失败路径计入退出码 1，失败行照打") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"sub"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"sub\\ghost.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  opt_case.vec_paths.push_back(ntfscompact_test::temp_sub(tmp_dir, L"ghost.bin"));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, DirFlagFn{}, f_out, f_err)};
  CHECK(i4_code == 1);

  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);
  CHECK(contains(str_err_text, L"[失败] "));
  CHECK(contains(str_err_text, L"ghost.bin"));
}

TEST_CASE("run_pipeline：-q 仅输出汇总一行") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"wof8k.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.b_quiet = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, DirFlagFn{}, f_out, f_err)};
  const std::wstring str_text{read_all(f_out)};
  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);

  CHECK(i4_code == 0);
  CHECK(recorder.vec_calls.size() == 1);
  CHECK(count_lines(str_text) == 1);
  CHECK(contains(str_text, L"共 2 个文件：压缩 1、重压 0、解压 0、跳过 1"));
  CHECK_FALSE(contains(str_text, L"["));
  CHECK(str_err_text.empty());
}

TEST_CASE("run_pipeline：-q 失败行仍走 stderr 且退出码 1") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"execfail.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.b_quiet = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, DirFlagFn{}, f_out, f_err)};
  const std::wstring str_text{read_all(f_out)};
  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);

  CHECK(i4_code == 1);
  CHECK(count_lines(str_text) == 1);
  CHECK(contains(str_text, L"；失败 1"));
  CHECK(contains(str_err_text, L"[失败] "));
  CHECK(contains(str_err_text, L"execfail.bin：压缩失败（模拟）"));
  CHECK(count_lines(str_err_text) == 1);
}

TEST_CASE("run_pipeline：退出码边界——全部跳过为 0，全部失败为 1") {
  SUBCASE("全部跳过（已是指定算法）退出码 0") {
    ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
    REQUIRE(tmp_dir->b_ok);
    REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"wof8k.bin", 8192));
    REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"tiny.bin", 100));

    Options opt_case{compress_options()};
    opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
    const DetectFn fn_detect{fake_detect};
    ExecuteRecorder recorder{};
    std::FILE* f_out{std::tmpfile()};
    REQUIRE(f_out != nullptr);
    const int i4_code{run_pipeline(opt_case, fn_detect,
                                   make_record_executor(recorder), UncompressFn{},
                                   ReleaseFn{}, DirFlagFn{}, f_out, f_out)};
    const std::wstring str_text{read_all(f_out)};
    std::fclose(f_out);
    CHECK(i4_code == 0);
    CHECK(recorder.vec_calls.empty());
    CHECK(contains(str_text, L"共 2 个文件：压缩 0、重压 0、解压 0、跳过 2"));
  }
  SUBCASE("全部执行失败退出码 1") {
    ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
    REQUIRE(tmp_dir->b_ok);
    REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"sub"));
    REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"execfail.bin", 8192));
    REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"sub\\execfail.bin", 8192));

    Options opt_case{compress_options()};
    opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
    const DetectFn fn_detect{fake_detect};
    ExecuteRecorder recorder{};
    std::FILE* f_out{std::tmpfile()};
    std::FILE* f_err{std::tmpfile()};
    REQUIRE(f_out != nullptr);
    REQUIRE(f_err != nullptr);
    const int i4_code{run_pipeline(opt_case, fn_detect,
                                   make_record_executor(recorder), UncompressFn{},
                                   ReleaseFn{}, DirFlagFn{}, f_out, f_err)};
    const std::wstring str_text{read_all(f_out)};
    const std::wstring str_err_text{read_all(f_err)};
    std::fclose(f_out);
    std::fclose(f_err);
    CHECK(i4_code == 1);
    CHECK(contains(str_text, L"；失败 2"));
    CHECK(count_lines(str_err_text) == 2);
    CHECK_FALSE(contains(str_text, L"压缩/重压"));
  }
  SUBCASE("干跑含检测失败退出码 1") {
    ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
    REQUIRE(tmp_dir->b_ok);
    REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"failme.bin", 8192));

    Options opt_case{compress_options()};
    opt_case.b_dry_run = true;
    opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
    const DetectFn fn_detect{fake_detect};
    ExecuteRecorder recorder{};
    std::FILE* f_out{std::tmpfile()};
    std::FILE* f_err{std::tmpfile()};
    REQUIRE(f_out != nullptr);
    REQUIRE(f_err != nullptr);
    const int i4_code{run_pipeline(opt_case, fn_detect,
                                   make_record_executor(recorder), UncompressFn{},
                                   ReleaseFn{}, DirFlagFn{}, f_out, f_err)};
    const std::wstring str_err_text{read_all(f_err)};
    std::fclose(f_out);
    std::fclose(f_err);
    CHECK(i4_code == 1);
    CHECK(contains(str_err_text, L"failme.bin"));
    CHECK(recorder.vec_calls.empty());
  }
}

TEST_CASE("run_pipeline：-v 汇总与逐文件行同出，占用项带升级释放记录") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  TestFileLock lock_file{
      lock_test_file(ntfscompact_test::temp_long_api_path(
          ntfscompact_test::temp_sub(tmp_dir, L"plain.bin")))};
  REQUIRE(lock_file);

  Options opt_case{compress_options()};
  opt_case.b_verbose = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  ReleaseRecorder rec_release{make_release_recorder()};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, make_record_releaser(rec_release), DirFlagFn{},
                                f_out, f_out)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  CHECK(count_lines(str_text) == 2);
  CHECK(contains(str_text, L"plain.bin：现状 未压缩，已压缩为指定算法 xpress8k（8192 → "));
  CHECK_FALSE(contains(str_text, L"→ 0 字节）"));
  CHECK(contains(str_text, L"；压缩/重压 8192 → 8192 字节，节省 0%"));
}

TEST_CASE("run_pipeline：占用经升级释放后执行，默认输出级与被关进程") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  TestFileLock lock_file{
      lock_test_file(ntfscompact_test::temp_long_api_path(
          ntfscompact_test::temp_sub(tmp_dir, L"plain.bin")))};
  REQUIRE(lock_file);

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  ReleaseRecorder rec_release{make_release_recorder()};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, make_record_releaser(rec_release), DirFlagFn{},
                                f_out, f_out)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  REQUIRE(rec_release.vec_calls.size() == 1);
  CHECK(rec_release.vec_calls[0].starts_with(L"\\\\?\\"));
  CHECK(contains(rec_release.vec_calls[0], L"plain.bin"));
  REQUIRE(recorder.vec_calls.size() == 1);
  CHECK(contains(str_text, L"plain.bin：已压缩为指定算法 xpress8k"));
  CHECK(contains(str_text, L"；升级释放：RM 强制关（hold_app.exe PID 4242）"));
  CHECK(contains(str_text, L"共 1 个文件：压缩 1、重压 0、解压 0、跳过 0"));
}

TEST_CASE("run_pipeline：非 -v 模式同样默认输出升级释放审计") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  TestFileLock lock_file{
      lock_test_file(ntfscompact_test::temp_long_api_path(
          ntfscompact_test::temp_sub(tmp_dir, L"plain.bin")))};
  REQUIRE(lock_file);

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  ReleaseRecorder rec_release{make_release_recorder()};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, make_record_releaser(rec_release), DirFlagFn{},
                                f_out, f_out)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  CHECK(rec_release.vec_calls.size() == 1);
  CHECK(contains(str_text, L"plain.bin：已压缩为指定算法 xpress8k"));
  CHECK(contains(str_text, L"；升级释放：RM 强制关（hold_app.exe PID 4242）"));
  CHECK_FALSE(contains(str_text, L"现状 "));
}

TEST_CASE("run_pipeline：占用不可释放计跳过不计失败") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"other.bin", 8192));
  TestFileLock lock_file{
      lock_test_file(ntfscompact_test::temp_long_api_path(
          ntfscompact_test::temp_sub(tmp_dir, L"plain.bin")))};
  REQUIRE(lock_file);

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::vector<std::wstring> vec_release_calls{};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, make_busy_releaser(vec_release_calls), DirFlagFn{},
                                f_out, f_out)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  REQUIRE(vec_release_calls.size() == 1);
  CHECK(contains(vec_release_calls[0], L"plain.bin"));
  CHECK(recorder.vec_calls.size() == 1);
  CHECK(contains(str_text, L"[跳过] "));
  CHECK(contains(str_text, L"plain.bin：占用不可释放：升级链尽后仍被占用（模拟）"));
  CHECK(contains(str_text, L"共 2 个文件：压缩 1、重压 0、解压 0、跳过 1"));
}

TEST_CASE("run_pipeline：执行器报 32 时先升级释放再重试一次") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ReleaseRecorder rec_release{make_release_recorder()};
  int i4_execute_calls{0};
  const ExecuteFn fn_execute{
      [&i4_execute_calls](const std::wstring&, Plan::Action, const CurrentState&, Algorithm) {
        ExecuteResult er_result{};
        if (i4_execute_calls == 0) {
          er_result.e_outcome = ExecuteResult::Outcome::kError;
          er_result.str_message = L"无法打开文件（错误码 32）（模拟）";
          er_result.u4_error = 32;
        }
        ++i4_execute_calls;
        return er_result;
      }};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, fn_execute, UncompressFn{},
                                make_record_releaser(rec_release), DirFlagFn{}, f_out, f_out)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  CHECK(i4_execute_calls == 2);
  REQUIRE(rec_release.vec_calls.size() == 1);
  CHECK(contains(str_text, L"plain.bin：已压缩为指定算法 xpress8k"));
}

TEST_CASE("run_pipeline：执行器报 32 且释放失败计跳过") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  std::vector<std::wstring> vec_release_calls{};
  const ExecuteFn fn_execute{[](const std::wstring&, Plan::Action, const CurrentState&, Algorithm) {
    ExecuteResult er_result{};
    er_result.e_outcome = ExecuteResult::Outcome::kError;
    er_result.str_message = L"无法打开文件（错误码 32）（模拟）";
    er_result.u4_error = 32;
    return er_result;
  }};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, fn_execute, UncompressFn{},
                                make_busy_releaser(vec_release_calls), DirFlagFn{}, f_out, f_out)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  CHECK(vec_release_calls.size() == 1);
  CHECK(contains(str_text, L"[跳过] "));
  CHECK(contains(str_text, L"plain.bin：占用不可释放"));
}

TEST_CASE("run_pipeline：释放后重试执行仍报 32 计跳过") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ReleaseRecorder rec_release{make_release_recorder()};
  int i4_execute_calls{0};
  const ExecuteFn fn_execute{
      [&i4_execute_calls](const std::wstring&, Plan::Action, const CurrentState&, Algorithm) {
        ExecuteResult er_result{};
        er_result.e_outcome = ExecuteResult::Outcome::kError;
        er_result.str_message = L"无法打开文件（错误码 32）（模拟）";
        er_result.u4_error = 32;
        ++i4_execute_calls;
        return er_result;
      }};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, fn_execute, UncompressFn{},
                                make_record_releaser(rec_release), DirFlagFn{}, f_out, f_out)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  CHECK(i4_execute_calls == 2);
  CHECK(rec_release.vec_calls.size() == 1);
  CHECK(contains(str_text, L"[跳过] "));
  CHECK(contains(str_text, L"plain.bin：占用不可释放"));
}

TEST_CASE("run_pipeline：非 32 错误不触发升级释放重试") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ReleaseRecorder rec_release{make_release_recorder()};
  int i4_execute_calls{0};
  const ExecuteFn fn_execute{
      [&i4_execute_calls](const std::wstring&, Plan::Action, const CurrentState&, Algorithm) {
        ExecuteResult er_result{};
        er_result.e_outcome = ExecuteResult::Outcome::kError;
        er_result.str_message = L"压缩失败（错误码 5）（模拟）";
        er_result.u4_error = 5;
        ++i4_execute_calls;
        return er_result;
      }};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, fn_execute, UncompressFn{},
                                make_record_releaser(rec_release), DirFlagFn{}, f_out, f_err)};
  std::fclose(f_out);
  std::fclose(f_err);

  CHECK(i4_code == 1);
  CHECK(i4_execute_calls == 1);
  CHECK(rec_release.vec_calls.empty());
}

TEST_CASE("run_pipeline：释放器未接线时保持既有行为（占用不拦截）") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"plain.bin", 8192));
  TestFileLock lock_file{
      lock_test_file(ntfscompact_test::temp_long_api_path(
          ntfscompact_test::temp_sub(tmp_dir, L"plain.bin")))};
  REQUIRE(lock_file);

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, DirFlagFn{}, f_out, f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  REQUIRE(recorder.vec_calls.size() == 1);
}

TEST_CASE("run_pipeline：-u 占用不可释放计跳过不调解压器") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"woflzx.bin", 8192));
  TestFileLock lock_file{
      lock_test_file(ntfscompact_test::temp_long_api_path(
          ntfscompact_test::temp_sub(tmp_dir, L"woflzx.bin")))};
  REQUIRE(lock_file);

  Options opt_case{compress_options()};
  opt_case.b_uncompress = true;
  opt_case.b_verbose = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  std::vector<std::wstring> vec_uncompress_calls{};
  const UncompressFn fn_uncompress{
      [&vec_uncompress_calls](const std::wstring& str_long_path)
          -> std::expected<void, std::wstring> {
        vec_uncompress_calls.push_back(str_long_path);
        return {};
      }};
  std::vector<std::wstring> vec_release_calls{};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, ExecuteFn{}, fn_uncompress,
                                make_busy_releaser(vec_release_calls), DirFlagFn{}, f_out, f_out)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  REQUIRE(vec_release_calls.size() == 1);
  CHECK(vec_uncompress_calls.empty());
  CHECK(contains(str_text, L"[跳过] "));
  CHECK(contains(str_text, L"woflzx.bin：占用不可释放"));
}

TEST_CASE("run_pipeline：-u 占用经升级释放后解压") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"woflzx.bin", 8192));
  TestFileLock lock_file{
      lock_test_file(ntfscompact_test::temp_long_api_path(
          ntfscompact_test::temp_sub(tmp_dir, L"woflzx.bin")))};
  REQUIRE(lock_file);

  Options opt_case{compress_options()};
  opt_case.b_uncompress = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  std::vector<std::wstring> vec_uncompress_calls{};
  const UncompressFn fn_uncompress{
      [&vec_uncompress_calls](const std::wstring& str_long_path)
          -> std::expected<void, std::wstring> {
        vec_uncompress_calls.push_back(str_long_path);
        return {};
      }};
  ReleaseRecorder rec_release{make_release_recorder()};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, ExecuteFn{}, fn_uncompress,
                                make_record_releaser(rec_release), DirFlagFn{}, f_out, f_out)};
  std::fclose(f_out);

  CHECK(i4_code == 0);
  REQUIRE(vec_uncompress_calls.size() == 1);
  REQUIRE(rec_release.vec_calls.size() == 1);
}

TEST_CASE("run_pipeline：实跑清除目录标记先于逐文件处理") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"one"));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"two"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"one\\plain.bin", 8192));
  REQUIRE(ntfscompact_test::mark_dir(ntfscompact_test::temp_sub(tmp_dir, L"one")));
  REQUIRE(ntfscompact_test::mark_dir(ntfscompact_test::temp_sub(tmp_dir, L"two")));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  std::vector<std::wstring> vec_events{};
  const ExecuteFn fn_execute{
      [&vec_events](const std::wstring& str_long_path, Plan::Action,
                    const CurrentState&, Algorithm) {
        vec_events.push_back(L"exec:" + str_long_path);
        return ExecuteResult{};
      }};
  const DirFlagFn fn_dir_flag{
      [&vec_events](const std::wstring& str_long_path)
          -> std::expected<void, std::wstring> {
        vec_events.push_back(L"clear:" + str_long_path);
        return {};
      }};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, fn_execute, UncompressFn{},
                                ReleaseFn{}, fn_dir_flag, f_out, f_err)};
  const std::wstring str_text{read_all(f_out)};
  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);

  CHECK(i4_code == 0);
  CHECK(str_err_text.empty());
  REQUIRE(vec_events.size() == 3);
  CHECK(vec_events[0].rfind(L"clear:", 0) == 0);
  CHECK(vec_events[1].rfind(L"clear:", 0) == 0);
  CHECK(vec_events[2].rfind(L"exec:", 0) == 0);
  CHECK(contains(str_text,
                 ntfscompact_test::temp_sub(tmp_dir, L"one") + L"：已清除标记"));
  CHECK(contains(str_text,
                 ntfscompact_test::temp_sub(tmp_dir, L"two") + L"：已清除标记"));
  CHECK(contains(str_text, L"[目录标记] "));
  CHECK(contains(str_text, L"；已清除目录标记 2 个"));
}

TEST_CASE("run_pipeline：目录标记清除失败计错误进 stderr 且退出码 1，其余照常") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"bad"));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"good"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"bad\\plain.bin", 8192));
  REQUIRE(ntfscompact_test::mark_dir(ntfscompact_test::temp_sub(tmp_dir, L"bad")));
  REQUIRE(ntfscompact_test::mark_dir(ntfscompact_test::temp_sub(tmp_dir, L"good")));

  Options opt_case{compress_options()};
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  const DirFlagFn fn_dir_flag{
      [](const std::wstring& str_long_path) -> std::expected<void, std::wstring> {
        if (str_long_path.find(L"bad") != std::wstring::npos) {
          return std::unexpected(L"无法打开目录（错误码 5）（模拟）");
        }
        return {};
      }};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, fn_dir_flag, f_out, f_err)};
  const std::wstring str_text{read_all(f_out)};
  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);

  CHECK(i4_code == 1);
  CHECK(contains(str_err_text,
                 ntfscompact_test::temp_sub(tmp_dir, L"bad") +
                     L"：无法打开目录（错误码 5）（模拟）"));
  CHECK(contains(str_err_text, L"[失败] "));
  CHECK(contains(str_text,
                 ntfscompact_test::temp_sub(tmp_dir, L"good") + L"：已清除标记"));
  CHECK(contains(str_text, L"plain.bin：已压缩为指定算法 xpress8k"));
  CHECK(contains(str_text, L"；失败 1"));
  CHECK(contains(str_text, L"；已清除目录标记 1 个"));
  REQUIRE(recorder.vec_calls.size() == 1);
}

TEST_CASE("run_pipeline：干跑只列目录标记，不清除不执行") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"one"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"one\\plain.bin", 8192));
  const std::wstring str_marked{ntfscompact_test::temp_sub(tmp_dir, L"one")};
  REQUIRE(ntfscompact_test::mark_dir(str_marked));

  Options opt_case{compress_options()};
  opt_case.b_dry_run = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  std::vector<std::wstring> vec_dir_calls{};
  const DirFlagFn fn_dir_flag{
      [&vec_dir_calls](const std::wstring& str_long_path)
          -> std::expected<void, std::wstring> {
        vec_dir_calls.push_back(str_long_path);
        return {};
      }};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, fn_dir_flag, f_out, f_err)};
  const std::wstring str_text{read_all(f_out)};
  std::fclose(f_out);
  std::fclose(f_err);

  CHECK(i4_code == 0);
  CHECK(vec_dir_calls.empty());
  CHECK(recorder.vec_calls.empty());
  CHECK(contains(str_text, str_marked + L"：将清除标记"));
  CHECK(contains(str_text, L"；将清除目录标记 1 个"));
  CHECK(ntfscompact_test::dir_is_marked(str_marked));
}

TEST_CASE("run_pipeline：-q 目录行受抑，汇总计数仍在") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"one"));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"two"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"one\\plain.bin", 8192));
  REQUIRE(ntfscompact_test::mark_dir(ntfscompact_test::temp_sub(tmp_dir, L"one")));
  REQUIRE(ntfscompact_test::mark_dir(ntfscompact_test::temp_sub(tmp_dir, L"two")));

  Options opt_case{compress_options()};
  opt_case.b_quiet = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  ExecuteRecorder recorder{};
  const DirFlagFn fn_dir_flag{[](const std::wstring&) -> std::expected<void, std::wstring> {
    return {};
  }};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, make_record_executor(recorder),
                                UncompressFn{}, ReleaseFn{}, fn_dir_flag, f_out, f_err)};
  const std::wstring str_text{read_all(f_out)};
  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);

  CHECK(i4_code == 0);
  CHECK(str_err_text.empty());
  CHECK(count_lines(str_text) == 1);
  CHECK(contains(str_text, L"；已清除目录标记 2 个"));
  CHECK_FALSE(contains(str_text, L"["));
  CHECK(recorder.vec_calls.size() == 1);
}

TEST_CASE("run_pipeline：解压模式同样清除目录标记") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_dir, L"one"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp_dir, L"one\\woflzx.bin", 8192));
  REQUIRE(ntfscompact_test::mark_dir(ntfscompact_test::temp_sub(tmp_dir, L"one")));

  Options opt_case{compress_options()};
  opt_case.b_uncompress = true;
  opt_case.vec_paths.push_back(ntfscompact_test::temp_path(tmp_dir));
  const DetectFn fn_detect{fake_detect};
  std::vector<std::wstring> vec_uncompress_calls{};
  const UncompressFn fn_uncompress{
      [&vec_uncompress_calls](const std::wstring& str_long_path)
          -> std::expected<void, std::wstring> {
        vec_uncompress_calls.push_back(str_long_path);
        return {};
      }};
  std::vector<std::wstring> vec_dir_calls{};
  const DirFlagFn fn_dir_flag{
      [&vec_dir_calls](const std::wstring& str_long_path)
          -> std::expected<void, std::wstring> {
        vec_dir_calls.push_back(str_long_path);
        return {};
      }};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i4_code{run_pipeline(opt_case, fn_detect, ExecuteFn{}, fn_uncompress,
                                ReleaseFn{}, fn_dir_flag, f_out, f_err)};
  const std::wstring str_text{read_all(f_out)};
  const std::wstring str_err_text{read_all(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);

  CHECK(i4_code == 0);
  CHECK(str_err_text.empty());
  REQUIRE(vec_dir_calls.size() == 1);
  CHECK(contains(vec_dir_calls[0], L"one"));
  CHECK(contains(str_text,
                 ntfscompact_test::temp_sub(tmp_dir, L"one") + L"：已清除标记"));
  CHECK(contains(str_text, L"；已清除目录标记 1 个"));
  REQUIRE(vec_uncompress_calls.size() == 1);
  CHECK(contains(str_text, L"woflzx.bin：已解压"));
}
