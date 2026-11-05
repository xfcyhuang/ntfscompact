#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "executor.hpp"
#include "temp_dir.hpp"

#include <cstdlib>
#include <windows.h>

namespace {

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::DetectionResult;
using ntfscompact::ExecuteFn;
using ntfscompact::ExecuteResult;
using ntfscompact::Plan;
using ntfscompact::DetectFn;
using ntfscompact::make_real_detector;
using ntfscompact::check_compression_meets_target;
using ntfscompact::make_real_executor;

std::string narrow(const std::wstring& str_wide) {
  const int i4_bytes{WideCharToMultiByte(CP_UTF8, 0, str_wide.data(),
                                         static_cast<int>(str_wide.size()),
                                         nullptr, 0, nullptr, nullptr)};
  std::string str_utf8(static_cast<size_t>(i4_bytes), '\0');
  if (i4_bytes > 0) {
    WideCharToMultiByte(CP_UTF8, 0, str_wide.data(),
                        static_cast<int>(str_wide.size()), str_utf8.data(),
                        i4_bytes, nullptr, nullptr);
  }
  return str_utf8;
}

int run_compact(const std::wstring& str_args) {
  const std::wstring str_cmd{L"compact " + str_args + L" >nul 2>&1"};
  return _wsystem(str_cmd.c_str());
}

bool write_compressible(const std::wstring& str_path) {
  HANDLE h_file{CreateFileW(ntfscompact_test::temp_long_api_path(str_path).c_str(),
                            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  static const char kSentence[]{"The quick brown fox jumps over the lazy dog. "};
  static const char kChunk[4096]{};
  bool b_ok{true};
  for (int i4_block{0}; b_ok && i4_block < 4; ++i4_block) {
    DWORD u4_written{};
    b_ok = ::WriteFile(h_file, kChunk, sizeof(kChunk), &u4_written, nullptr) &&
           u4_written == sizeof(kChunk);
  }
  for (int i4_line{0}; b_ok && i4_line < 4000; ++i4_line) {
    DWORD u4_written{};
    b_ok = ::WriteFile(h_file, kSentence, sizeof(kSentence) - 1, &u4_written,
                       nullptr) &&
           u4_written == sizeof(kSentence) - 1;
  }
  CloseHandle(h_file);
  return b_ok;
}

bool write_incompressible(const std::wstring& str_path) {
  HANDLE h_file{CreateFileW(ntfscompact_test::temp_long_api_path(str_path).c_str(),
                            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  std::array<unsigned char, 65536> buf_bytes{};
  unsigned long long u8_state{0x9e3779b97f4a7c15ULL};
  for (auto& ref_byte : buf_bytes) {
    u8_state ^= u8_state << 13;
    u8_state ^= u8_state >> 7;
    u8_state ^= u8_state << 17;
    ref_byte = static_cast<unsigned char>(u8_state);
  }
  bool b_ok{true};
  for (int i4_block{0}; b_ok && i4_block < 8; ++i4_block) {
    DWORD u4_written{};
    b_ok = ::WriteFile(h_file, buf_bytes.data(),
                       static_cast<DWORD>(buf_bytes.size()), &u4_written,
                       nullptr) &&
           u4_written == buf_bytes.size();
  }
  CloseHandle(h_file);
  return b_ok;
}

DWORD get_attrs(const std::wstring& str_path) {
  return GetFileAttributesW(
      ntfscompact_test::temp_long_api_path(str_path).c_str());
}

bool is_wof_form(const std::wstring& str_path) {
  const DWORD u4_attrs{get_attrs(str_path)};
  return (u4_attrs & FILE_ATTRIBUTE_SPARSE_FILE) != 0 &&
         (u4_attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool make_plain(const std::wstring& str_path) {
  if (!write_compressible(str_path)) {
    return false;
  }
  if (run_compact(L"/u \"" + str_path + L"\"") != 0) {
    return false;
  }
  return (get_attrs(str_path) & FILE_ATTRIBUTE_COMPRESSED) == 0;
}

bool has(const std::string& str_haystack, const char* ptr_needle) {
  return str_haystack.find(ptr_needle) != std::string::npos;
}

}

TEST_CASE("check_compression_meets_target：达到指定算法返回空串") {
  CHECK(narrow(check_compression_meets_target(
            true, CurrentState::kProviderFile, Algorithm::kXpress8k,
            Algorithm::kXpress8k))
            .empty());
  CHECK(narrow(check_compression_meets_target(
            true, CurrentState::kProviderFile, Algorithm::kLzx, Algorithm::kLzx))
            .empty());
}

TEST_CASE("check_compression_meets_target：静默未压缩报不符") {
  const std::wstring str_msg{check_compression_meets_target(
      false, 0, Algorithm::kXpress4k, Algorithm::kXpress8k)};
  CHECK(has(narrow(str_msg), "复核失败"));
}

TEST_CASE("check_compression_meets_target：provider 非 WOF FILE 报不符") {
  const std::wstring str_msg{check_compression_meets_target(
      true, CurrentState::kProviderWim, Algorithm::kXpress8k, Algorithm::kXpress8k)};
  CHECK(has(narrow(str_msg), "复核失败"));
}

TEST_CASE("check_compression_meets_target：算法不符报双方算法名") {
  const std::wstring str_msg{check_compression_meets_target(
      true, CurrentState::kProviderFile, Algorithm::kLzx, Algorithm::kXpress8k)};
  const std::string str_narrow{narrow(str_msg)};
  CHECK(has(str_narrow, "复核失败"));
  CHECK(has(str_narrow, "lzx"));
  CHECK(has(str_narrow, "xpress8k"));
}

TEST_CASE("make_real_executor：wofutil.dll 动态加载成功") {
  auto xe_executor{make_real_executor()};
  REQUIRE(xe_executor.has_value());
  CHECK(xe_executor.value());
}

TEST_CASE("普通文件压缩后 WofIsExternalFile 报告指定算法") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(make_plain(ntfscompact_test::temp_sub(tmp_dir, L"plain.bin")));

  auto xe_executor{make_real_executor()};
  REQUIRE(xe_executor.has_value());
  ExecuteFn fn_execute{std::move(xe_executor.value())};
  CurrentState cs_plain{};
  const ExecuteResult er_result{fn_execute(
      ntfscompact_test::temp_sub(tmp_dir, L"plain.bin"), Plan::Action::kCompress, cs_plain,
      Algorithm::kXpress8k)};
  CHECK(er_result.e_outcome == ExecuteResult::Outcome::kSuccess);
  CHECK(narrow(er_result.str_message).empty());

  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  const DetectionResult dr_verified{fn_detect(ntfscompact_test::temp_sub(tmp_dir, L"plain.bin"))};
  CHECK(narrow(dr_verified.str_error).empty());
  CHECK(dr_verified.cs_state.e_kind == CurrentState::Kind::kWof);
  CHECK(dr_verified.cs_state.i4_provider == CurrentState::kProviderFile);
  CHECK(dr_verified.cs_state.e_algorithm == Algorithm::kXpress8k);
}

TEST_CASE("指定算法 lzx 时压缩终态为 lzx") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(make_plain(ntfscompact_test::temp_sub(tmp_dir, L"lzx_target.bin")));

  auto xe_executor{make_real_executor()};
  REQUIRE(xe_executor.has_value());
  ExecuteFn fn_execute{std::move(xe_executor.value())};
  CurrentState cs_plain{};
  const ExecuteResult er_result{fn_execute(ntfscompact_test::temp_sub(tmp_dir, L"lzx_target.bin"),
                                            Plan::Action::kCompress, cs_plain,
                                            Algorithm::kLzx)};
  CHECK(er_result.e_outcome == ExecuteResult::Outcome::kSuccess);

  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  const DetectionResult dr_verified{
      fn_detect(ntfscompact_test::temp_sub(tmp_dir, L"lzx_target.bin"))};
  CHECK(dr_verified.cs_state.e_kind == CurrentState::Kind::kWof);
  CHECK(dr_verified.cs_state.e_algorithm == Algorithm::kLzx);
}

TEST_CASE("LZNT1 文件重压后终态为指定算法") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(make_plain(ntfscompact_test::temp_sub(tmp_dir, L"lznt1.bin")));
  REQUIRE(run_compact(L"/c \"" + ntfscompact_test::temp_sub(tmp_dir, L"lznt1.bin") + L"\"") == 0);
  REQUIRE((get_attrs(ntfscompact_test::temp_sub(tmp_dir, L"lznt1.bin")) & FILE_ATTRIBUTE_COMPRESSED) != 0);

  auto xe_executor{make_real_executor()};
  REQUIRE(xe_executor.has_value());
  ExecuteFn fn_execute{std::move(xe_executor.value())};
  CurrentState cs_lznt1{};
  cs_lznt1.e_kind = CurrentState::Kind::kLznt1;
  const ExecuteResult er_result{fn_execute(
      ntfscompact_test::temp_sub(tmp_dir, L"lznt1.bin"), Plan::Action::kRecompress, cs_lznt1,
      Algorithm::kXpress8k)};
  CHECK_MESSAGE(er_result.e_outcome == ExecuteResult::Outcome::kSuccess,
                narrow(er_result.str_message));

  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  const DetectionResult dr_verified{fn_detect(ntfscompact_test::temp_sub(tmp_dir, L"lznt1.bin"))};
  CHECK(dr_verified.cs_state.e_kind == CurrentState::Kind::kWof);
  CHECK(dr_verified.cs_state.i4_provider == CurrentState::kProviderFile);
  CHECK(dr_verified.cs_state.e_algorithm == Algorithm::kXpress8k);
}

TEST_CASE("其他 WOF 算法文件重压后终态为指定算法") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(make_plain(ntfscompact_test::temp_sub(tmp_dir, L"woflzx.bin")));

  auto xe_executor{make_real_executor()};
  REQUIRE(xe_executor.has_value());
  ExecuteFn fn_execute{std::move(xe_executor.value())};
  CurrentState cs_plain{};
  const ExecuteResult er_setup{fn_execute(ntfscompact_test::temp_sub(tmp_dir, L"woflzx.bin"),
                                           Plan::Action::kCompress, cs_plain,
                                           Algorithm::kLzx)};
  REQUIRE_MESSAGE(er_setup.e_outcome == ExecuteResult::Outcome::kSuccess,
                  narrow(er_setup.str_message));

  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  const DetectionResult dr_setup{
      fn_detect(ntfscompact_test::temp_sub(tmp_dir, L"woflzx.bin"))};
  REQUIRE(dr_setup.cs_state.e_kind == CurrentState::Kind::kWof);
  REQUIRE(dr_setup.cs_state.e_algorithm == Algorithm::kLzx);

  CurrentState cs_wof{};
  cs_wof.e_kind = CurrentState::Kind::kWof;
  cs_wof.i4_provider = CurrentState::kProviderFile;
  cs_wof.e_algorithm = Algorithm::kLzx;
  const ExecuteResult er_result{fn_execute(
      ntfscompact_test::temp_sub(tmp_dir, L"woflzx.bin"), Plan::Action::kRecompress, cs_wof,
      Algorithm::kXpress8k)};
  CHECK_MESSAGE(er_result.e_outcome == ExecuteResult::Outcome::kSuccess,
                narrow(er_result.str_message));

  const DetectionResult dr_verified{
      fn_detect(ntfscompact_test::temp_sub(tmp_dir, L"woflzx.bin"))};
  CHECK(dr_verified.cs_state.e_kind == CurrentState::Kind::kWof);
  CHECK(dr_verified.cs_state.e_algorithm == Algorithm::kXpress8k);
}

TEST_CASE("无收益文件计跳过不计错误") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(make_plain(ntfscompact_test::temp_sub(tmp_dir, L"random.bin")));
  REQUIRE(write_incompressible(ntfscompact_test::temp_sub(tmp_dir, L"random.bin")));

  auto xe_executor{make_real_executor()};
  REQUIRE(xe_executor.has_value());
  ExecuteFn fn_execute{std::move(xe_executor.value())};
  CurrentState cs_plain{};
  const ExecuteResult er_result{fn_execute(
      ntfscompact_test::temp_sub(tmp_dir, L"random.bin"), Plan::Action::kCompress, cs_plain,
      Algorithm::kXpress8k)};
  if (er_result.e_outcome == ExecuteResult::Outcome::kSkipped) {
    CHECK(has(narrow(er_result.str_message), "无收益"));
    CHECK_FALSE(is_wof_form(ntfscompact_test::temp_sub(tmp_dir, L"random.bin")));
  } else if (er_result.e_outcome == ExecuteResult::Outcome::kSuccess) {
    MESSAGE("内核对该数据仍判定有收益，按压缩成功且复核通过计");
  } else {
    FAIL("无收益文件不应计错误：", narrow(er_result.str_message));
  }
}

TEST_CASE("只读属性文件压缩成功且属性恢复") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(make_plain(ntfscompact_test::temp_sub(tmp_dir, L"readonly.bin")));
  REQUIRE(SetFileAttributesW(
              ntfscompact_test::temp_long_api_path(ntfscompact_test::temp_sub(tmp_dir, L"readonly.bin"))
                  .c_str(),
              FILE_ATTRIBUTE_READONLY));
  REQUIRE((get_attrs(ntfscompact_test::temp_sub(tmp_dir, L"readonly.bin")) & FILE_ATTRIBUTE_READONLY) != 0);

  auto xe_executor{make_real_executor()};
  REQUIRE(xe_executor.has_value());
  ExecuteFn fn_execute{std::move(xe_executor.value())};
  CurrentState cs_plain{};
  const ExecuteResult er_result{fn_execute(
      ntfscompact_test::temp_sub(tmp_dir, L"readonly.bin"), Plan::Action::kCompress, cs_plain,
      Algorithm::kXpress8k)};
  CHECK(er_result.e_outcome == ExecuteResult::Outcome::kSuccess);
  CHECK((get_attrs(ntfscompact_test::temp_sub(tmp_dir, L"readonly.bin")) & FILE_ATTRIBUTE_READONLY) != 0);

  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  const DetectionResult dr_verified{
      fn_detect(ntfscompact_test::temp_sub(tmp_dir, L"readonly.bin"))};
  CHECK(dr_verified.cs_state.e_kind == CurrentState::Kind::kWof);
  CHECK(dr_verified.cs_state.e_algorithm == Algorithm::kXpress8k);
}

TEST_CASE("被独占打开的文件计错误") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  REQUIRE(make_plain(ntfscompact_test::temp_sub(tmp_dir, L"locked.bin")));
  HANDLE h_lock{CreateFileW(
      ntfscompact_test::temp_long_api_path(ntfscompact_test::temp_sub(tmp_dir, L"locked.bin")).c_str(),
      GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
  REQUIRE(h_lock != INVALID_HANDLE_VALUE);

  auto xe_executor{make_real_executor()};
  REQUIRE(xe_executor.has_value());
  ExecuteFn fn_execute{std::move(xe_executor.value())};
  CurrentState cs_plain{};
  const ExecuteResult er_result{fn_execute(
      ntfscompact_test::temp_sub(tmp_dir, L"locked.bin"), Plan::Action::kCompress, cs_plain,
      Algorithm::kXpress8k)};
  CloseHandle(h_lock);
  CHECK(er_result.e_outcome == ExecuteResult::Outcome::kError);
  CHECK_FALSE(narrow(er_result.str_message).empty());
}

TEST_CASE("Uncompress 动作为未实现桩计错误") {
  auto xe_executor{make_real_executor()};
  REQUIRE(xe_executor.has_value());
  ExecuteFn fn_execute{std::move(xe_executor.value())};
  CurrentState cs_wof{};
  cs_wof.e_kind = CurrentState::Kind::kWof;
  const ExecuteResult er_result{fn_execute(L"\\\\?\\D:\\no-such-t3.bin",
                                            Plan::Action::kUncompress, cs_wof,
                                            Algorithm::kXpress8k)};
  CHECK(er_result.e_outcome == ExecuteResult::Outcome::kError);
  CHECK(has(narrow(er_result.str_message), "未实现"));
}
