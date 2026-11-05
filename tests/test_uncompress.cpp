import std;

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "detect.hpp"
#include "release.hpp"
#include "pipeline.hpp"
#include "temp_dir.hpp"
#include "util.hpp"

#include <windows.h>
#include <cstdlib>

namespace {

constexpr std::wstring_view kSentence{L"The quick brown fox jumps over the lazy dog. "};

using ntfscompact_test::narrow;
using ntfscompact_test::run_compact;

bool write_compressible(const std::wstring& str_path) {
  HANDLE h_file{CreateFileW(ntfscompact_test::temp_long_api_path(str_path).c_str(),
                            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool b_ok{true};
  const std::string str_zeros(16384, '\0');
  std::wstring str_text{};
  for (int i_sentence{0}; i_sentence < 4000; ++i_sentence) {
    str_text.append(kSentence);
  }
  const std::string str_body{std::string(str_text.begin(), str_text.end())};
  DWORD u4_written{};
  b_ok = WriteFile(h_file, str_zeros.data(), static_cast<DWORD>(str_zeros.size()),
                   &u4_written, nullptr) &&
         u4_written == str_zeros.size();
  b_ok = b_ok && WriteFile(h_file, str_body.data(), static_cast<DWORD>(str_body.size()),
                           &u4_written, nullptr) &&
         u4_written == str_body.size();
  CloseHandle(h_file);
  return b_ok;
}

std::vector<char> read_all_bytes(const std::wstring& str_path) {
  std::vector<char> vec_bytes{};
  HANDLE h_file{CreateFileW(ntfscompact_test::temp_long_api_path(str_path).c_str(),
                            GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return vec_bytes;
  }
  char vec_buf[65536]{};
  DWORD u4_read{};
  while (::ReadFile(h_file, vec_buf, sizeof(vec_buf), &u4_read, nullptr) && u4_read > 0) {
    vec_bytes.insert(vec_bytes.end(), vec_buf, vec_buf + u4_read);
  }
  CloseHandle(h_file);
  return vec_bytes;
}

ntfscompact::DetectionResult fake_detect(const std::wstring& str_long_path) {
    ntfscompact::DetectionResult st{};
    const std::wstring str_name{
        str_long_path.substr(str_long_path.find_last_of(L"\\") + 1)};
    if (str_name == L"failme.bin") {
      st.str_error = L"现状检测失败（模拟）";
    } else if (str_name == L"woflzx.bin") {
      st.cs_state.e_kind = ntfscompact::CurrentState::Kind::kWof;
      st.cs_state.e_algorithm = ntfscompact::Algorithm::kLzx;
      st.cs_state.i4_provider = ntfscompact::CurrentState::kProviderFile;
    } else if (str_name == L"lznt1.bin") {
      st.cs_state.e_kind = ntfscompact::CurrentState::Kind::kLznt1;
    }
    return st;
  }

std::wstring read_stream(std::FILE* f_stream) {
  std::rewind(f_stream);
  std::wstring str_content{};
  char vec_buf[4096]{};
  std::size_t n_read{};
  while ((n_read = std::fread(vec_buf, 1, sizeof(vec_buf), f_stream)) > 0) {
    const int i_wchars{MultiByteToWideChar(CP_UTF8, 0, vec_buf,
                                           static_cast<int>(n_read), nullptr, 0)};
    std::wstring str_chunk(static_cast<std::size_t>(i_wchars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, vec_buf, static_cast<int>(n_read), str_chunk.data(),
                        i_wchars);
    str_content += str_chunk;
  }
  return str_content;
}

bool text_contains(const std::wstring& str_haystack, const std::wstring& str_needle) {
  return str_haystack.find(str_needle) != std::wstring::npos;
}

bool calls_have(const std::vector<std::wstring>& vec_calls, const std::wstring& str_name) {
  for (const std::wstring& str_call : vec_calls) {
    if (str_call.find(str_name) != std::wstring::npos) {
      return true;
    }
  }
  return false;
}

ntfscompact::UncompressFn fake_uncompressor(std::vector<std::wstring>& vec_calls,
                                             const std::wstring& str_fail_name) {
  return [&vec_calls, str_fail_name](const std::wstring& str_long_path)
             -> std::expected<void, std::wstring> {
    vec_calls.push_back(str_long_path);
    if (!str_fail_name.empty() &&
        str_long_path.find(str_fail_name) != std::wstring::npos) {
      return std::unexpected(L"解除失败（模拟）");
    }
    return {};
  };
}

}

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::DetectionResult;
using ntfscompact::ExecuteFn;
using ntfscompact::ExecuteResult;
using ntfscompact::Options;
using ntfscompact::Plan;
using ntfscompact::DetectFn;
using ntfscompact::make_real_detector;
using ntfscompact::run_pipeline;
using ntfscompact::UncompressFn;
using ntfscompact::make_real_uncompressor;
using ntfscompact::uncompress_file;

TEST_CASE("make_real_uncompressor：wofutil.dll 动态加载成功") {
  const std::expected<UncompressFn, std::wstring> exp_fn{make_real_uncompressor()};
  REQUIRE(exp_fn.has_value());
}

TEST_CASE("uncompress_file：WOF 四算法解压后非 backing 且内容不变") {
  auto xe_detector{ntfscompact::make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const ntfscompact::DetectFn fn_detect{std::move(xe_detector).value()};

  const wchar_t* vec_algos[]{L"xpress4k", L"xpress8k", L"xpress16k", L"lzx"};
  for (const wchar_t* vec_algo : vec_algos) {
    ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
    REQUIRE(tmp->b_ok);
    const std::wstring str_file{ntfscompact_test::temp_sub(tmp, std::wstring(L"wof-") + vec_algo + L".bin")};
    REQUIRE(write_compressible(str_file));
    REQUIRE(run_compact(L"/u \"" + str_file + L"\"") == 0);
    if (run_compact(std::wstring(L"/exe:") + vec_algo + L" /c \"" + str_file + L"\"") != 0) {
      const std::string str_skip8{std::format("compact /exe 退出非零，跳过 {}", narrow(vec_algo))};
      MESSAGE(str_skip8);
      continue;
    }
    const DetectionResult st_before{fn_detect(str_file)};
    if (st_before.cs_state.e_kind != CurrentState::Kind::kWof) {
      const std::string str_silent8{"compact /exe 静默未压成 WOF（研究笔记已记录该偶发），跳过该算法断言"};
      MESSAGE(str_silent8);
      continue;
    }
    REQUIRE(narrow(st_before.str_error).empty());
    const std::vector<char> vec_before{read_all_bytes(str_file)};
    REQUIRE(!vec_before.empty());

    const std::expected<void, std::wstring> exp{uncompress_file(str_file, fn_detect)};
    CHECK(exp.has_value());
    if (!exp.has_value()) {
      const std::string str_err8{narrow(exp.error())};
      MESSAGE(str_err8.c_str());
    }

    const DetectionResult st_after{fn_detect(str_file)};
    REQUIRE(narrow(st_after.str_error).empty());
    CHECK(st_after.cs_state.e_kind == CurrentState::Kind::kNone);

    const std::vector<char> vec_after{read_all_bytes(str_file)};
    REQUIRE(vec_after.size() == vec_before.size());
    CHECK(std::equal(vec_after.begin(), vec_after.end(), vec_before.begin()));
  }
}

TEST_CASE("uncompress_file：LZNT1 文件解压成功且内容不变") {
  auto xe_detector{ntfscompact::make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const ntfscompact::DetectFn fn_detect{std::move(xe_detector).value()};

  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_sub(tmp, L"lznt1.bin")};
  REQUIRE(write_compressible(str_file));
  REQUIRE(run_compact(L"/u \"" + str_file + L"\"") == 0);
  REQUIRE(run_compact(L"/c \"" + str_file + L"\"") == 0);

  const DetectionResult st_before{fn_detect(str_file)};
  REQUIRE(narrow(st_before.str_error).empty());
  REQUIRE(st_before.cs_state.e_kind == CurrentState::Kind::kLznt1);
  const std::vector<char> vec_before{read_all_bytes(str_file)};
  REQUIRE(!vec_before.empty());

  const std::expected<void, std::wstring> exp{uncompress_file(str_file, fn_detect)};
  CHECK(exp.has_value());
  if (!exp.has_value()) {
    const std::string str_err8{narrow(exp.error())};
    MESSAGE(str_err8);
  }

  const DetectionResult st_after{fn_detect(str_file)};
  REQUIRE(narrow(st_after.str_error).empty());
  CHECK(st_after.cs_state.e_kind == CurrentState::Kind::kNone);

  const std::vector<char> vec_after{read_all_bytes(str_file)};
  REQUIRE(vec_after.size() == vec_before.size());
  CHECK(std::equal(vec_after.begin(), vec_after.end(), vec_before.begin()));
}

TEST_CASE("uncompress_file：只读属性文件解压成功且属性恢复") {
  auto xe_detector{ntfscompact::make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const ntfscompact::DetectFn fn_detect{std::move(xe_detector).value()};

  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_sub(tmp, L"readonly.bin")};
  REQUIRE(write_compressible(str_file));
  auto xe_executor{ntfscompact::make_real_executor()};
  REQUIRE(xe_executor.has_value());
  const ntfscompact::ExecuteFn fn_execute{std::move(xe_executor).value()};
  const ntfscompact::CurrentState cs_plain{};
  const ntfscompact::ExecuteResult er_setup{
      fn_execute(str_file, ntfscompact::Plan::Action::kCompress, cs_plain,
                 ntfscompact::Algorithm::kLzx)};
  REQUIRE_MESSAGE(er_setup.e_outcome == ntfscompact::ExecuteResult::Outcome::kSuccess,
                  narrow(er_setup.str_message));
  REQUIRE(fn_detect(str_file).cs_state.e_kind == CurrentState::Kind::kWof);
  REQUIRE(SetFileAttributesW(ntfscompact_test::temp_long_api_path(str_file).c_str(),
                             FILE_ATTRIBUTE_READONLY));

  const std::expected<void, std::wstring> exp{uncompress_file(str_file, fn_detect)};
  if (!exp.has_value()) {
    const std::string str_err8{narrow(exp.error())};
    MESSAGE(str_err8);
  }
  REQUIRE(exp.has_value());
  CHECK(fn_detect(str_file).cs_state.e_kind == CurrentState::Kind::kNone);
  CHECK((GetFileAttributesW(ntfscompact_test::temp_long_api_path(str_file).c_str()) &
         FILE_ATTRIBUTE_READONLY) != 0);
}

TEST_CASE("uncompress_file：未压缩文件为无操作成功（幂等）") {
  auto xe_detector{ntfscompact::make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const ntfscompact::DetectFn fn_detect{std::move(xe_detector).value()};

  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  const std::wstring str_file{ntfscompact_test::temp_sub(tmp, L"plain.bin")};
  REQUIRE(write_compressible(str_file));
  REQUIRE(run_compact(L"/u \"" + str_file + L"\"") == 0);
  REQUIRE(fn_detect(str_file).cs_state.e_kind == CurrentState::Kind::kNone);

  const std::expected<void, std::wstring> exp{uncompress_file(str_file, fn_detect)};
  CHECK(exp.has_value());
  CHECK(fn_detect(str_file).cs_state.e_kind == CurrentState::Kind::kNone);
}

TEST_CASE("uncompress_file：不存在的路径返回错误") {
  auto xe_detector{ntfscompact::make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const ntfscompact::DetectFn fn_detect{std::move(xe_detector).value()};
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  const std::expected<void, std::wstring> exp{
      uncompress_file(ntfscompact_test::temp_sub(tmp, L"no-such-t4.bin"), fn_detect)};
  CHECK_FALSE(exp.has_value());
  if (!exp.has_value()) {
    const std::string str_err8{narrow(exp.error())};
    MESSAGE(str_err8);
  }
}

TEST_CASE("run_pipeline：-u 实跑逐文件解压，未压缩与遍历期跳过项不执行") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"woflzx.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"lznt1.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"tiny.bin", 100));

  Options opt{};
  opt.b_uncompress = true;
  opt.vec_paths.push_back(ntfscompact_test::temp_path(tmp));
  const ntfscompact::DetectFn fn_detect{fake_detect};
  std::vector<std::wstring> vec_calls{};
  const UncompressFn fn{fake_uncompressor(vec_calls, L"")};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i_code{run_pipeline(opt, fn_detect, ExecuteFn{}, fn, ntfscompact::ReleaseFn{}, f_out, f_out)};
  const std::wstring str_text{read_stream(f_out)};
  std::fclose(f_out);

  CHECK(i_code == 0);
  REQUIRE(vec_calls.size() == 2);
  CHECK(calls_have(vec_calls, L"woflzx.bin"));
  CHECK(calls_have(vec_calls, L"lznt1.bin"));
  CHECK(vec_calls[0].starts_with(L"\\\\?\\"));
  CHECK(text_contains(str_text, L"[解压] "));
  CHECK(text_contains(str_text, L"woflzx.bin：已解压"));
  CHECK(text_contains(str_text, L"[跳过] "));
  CHECK(text_contains(str_text, L"共 4 个文件：压缩 0、重压 0、解压 2、跳过 2"));
}

TEST_CASE("run_pipeline：解压失败计入失败并继续跑完") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"woflzx.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"lznt1.bin", 8192));

  Options opt{};
  opt.b_uncompress = true;
  opt.b_verbose = true;
  opt.vec_paths.push_back(ntfscompact_test::temp_path(tmp));
  const ntfscompact::DetectFn fn_detect{fake_detect};
  std::vector<std::wstring> vec_calls{};
  const UncompressFn fn{fake_uncompressor(vec_calls, L"woflzx.bin")};

  std::FILE* f_out{std::tmpfile()};
  std::FILE* f_err{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  REQUIRE(f_err != nullptr);
  const int i_code{run_pipeline(opt, fn_detect, ExecuteFn{}, fn, ntfscompact::ReleaseFn{}, f_out, f_err)};
  const std::wstring str_out{read_stream(f_out)};
  const std::wstring str_err{read_stream(f_err)};
  std::fclose(f_out);
  std::fclose(f_err);

  CHECK(i_code == 1);
  REQUIRE(vec_calls.size() == 2);
  CHECK(calls_have(vec_calls, L"lznt1.bin"));
  CHECK(text_contains(str_err, L"woflzx.bin"));
  CHECK(text_contains(str_err, L"解除失败（模拟）"));
  CHECK(text_contains(str_out, L"解压 1、跳过 0"));
  CHECK(text_contains(str_out, L"失败 1"));
}

TEST_CASE("run_pipeline：-u -n 干跑只报告不执行") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"woflzx.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"lznt1.bin", 8192));

  Options opt{};
  opt.b_uncompress = true;
  opt.b_dry_run = true;
  opt.vec_paths.push_back(ntfscompact_test::temp_path(tmp));
  const ntfscompact::DetectFn fn_detect{fake_detect};
  std::vector<std::wstring> vec_calls{};
  const UncompressFn fn{fake_uncompressor(vec_calls, L"")};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i_code{run_pipeline(opt, fn_detect, ExecuteFn{}, fn, ntfscompact::ReleaseFn{}, f_out, f_out)};
  const std::wstring str_text{read_stream(f_out)};
  std::fclose(f_out);

  CHECK(i_code == 0);
  CHECK(vec_calls.empty());
  CHECK(text_contains(str_text, L"[解压] "));
  CHECK(text_contains(str_text, L"将解压"));
}

TEST_CASE("run_pipeline：压缩模式不调解压执行器") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"plain.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"woflzx.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"lznt1.bin", 8192));

  Options opt{};
  opt.vec_paths.push_back(ntfscompact_test::temp_path(tmp));
  const ntfscompact::DetectFn fn_detect{fake_detect};
  std::vector<std::wstring> vec_calls{};
  const UncompressFn fn{fake_uncompressor(vec_calls, L"")};
  const ExecuteFn fn_execute{
      [](const std::wstring&, Plan::Action, const CurrentState&, Algorithm) {
        return ExecuteResult{};
      }};

  std::FILE* f_out{std::tmpfile()};
  REQUIRE(f_out != nullptr);
  const int i_code{run_pipeline(opt, fn_detect, fn_execute, fn, ntfscompact::ReleaseFn{}, f_out, f_out)};
  std::fclose(f_out);

  CHECK(i_code == 0);
  CHECK(vec_calls.empty());
}

