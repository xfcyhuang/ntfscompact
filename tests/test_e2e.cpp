#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "detect.hpp"
#include "temp_dir.hpp"
#include "util.hpp"

#include <windows.h>
#include <cstdlib>

namespace {

std::wstring g_str_tool_path{};

using ntfscompact::Algorithm;
using ntfscompact::CurrentState;
using ntfscompact::DetectionResult;
using ntfscompact::DetectFn;
using ntfscompact::make_real_detector;
using ntfscompact_test::narrow;
using ntfscompact_test::run_compact;

bool has_text(const std::wstring& str_haystack, const std::wstring& str_needle) {
  return str_haystack.find(str_needle) != std::wstring::npos;
}

std::uint64_t write_compressible(const std::wstring& str_path) {
  HANDLE h_file{CreateFileW(ntfscompact_test::temp_long_api_path(str_path).c_str(),
                            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return 0;
  }
  static const char kSentence[]{"The quick brown fox jumps over the lazy dog. "};
  const std::string str_zeros(16384, '\0');
  bool b_ok{true};
  std::uint64_t u8_total{0};
  DWORD u4_written{0};
  b_ok = WriteFile(h_file, str_zeros.data(), static_cast<DWORD>(str_zeros.size()),
                   &u4_written, nullptr) &&
         u4_written == str_zeros.size();
  u8_total += u4_written;
  for (int i4_line{0}; b_ok && i4_line < 4000; ++i4_line) {
    b_ok = WriteFile(h_file, kSentence, sizeof(kSentence) - 1, &u4_written,
                     nullptr) &&
           u4_written == sizeof(kSentence) - 1;
    u8_total += u4_written;
  }
  CloseHandle(h_file);
  return b_ok ? u8_total : 0;
}

std::uint64_t write_random(const std::wstring& str_path) {
  HANDLE h_file{CreateFileW(ntfscompact_test::temp_long_api_path(str_path).c_str(),
                            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (h_file == INVALID_HANDLE_VALUE) {
    return 0;
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
  std::uint64_t u8_total{0};
  DWORD u4_written{0};
  for (int i4_block{0}; b_ok && i4_block < 8; ++i4_block) {
    b_ok = WriteFile(h_file, buf_bytes.data(),
                     static_cast<DWORD>(buf_bytes.size()), &u4_written, nullptr) &&
           u4_written == buf_bytes.size();
    u8_total += u4_written;
  }
  CloseHandle(h_file);
  return b_ok ? u8_total : 0;
}

std::uint64_t disk_usage(const std::wstring& str_path) {
  DWORD u4_high{0};
  SetLastError(ERROR_SUCCESS);
  const DWORD u4_low{
      GetCompressedFileSizeW(ntfscompact_test::temp_long_api_path(str_path).c_str(),
                             &u4_high)};
  if (u4_low == INVALID_FILE_SIZE && GetLastError() != ERROR_SUCCESS) {
    return 0;
  }
  return (static_cast<std::uint64_t>(u4_high) << 32) |
         static_cast<std::uint64_t>(u4_low);
}

std::wstring decode_utf8(const std::string& str_bytes) {
  if (str_bytes.empty()) {
    return L"";
  }
  const int i4_wchars{MultiByteToWideChar(CP_UTF8, 0, str_bytes.data(),
                                          static_cast<int>(str_bytes.size()),
                                          nullptr, 0)};
  std::wstring str_wide(static_cast<std::size_t>(i4_wchars), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, str_bytes.data(),
                      static_cast<int>(str_bytes.size()), str_wide.data(),
                      i4_wchars);
  return str_wide;
}

std::wstring read_pipe_wide(HANDLE h_read) {
  std::string str_bytes{};
  char buf_read[4096]{};
  DWORD u4_read{0};
  while (ReadFile(h_read, buf_read, sizeof(buf_read), &u4_read, nullptr) &&
         u4_read > 0) {
    str_bytes.append(buf_read, u4_read);
  }
  return decode_utf8(str_bytes);
}

struct ToolRun {
  int i4_code{-1};
  std::wstring str_out{};
  std::wstring str_err{};
};

struct TestHandleReleaser {
  void operator()(HANDLE h_raw) const { CloseHandle(h_raw); }
};

using TestHandle =
    std::unique_ptr<std::remove_pointer_t<HANDLE>, TestHandleReleaser>;

bool run_tool(const std::wstring& str_arguments, ToolRun& run_result) {
  SECURITY_ATTRIBUTES sa_inherit{};
  sa_inherit.nLength = sizeof(sa_inherit);
  sa_inherit.bInheritHandle = TRUE;
  HANDLE h_out_read_raw{INVALID_HANDLE_VALUE};
  HANDLE h_out_write_raw{INVALID_HANDLE_VALUE};
  if (!CreatePipe(&h_out_read_raw, &h_out_write_raw, &sa_inherit, 0)) {
    return false;
  }
  TestHandle guard_out_read{h_out_read_raw};
  TestHandle guard_out_write{h_out_write_raw};
  HANDLE h_err_read_raw{INVALID_HANDLE_VALUE};
  HANDLE h_err_write_raw{INVALID_HANDLE_VALUE};
  if (!CreatePipe(&h_err_read_raw, &h_err_write_raw, &sa_inherit, 0)) {
    return false;
  }
  TestHandle guard_err_read{h_err_read_raw};
  TestHandle guard_err_write{h_err_write_raw};
  SetHandleInformation(guard_out_read.get(), HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(guard_err_read.get(), HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si_startup{};
  si_startup.cb = sizeof(si_startup);
  si_startup.dwFlags = STARTF_USESTDHANDLES;
  si_startup.hStdOutput = guard_out_write.get();
  si_startup.hStdError = guard_err_write.get();
  PROCESS_INFORMATION pi_process{};
  const std::wstring str_command{L"\"" + g_str_tool_path + L"\" " + str_arguments};
  std::vector<wchar_t> vec_command(str_command.begin(), str_command.end());
  vec_command.push_back(L'\0');
  const BOOL b_started{CreateProcessW(nullptr, vec_command.data(), nullptr,
                                      nullptr, TRUE, 0, nullptr, nullptr,
                                      &si_startup, &pi_process)};
  guard_out_write.reset();
  guard_err_write.reset();
  if (!b_started) {
    return false;
  }
  TestHandle guard_process{pi_process.hProcess};
  TestHandle guard_thread{pi_process.hThread};
  run_result.str_out = read_pipe_wide(guard_out_read.get());
  run_result.str_err = read_pipe_wide(guard_err_read.get());
  const DWORD u4_wait{WaitForSingleObject(guard_process.get(), 60000)};
  if (u4_wait != WAIT_OBJECT_0) {
    TerminateProcess(guard_process.get(), 1);
  }
  DWORD u4_exit{static_cast<DWORD>(-1)};
  GetExitCodeProcess(guard_process.get(), &u4_exit);
  run_result.i4_code = static_cast<int>(u4_exit);
  return u4_wait == WAIT_OBJECT_0;
}

bool file_is_wof(const DetectionResult& dr_state, Algorithm e_algorithm) {
  return dr_state.str_error.empty() &&
         dr_state.cs_state.e_kind == CurrentState::Kind::kWof &&
         dr_state.cs_state.i4_provider == CurrentState::kProviderFile &&
         dr_state.cs_state.e_algorithm == e_algorithm;
}

}

TEST_CASE("e2e_flow：混合目录压缩→逐文件复核→干跑全跳过→解压→复核全普通") {
  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};

  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_dir{ntfscompact_test::temp_path(tmp_dir)};
  const std::wstring str_plain{ntfscompact_test::temp_sub(tmp_dir, L"plain.bin")};
  const std::wstring str_readonly{
      ntfscompact_test::temp_sub(tmp_dir, L"readonly.bin")};
  const std::wstring str_lzx{ntfscompact_test::temp_sub(tmp_dir, L"lzx.bin")};
  const std::wstring str_lznt1{ntfscompact_test::temp_sub(tmp_dir, L"lznt1.bin")};
  const std::wstring str_random{ntfscompact_test::temp_sub(tmp_dir, L"random.bin")};

  const std::uint64_t u8_plain{write_compressible(str_plain)};
  REQUIRE(u8_plain > 0);
  const std::uint64_t u8_readonly{write_compressible(str_readonly)};
  REQUIRE(u8_readonly > 0);
  REQUIRE(SetFileAttributesW(
      ntfscompact_test::temp_long_api_path(str_readonly).c_str(),
      FILE_ATTRIBUTE_READONLY));
  const std::uint64_t u8_lzx{write_compressible(str_lzx)};
  REQUIRE(u8_lzx > 0);
  const std::uint64_t u8_lznt1{write_compressible(str_lznt1)};
  REQUIRE(u8_lznt1 > 0);
  REQUIRE(run_compact(L"/c \"" + str_lznt1 + L"\"") == 0);
  REQUIRE((GetFileAttributesW(ntfscompact_test::temp_long_api_path(str_lznt1)
                                  .c_str()) &
           FILE_ATTRIBUTE_COMPRESSED) != 0);
  const std::uint64_t u8_random{write_random(str_random)};
  REQUIRE(u8_random > 0);

  ToolRun run_setup{};
  REQUIRE(run_tool(L"-a lzx \"" + str_lzx + L"\"", run_setup));
  REQUIRE_MESSAGE(run_setup.i4_code == 0,
                  narrow(run_setup.str_out + run_setup.str_err));
  REQUIRE(file_is_wof(fn_detect(str_lzx), Algorithm::kLzx));

  ToolRun run_compress{};
  REQUIRE(run_tool(L"\"" + str_dir + L"\"", run_compress));
  REQUIRE_MESSAGE(run_compress.i4_code == 0,
                  narrow(run_compress.str_out + run_compress.str_err));

  REQUIRE(file_is_wof(fn_detect(str_plain), Algorithm::kXpress8k));
  REQUIRE(file_is_wof(fn_detect(str_readonly), Algorithm::kXpress8k));
  REQUIRE(file_is_wof(fn_detect(str_lzx), Algorithm::kXpress8k));
  REQUIRE(file_is_wof(fn_detect(str_lznt1), Algorithm::kXpress8k));
  CHECK((GetFileAttributesW(ntfscompact_test::temp_long_api_path(str_readonly)
                                .c_str()) &
         FILE_ATTRIBUTE_READONLY) != 0);
  const DetectionResult dr_random{fn_detect(str_random)};
  const bool b_random_compressed{file_is_wof(dr_random, Algorithm::kXpress8k)};
  if (!b_random_compressed) {
    CHECK(dr_random.cs_state.e_kind == CurrentState::Kind::kNone);
  }

  CHECK(has_text(run_compress.str_out, L"[压缩] " + str_plain));
  CHECK(has_text(run_compress.str_out, L"[压缩] " + str_readonly));
  CHECK(has_text(run_compress.str_out, L"[重压] " + str_lzx));
  CHECK(has_text(run_compress.str_out, L"[重压] " + str_lznt1));
  CHECK(has_text(run_compress.str_out, str_random));

  std::uint64_t u8_orig_expected{u8_plain + u8_readonly + u8_lzx + u8_lznt1};
  std::uint64_t u8_disk_expected{disk_usage(str_plain) + disk_usage(str_readonly) +
                                 disk_usage(str_lzx) + disk_usage(str_lznt1)};
  if (b_random_compressed) {
    u8_orig_expected += u8_random;
    u8_disk_expected += disk_usage(str_random);
    CHECK(has_text(run_compress.str_out,
                   L"共 5 个文件：压缩 3、重压 2、解压 0、跳过 0"));
  } else {
    CHECK(has_text(run_compress.str_out,
                   L"共 5 个文件：压缩 2、重压 2、解压 0、跳过 1"));
    CHECK(has_text(run_compress.str_out, L"random.bin"));
  }
  CHECK(u8_disk_expected < u8_orig_expected);
  const std::uint64_t u8_percent{
      (u8_orig_expected - u8_disk_expected) * 100 / u8_orig_expected};
  CHECK(has_text(run_compress.str_out,
                 std::format(L"；压缩/重压 {} → {} 字节，节省 {}%",
                             u8_orig_expected, u8_disk_expected, u8_percent)));

  ToolRun run_dry{};
  REQUIRE(run_tool(L"-n \"" + str_dir + L"\"", run_dry));
  REQUIRE_MESSAGE(run_dry.i4_code == 0,
                  narrow(run_dry.str_out + run_dry.str_err));
  CHECK(has_text(run_dry.str_out, str_plain + L"：已是指定算法 xpress8k"));
  CHECK(has_text(run_dry.str_out, str_readonly + L"：已是指定算法 xpress8k"));
  CHECK(has_text(run_dry.str_out, str_lzx + L"：已是指定算法 xpress8k"));
  CHECK(has_text(run_dry.str_out, str_lznt1 + L"：已是指定算法 xpress8k"));
  CHECK_FALSE(has_text(run_dry.str_out, L"压缩/重压"));
  if (b_random_compressed) {
    CHECK(has_text(run_dry.str_out,
                   L"共 5 个文件：压缩 0、重压 0、解压 0、跳过 5"));
  } else {
    CHECK(has_text(run_dry.str_out,
                   L"共 5 个文件：压缩 1、重压 0、解压 0、跳过 4"));
    CHECK(has_text(run_dry.str_out,
                   str_random + L"：现状 未压缩，将压缩为指定算法 xpress8k"));
  }

  ToolRun run_uncompress{};
  REQUIRE(run_tool(L"-u \"" + str_dir + L"\"", run_uncompress));
  REQUIRE_MESSAGE(run_uncompress.i4_code == 0,
                  narrow(run_uncompress.str_out + run_uncompress.str_err));
  CHECK(has_text(run_uncompress.str_out, L"[解压] " + str_plain));
  CHECK(has_text(run_uncompress.str_out, str_plain + L"：已解压"));
  CHECK(has_text(run_uncompress.str_out, L"[解压] " + str_lznt1));
  if (b_random_compressed) {
    CHECK(has_text(run_uncompress.str_out,
                   L"共 5 个文件：压缩 0、重压 0、解压 5、跳过 0"));
  } else {
    CHECK(has_text(run_uncompress.str_out,
                   L"共 5 个文件：压缩 0、重压 0、解压 4、跳过 1"));
    CHECK(has_text(run_uncompress.str_out, L"[跳过] " + str_random));
  }
  for (const std::wstring& str_file : {str_plain, str_readonly, str_lzx,
                                       str_lznt1, str_random}) {
    const DetectionResult dr_after{fn_detect(str_file)};
    REQUIRE_MESSAGE(dr_after.str_error.empty(), narrow(dr_after.str_error));
    CHECK(dr_after.cs_state.e_kind == CurrentState::Kind::kNone);
  }
  CHECK((GetFileAttributesW(ntfscompact_test::temp_long_api_path(str_readonly)
                                .c_str()) &
         FILE_ATTRIBUTE_READONLY) != 0);
}

TEST_CASE("e2e_flow：退出码三态（用法 2 / 部分失败 1 / 全成 0）") {
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_ghost{
      ntfscompact_test::temp_sub(tmp_dir, L"ghost.bin")};

  ToolRun run_no_args{};
  REQUIRE(run_tool(L"", run_no_args));
  CHECK(run_no_args.i4_code == 2);
  CHECK_FALSE(run_no_args.str_err.empty());

  ToolRun run_bad_algo{};
  REQUIRE(run_tool(
      L"-a zip \"" + ntfscompact_test::temp_path(tmp_dir) + L"\"", run_bad_algo));
  CHECK(run_bad_algo.i4_code == 2);
  CHECK_FALSE(run_bad_algo.str_err.empty());

  ToolRun run_missing{};
  REQUIRE(run_tool(L"\"" + str_ghost + L"\"", run_missing));
  CHECK(run_missing.i4_code == 1);
  CHECK(has_text(run_missing.str_err, L"ghost.bin"));
  CHECK(has_text(run_missing.str_err, L"[失败]"));

  const std::uint64_t u8_plain{
      write_compressible(ntfscompact_test::temp_sub(tmp_dir, L"plain.bin"))};
  REQUIRE(u8_plain > 0);
  ToolRun run_ok{};
  REQUIRE(run_tool(L"-q \"" + ntfscompact_test::temp_path(tmp_dir) + L"\"", run_ok));
  REQUIRE_MESSAGE(run_ok.i4_code == 0, narrow(run_ok.str_out + run_ok.str_err));
  CHECK(has_text(run_ok.str_out, L"共 1 个文件：压缩 1、重压 0、解压 0、跳过 0"));
  CHECK(has_text(run_ok.str_out, L"；压缩/重压"));
  ToolRun run_verify{};
  REQUIRE(run_tool(
      L"-n \"" + ntfscompact_test::temp_path(tmp_dir) + L"\"", run_verify));
  REQUIRE(run_verify.i4_code == 0);
  CHECK(has_text(run_verify.str_out,
                 L"共 1 个文件：压缩 0、重压 0、解压 0、跳过 1"));
  CHECK_FALSE(has_text(run_verify.str_out, L"压缩/重压"));
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fwprintf(stderr, L"missing tool path argument\n");
    return 2;
  }
  g_str_tool_path = argv[1];
  doctest::Context ctx_context{};
  return ctx_context.run();
}
