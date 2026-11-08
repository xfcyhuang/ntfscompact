#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "detect.hpp"
#include "temp_dir.hpp"
#include "util.hpp"

#include <windows.h>

namespace {

using ntfscompact_test::narrow;
using ntfscompact_test::run_compact;

bool write_compressible(const std::wstring& str_path) {
  HANDLE h_file{
      CreateFileW(ntfscompact_test::temp_long_api_path(str_path).c_str(),
                  GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  static const char kSentence[]{"The quick brown fox jumps over the lazy dog. "};
  static const char kChunk[4096]{};
  for (int i4_block{0}; i4_block < 4; ++i4_block) {
    DWORD u4_written{};
    if (!::WriteFile(h_file, kChunk, sizeof(kChunk), &u4_written, nullptr)) {
      CloseHandle(h_file);
      return false;
    }
  }
  for (int i4_line{0}; i4_line < 4000; ++i4_line) {
    DWORD u4_written{};
    if (!::WriteFile(h_file, kSentence, sizeof(kSentence) - 1, &u4_written,
                     nullptr)) {
      CloseHandle(h_file);
      return false;
    }
  }
  CloseHandle(h_file);
  return true;
}

DWORD get_attrs(const std::wstring& str_path) {
  return GetFileAttributesW(ntfscompact_test::temp_long_api_path(str_path).c_str());
}

}

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::DetectionResult;
using ntfscompact::DetectFn;
using ntfscompact::make_real_detector;

TEST_CASE("make_real_detector：wofutil.dll 动态加载成功") {
  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
}

TEST_CASE("make_real_detector：wofutil 加载失败注入 → unexpected 上抛启动即败") {
  auto xe_detector{make_real_detector(
      std::unexpected(std::wstring{L"wofutil.dll 缺少导出 WofIsExternalFile"}))};
  REQUIRE_FALSE(xe_detector.has_value());
  const std::string str_error{narrow(xe_detector.error())};
  CHECK(str_error.find("wofutil.dll") != std::string::npos);
  CHECK(str_error.find("WOF 现状检测不可用") != std::string::npos);
  CHECK(str_error.find("工具终止") != std::string::npos);
}

TEST_CASE("make_real_detector：普通文件 → 未压缩（None）") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(write_compressible(ntfscompact_test::temp_sub(tmp, L"plain.bin")));
  REQUIRE(run_compact(L"/u \"" + ntfscompact_test::temp_sub(tmp, L"plain.bin") +
                      L"\"") == 0);
  REQUIRE((get_attrs(ntfscompact_test::temp_sub(tmp, L"plain.bin")) &
           FILE_ATTRIBUTE_COMPRESSED) == 0);

  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  const DetectionResult dr_result{
      fn_detect(ntfscompact_test::temp_sub(tmp, L"plain.bin"))};
  CHECK(narrow(dr_result.str_error) == "");
  CHECK(dr_result.cs_state.e_kind == CurrentState::Kind::kNone);
}

TEST_CASE("make_real_detector：compact /c 造的 LZNT1 文件 → Lznt1") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(write_compressible(ntfscompact_test::temp_sub(tmp, L"lznt1.bin")));
  REQUIRE(run_compact(L"/u \"" + ntfscompact_test::temp_sub(tmp, L"lznt1.bin") +
                      L"\"") == 0);
  REQUIRE(run_compact(L"/c \"" + ntfscompact_test::temp_sub(tmp, L"lznt1.bin") +
                      L"\"") == 0);

  const DWORD u4_attrs{get_attrs(ntfscompact_test::temp_sub(tmp, L"lznt1.bin"))};
  REQUIRE((u4_attrs & FILE_ATTRIBUTE_COMPRESSED) != 0);

  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  const DetectionResult dr_result{
      fn_detect(ntfscompact_test::temp_sub(tmp, L"lznt1.bin"))};
  CHECK(narrow(dr_result.str_error) == "");
  CHECK(dr_result.cs_state.e_kind == CurrentState::Kind::kLznt1);
}

TEST_CASE("make_real_detector：compact /exe:lzx 造的 WOF 文件 → Wof/lzx/ProviderFile") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(write_compressible(ntfscompact_test::temp_sub(tmp, L"wof.bin")));
  REQUIRE(run_compact(L"/u \"" + ntfscompact_test::temp_sub(tmp, L"wof.bin") +
                      L"\"") == 0);
  REQUIRE(run_compact(L"/exe:lzx /c \"" +
                      ntfscompact_test::temp_sub(tmp, L"wof.bin") + L"\"") == 0);

  const DWORD u4_attrs{get_attrs(ntfscompact_test::temp_sub(tmp, L"wof.bin"))};
  const bool b_wof_form{(u4_attrs & FILE_ATTRIBUTE_SPARSE_FILE) != 0 &&
                        (u4_attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0};
  if (!b_wof_form) {
    MESSAGE("compact /exe:lzx 静默未压缩（Win11 偶发，见研究笔记），跳过断言");
    return;
  }

  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  const DetectionResult dr_result{
      fn_detect(ntfscompact_test::temp_sub(tmp, L"wof.bin"))};
  CHECK(narrow(dr_result.str_error) == "");
  CHECK(dr_result.cs_state.e_kind == CurrentState::Kind::kWof);
  CHECK(dr_result.cs_state.i4_provider == CurrentState::kProviderFile);
  CHECK(dr_result.cs_state.e_algorithm == Algorithm::kLzx);
}

TEST_CASE("make_real_detector：不存在的路径 → 带错误的 DetectionResult") {
  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  const DetectionResult dr_result{
      fn_detect(L"\\\\?\\D:\\Temp\\no-such-file-t3.bin")};
  CHECK_FALSE(dr_result.str_error.empty());
}

TEST_CASE("make_real_detector：LZNT1 文件 compact /u 后 → None（状态可逆复核）") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(write_compressible(ntfscompact_test::temp_sub(tmp, L"roundtrip.bin")));
  REQUIRE(run_compact(L"/u \"" +
                      ntfscompact_test::temp_sub(tmp, L"roundtrip.bin") +
                      L"\"") == 0);
  REQUIRE(run_compact(L"/c \"" +
                      ntfscompact_test::temp_sub(tmp, L"roundtrip.bin") +
                      L"\"") == 0);

  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};
  CHECK(fn_detect(ntfscompact_test::temp_sub(tmp, L"roundtrip.bin"))
            .cs_state.e_kind == CurrentState::Kind::kLznt1);

  REQUIRE(run_compact(L"/u \"" +
                      ntfscompact_test::temp_sub(tmp, L"roundtrip.bin") +
                      L"\"") == 0);
  CHECK(fn_detect(ntfscompact_test::temp_sub(tmp, L"roundtrip.bin"))
            .cs_state.e_kind == CurrentState::Kind::kNone);
}
