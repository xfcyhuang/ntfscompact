#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "detect.hpp"
#include "release.hpp"
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
using ntfscompact_test::dir_is_marked;
using ntfscompact_test::mark_dir;
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

bool deny_dir_write_data(const std::wstring& str_dir) {
  wchar_t arr_user[256]{};
  DWORD u4_len{sizeof(arr_user) / sizeof(wchar_t)};
  if (!GetUserNameW(arr_user, &u4_len)) {
    return false;
  }
  const std::wstring str_cmd{L"icacls \"" + str_dir + L"\" /deny \"" +
                             std::wstring(arr_user) +
                             L":(WD,AD)\" >nul 2>&1"};
  return _wsystem(str_cmd.c_str()) == 0;
}

void restore_dir_acl(const std::wstring& str_dir) {
  const std::wstring str_cmd{L"icacls \"" + str_dir + L"\" /reset >nul 2>&1"};
  _wsystem(str_cmd.c_str());
}

struct HoldChild {
  HANDLE h_process{INVALID_HANDLE_VALUE};
  HANDLE h_thread{INVALID_HANDLE_VALUE};
  DWORD u4_pid{0};
};

struct HoldChildReleaser {
  void operator()(HoldChild* ptr_child) const {
    if (ptr_child == nullptr) {
      return;
    }
    if (ptr_child->h_process != INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(ptr_child->h_process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(ptr_child->h_process, 1);
        WaitForSingleObject(ptr_child->h_process, 5000);
      }
      CloseHandle(ptr_child->h_process);
    }
    if (ptr_child->h_thread != INVALID_HANDLE_VALUE) {
      CloseHandle(ptr_child->h_thread);
    }
    delete ptr_child;
  }
};

using HoldChildPtr = std::unique_ptr<HoldChild, HoldChildReleaser>;

std::wstring hold_exe_path() {
  wchar_t arr_module[MAX_PATH]{};
  const DWORD u4_len{GetModuleFileNameW(nullptr, arr_module, MAX_PATH)};
  const std::wstring str_module{arr_module, u4_len};
  const std::size_t u8_slash{str_module.find_last_of(L"\\")};
  return str_module.substr(0, u8_slash) + L"\\ntfscompact_hold.exe";
}

HoldChildPtr spawn_hold(const std::wstring& str_hold_exe,
                        const std::wstring& str_file, bool b_stubborn) {
  std::wstring str_cmd{L"\"" + str_hold_exe + L"\" \"" + str_file + L"\""};
  if (b_stubborn) {
    str_cmd += L" --stubborn";
  }
  STARTUPINFOW si_start{};
  si_start.cb = sizeof(si_start);
  PROCESS_INFORMATION pi_process{};
  const BOOL b_ok{CreateProcessW(str_hold_exe.c_str(), str_cmd.data(), nullptr,
                                 nullptr, FALSE, 0, nullptr, nullptr,
                                 &si_start, &pi_process)};
  if (b_ok == FALSE) {
    return HoldChildPtr{};
  }
  HoldChildPtr up_child{new HoldChild{}};
  up_child->h_process = pi_process.hProcess;
  up_child->h_thread = pi_process.hThread;
  up_child->u4_pid = pi_process.dwProcessId;
  return up_child;
}

bool child_is_alive(const HoldChildPtr& up_child) {
  return WaitForSingleObject(up_child->h_process, 0) == WAIT_TIMEOUT;
}

bool wait_file_occupied(const std::wstring& str_long_path) {
  for (int i4_attempt{0}; i4_attempt < 100; ++i4_attempt) {
    if (ntfscompact::probe_file_occupied(str_long_path)) {
      return true;
    }
    Sleep(50);
  }
  return false;
}

bool wait_child_exit(const HoldChildPtr& up_child, DWORD u4_timeout_ms,
                     DWORD& u4_code) {
  if (WaitForSingleObject(up_child->h_process, u4_timeout_ms) !=
      WAIT_OBJECT_0) {
    return false;
  }
  return GetExitCodeProcess(up_child->h_process, &u4_code) != FALSE;
}

std::wstring lowercase_text(std::wstring str_text) {
  for (wchar_t& ch_wide : str_text) {
    ch_wide = static_cast<wchar_t>(std::towlower(ch_wide));
  }
  return str_text;
}

bool make_deep_dir(std::wstring& str_deep) {
  int i4_level{0};
  while (ntfscompact_test::temp_long_api_path(str_deep).size() <= 310) {
    if (i4_level >= 100) {
      return false;
    }
    str_deep += std::format(L"\\lvl{:03}", i4_level);
    if (CreateDirectoryW(
            ntfscompact_test::temp_long_api_path(str_deep).c_str(),
            nullptr) == FALSE) {
      return false;
    }
    ++i4_level;
  }
  return ntfscompact_test::temp_long_api_path(str_deep).size() > 310;
}

std::wstring hold_audit_needle(const wchar_t* ptr_level, DWORD u4_pid) {
  return lowercase_text(std::wstring{L"；升级释放："} + ptr_level +
                        L"（ntfscompact_hold.exe pid " +
                        std::to_wstring(u4_pid) + L"）");
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

TEST_CASE("e2e_flow：目录压缩标记——干跑列出、实跑清除、-q 汇总计数、解压同清、失败退出码 1") {
  auto xe_detector{make_real_detector()};
  REQUIRE(xe_detector.has_value());
  const DetectFn fn_detect{std::move(xe_detector).value()};

  ntfscompact_test::TempDir tmp_main{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_main->b_ok);
  const std::wstring str_root{ntfscompact_test::temp_path(tmp_main)};
  const std::wstring str_data{ntfscompact_test::temp_sub(tmp_main, L"data")};
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_main, L"data"));
  const std::wstring str_file{ntfscompact_test::temp_sub(tmp_main, L"data\\keep.bin")};
  REQUIRE(write_compressible(str_file) > 0);
  REQUIRE(mark_dir(str_root));
  REQUIRE(mark_dir(str_data));

  ToolRun run_dry{};
  REQUIRE(run_tool(L"-n \"" + str_root + L"\"", run_dry));
  REQUIRE_MESSAGE(run_dry.i4_code == 0,
                  narrow(run_dry.str_out + run_dry.str_err));
  CHECK(has_text(run_dry.str_out, L"[目录标记] " + str_root + L"：将清除标记"));
  CHECK(has_text(run_dry.str_out, L"[目录标记] " + str_data + L"：将清除标记"));
  CHECK(has_text(run_dry.str_out, L"；将清除目录标记 2 个"));
  CHECK(dir_is_marked(str_root));
  CHECK(dir_is_marked(str_data));

  ToolRun run_real{};
  REQUIRE(run_tool(L"\"" + str_root + L"\"", run_real));
  REQUIRE_MESSAGE(run_real.i4_code == 0,
                  narrow(run_real.str_out + run_real.str_err));
  CHECK(has_text(run_real.str_out, L"[目录标记] " + str_root + L"：已清除标记"));
  CHECK(has_text(run_real.str_out, L"[目录标记] " + str_data + L"：已清除标记"));
  CHECK(has_text(run_real.str_out, L"；已清除目录标记 2 个"));
  CHECK_FALSE(dir_is_marked(str_root));
  CHECK_FALSE(dir_is_marked(str_data));
  CHECK(file_is_wof(fn_detect(str_file), Algorithm::kXpress8k));

  REQUIRE(ntfscompact_test::temp_write_file(tmp_main, L"data\\after.bin", 8192));
  CHECK_FALSE(dir_is_marked(ntfscompact_test::temp_sub(tmp_main, L"data\\after.bin")));

  ToolRun run_repeat{};
  REQUIRE(run_tool(L"\"" + str_root + L"\"", run_repeat));
  REQUIRE_MESSAGE(run_repeat.i4_code == 0,
                  narrow(run_repeat.str_out + run_repeat.str_err));
  CHECK_FALSE(has_text(run_repeat.str_out, L"目录标记"));

  ToolRun run_uncompress{};
  REQUIRE(mark_dir(str_data));
  REQUIRE(run_tool(L"-u \"" + str_root + L"\"", run_uncompress));
  REQUIRE_MESSAGE(run_uncompress.i4_code == 0,
                  narrow(run_uncompress.str_out + run_uncompress.str_err));
  CHECK(has_text(run_uncompress.str_out,
                 L"[目录标记] " + str_data + L"：已清除标记"));
  CHECK(has_text(run_uncompress.str_out, L"；已清除目录标记 1 个"));
  CHECK_FALSE(dir_is_marked(str_data));

  ToolRun run_quiet{};
  REQUIRE(mark_dir(str_data));
  REQUIRE(run_tool(L"-q \"" + str_root + L"\"", run_quiet));
  REQUIRE_MESSAGE(run_quiet.i4_code == 0,
                  narrow(run_quiet.str_out + run_quiet.str_err));
  CHECK(run_quiet.str_out.find(L'\n') ==
        run_quiet.str_out.rfind(L'\n'));
  CHECK(has_text(run_quiet.str_out, L"；已清除目录标记 1 个"));
  CHECK_FALSE(has_text(run_quiet.str_out, L"["));
  CHECK_FALSE(dir_is_marked(str_data));

  ntfscompact_test::TempDir tmp_fail{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_fail->b_ok);
  const std::wstring str_fail_root{ntfscompact_test::temp_path(tmp_fail)};
  const std::wstring str_locked{ntfscompact_test::temp_sub(tmp_fail, L"locked")};
  REQUIRE(ntfscompact_test::temp_make_dir(tmp_fail, L"locked"));
  REQUIRE(write_compressible(
              ntfscompact_test::temp_sub(tmp_fail, L"plain.bin")) > 0);
  REQUIRE(mark_dir(str_locked));
  REQUIRE(deny_dir_write_data(str_locked));

  ToolRun run_fail{};
  const bool b_tool_ran{run_tool(L"\"" + str_fail_root + L"\"", run_fail)};
  restore_dir_acl(str_locked);
  REQUIRE(b_tool_ran);
  CHECK(run_fail.i4_code == 1);
  CHECK(has_text(run_fail.str_err, L"[失败] " + str_locked));
  CHECK(has_text(run_fail.str_err, L"无法打开目录（错误码 5）"));
  CHECK(has_text(run_fail.str_out, L"plain.bin：已压缩为指定算法 xpress8k"));
  CHECK(has_text(run_fail.str_out, L"；失败 1"));
  CHECK(dir_is_marked(str_locked));
}

TEST_CASE("e2e_release：占用经链释放后压缩成功且审计行正确（RM 优雅关）") {
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_busy{ntfscompact_test::temp_sub(tmp_dir, L"busy.bin")};
  const std::wstring str_free{ntfscompact_test::temp_sub(tmp_dir, L"free.bin")};
  REQUIRE(write_compressible(str_busy) > 0);
  REQUIRE(write_compressible(str_free) > 0);
  HoldChildPtr up_child{spawn_hold(str_hold_exe, str_busy, false)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(ntfscompact_test::temp_long_api_path(str_busy)));

  ToolRun run_compress{};
  REQUIRE(run_tool(L"\"" + ntfscompact_test::temp_path(tmp_dir) + L"\"",
                   run_compress));
  REQUIRE_MESSAGE(run_compress.i4_code == 0,
                  narrow(run_compress.str_out + run_compress.str_err));
  CHECK(has_text(run_compress.str_out,
                 L"[压缩] " + str_busy + L"：已压缩为指定算法 xpress8k"));
  CHECK(has_text(lowercase_text(run_compress.str_out),
                 hold_audit_needle(L"RM 优雅关", up_child->u4_pid)));
  CHECK(has_text(run_compress.str_out,
                 L"[压缩] " + str_free + L"：已压缩为指定算法 xpress8k"));
  CHECK(has_text(run_compress.str_out,
                 L"共 2 个文件：压缩 2、重压 0、解压 0、跳过 0"));
  DWORD u4_code{0};
  REQUIRE(wait_child_exit(up_child, 15000, u4_code));
  CHECK(u4_code == 0);
  CHECK_FALSE(ntfscompact::probe_file_occupied(
      ntfscompact_test::temp_long_api_path(str_busy)));
}

TEST_CASE("e2e_release：--stubborn 顽固占用升级至 RM 强制关后压缩成功") {
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_busy{ntfscompact_test::temp_sub(tmp_dir, L"busy.bin")};
  const std::wstring str_free{ntfscompact_test::temp_sub(tmp_dir, L"free.bin")};
  REQUIRE(write_compressible(str_busy) > 0);
  REQUIRE(write_compressible(str_free) > 0);
  HoldChildPtr up_child{spawn_hold(str_hold_exe, str_busy, true)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(ntfscompact_test::temp_long_api_path(str_busy)));

  ToolRun run_compress{};
  REQUIRE(run_tool(L"\"" + ntfscompact_test::temp_path(tmp_dir) + L"\"",
                   run_compress));
  REQUIRE_MESSAGE(run_compress.i4_code == 0,
                  narrow(run_compress.str_out + run_compress.str_err));
  CHECK(has_text(run_compress.str_out,
                 L"[压缩] " + str_busy + L"：已压缩为指定算法 xpress8k"));
  CHECK(has_text(lowercase_text(run_compress.str_out),
                 hold_audit_needle(L"RM 强制关", up_child->u4_pid)));
  CHECK(has_text(run_compress.str_out,
                 L"共 2 个文件：压缩 2、重压 0、解压 0、跳过 0"));
  DWORD u4_code{0};
  CHECK(wait_child_exit(up_child, 30000, u4_code));
  CHECK_FALSE(ntfscompact::probe_file_occupied(
      ntfscompact_test::temp_long_api_path(str_busy)));
}

TEST_CASE("e2e_release：-q 下释放运行仅剩汇总行且退出码不变") {
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  const std::wstring str_busy{ntfscompact_test::temp_sub(tmp_dir, L"busy.bin")};
  const std::wstring str_free{ntfscompact_test::temp_sub(tmp_dir, L"free.bin")};
  REQUIRE(write_compressible(str_busy) > 0);
  REQUIRE(write_compressible(str_free) > 0);
  HoldChildPtr up_child{spawn_hold(str_hold_exe, str_busy, false)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(ntfscompact_test::temp_long_api_path(str_busy)));

  ToolRun run_quiet{};
  REQUIRE(run_tool(L"-q \"" + ntfscompact_test::temp_path(tmp_dir) + L"\"",
                   run_quiet));
  REQUIRE_MESSAGE(run_quiet.i4_code == 0,
                  narrow(run_quiet.str_out + run_quiet.str_err));
  CHECK(run_quiet.str_out.find(L'\n') == run_quiet.str_out.rfind(L'\n'));
  CHECK(has_text(run_quiet.str_out,
                 L"共 2 个文件：压缩 2、重压 0、解压 0、跳过 0"));
  CHECK(has_text(run_quiet.str_out, L"；压缩/重压 "));
  CHECK_FALSE(has_text(run_quiet.str_out, L"升级释放"));
  CHECK_FALSE(has_text(run_quiet.str_out, L"["));
  CHECK(run_quiet.str_err.empty());
  DWORD u4_code{0};
  REQUIRE(wait_child_exit(up_child, 15000, u4_code));
  CHECK(u4_code == 0);
}

TEST_CASE("e2e_release：链尽跳过——非提权如实止步不终结占用进程，其余文件继续") {
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  std::wstring str_deep{ntfscompact_test::temp_path(tmp_dir)};
  REQUIRE(make_deep_dir(str_deep));
  str_deep += L"\\long.bin";
  REQUIRE(write_compressible(str_deep) > 0);
  const std::wstring str_free{ntfscompact_test::temp_sub(tmp_dir, L"free.bin")};
  REQUIRE(write_compressible(str_free) > 0);
  const std::wstring str_deep_long{
      ntfscompact_test::temp_long_api_path(str_deep)};
  HoldChildPtr up_child{spawn_hold(str_hold_exe, str_deep_long, true)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(str_deep_long));

  ToolRun run_compress{};
  REQUIRE(run_tool(L"\"" + str_deep_long + L"\" \"" +
                       ntfscompact_test::temp_long_api_path(str_free) + L"\"",
                   run_compress));
  if (ntfscompact::process_is_elevated()) {
    REQUIRE_MESSAGE(run_compress.i4_code == 0,
                    narrow(run_compress.str_out + run_compress.str_err));
    CHECK(has_text(run_compress.str_out,
                   L"[压缩] " + str_deep + L"：已压缩为指定算法 xpress8k"));
    CHECK(has_text(lowercase_text(run_compress.str_out),
                   hold_audit_needle(L"句柄强关", up_child->u4_pid)));
    CHECK(has_text(run_compress.str_out,
                   L"共 2 个文件：压缩 2、重压 0、解压 0、跳过 0"));
    CHECK_FALSE(ntfscompact::probe_file_occupied(str_deep_long));
    CHECK(child_is_alive(up_child));
  } else {
    REQUIRE_MESSAGE(run_compress.i4_code == 0,
                    narrow(run_compress.str_out + run_compress.str_err));
    CHECK(has_text(run_compress.str_out,
                   L"[跳过] " + str_deep +
                       L"：占用不可释放：升级链尽后仍被占用（句柄强关需管理员权限"));
    CHECK(has_text(run_compress.str_out,
                   L"[压缩] " + str_free + L"：已压缩为指定算法 xpress8k"));
    CHECK(has_text(run_compress.str_out,
                   L"共 2 个文件：压缩 1、重压 0、解压 0、跳过 1"));
    CHECK_FALSE(has_text(run_compress.str_out, L"升级释放"));
    CHECK(ntfscompact::probe_file_occupied(str_deep_long));
    CHECK(child_is_alive(up_child));
  }
}

TEST_CASE("e2e_release：-q 下链尽跳过行被抑制且退出码不变") {
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  std::wstring str_deep{ntfscompact_test::temp_path(tmp_dir)};
  REQUIRE(make_deep_dir(str_deep));
  str_deep += L"\\long.bin";
  REQUIRE(write_compressible(str_deep) > 0);
  const std::wstring str_free{ntfscompact_test::temp_sub(tmp_dir, L"free.bin")};
  REQUIRE(write_compressible(str_free) > 0);
  const std::wstring str_deep_long{
      ntfscompact_test::temp_long_api_path(str_deep)};
  HoldChildPtr up_child{spawn_hold(str_hold_exe, str_deep_long, true)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(str_deep_long));

  ToolRun run_quiet{};
  REQUIRE(run_tool(L"-q \"" + str_deep_long + L"\" \"" +
                       ntfscompact_test::temp_long_api_path(str_free) + L"\"",
                   run_quiet));
  REQUIRE_MESSAGE(run_quiet.i4_code == 0,
                  narrow(run_quiet.str_out + run_quiet.str_err));
  CHECK(run_quiet.str_out.find(L'\n') == run_quiet.str_out.rfind(L'\n'));
  CHECK(has_text(run_quiet.str_out,
                 L"共 2 个文件：压缩 1、重压 0、解压 0、跳过 1"));
  CHECK_FALSE(has_text(run_quiet.str_out, L"升级链尽"));
  CHECK_FALSE(has_text(run_quiet.str_out, L"升级释放"));
  CHECK_FALSE(has_text(run_quiet.str_out, L"["));
  CHECK(run_quiet.str_err.empty());
  if (!ntfscompact::process_is_elevated()) {
    CHECK(ntfscompact::probe_file_occupied(str_deep_long));
    CHECK(child_is_alive(up_child));
  }
}

TEST_CASE("e2e_release：--force-close-all 走全链——非提权如实止步，提权句柄级可达") {
  const std::wstring str_hold_exe{hold_exe_path()};
  REQUIRE(GetFileAttributesW(str_hold_exe.c_str()) != INVALID_FILE_ATTRIBUTES);
  ntfscompact_test::TempDir tmp_dir{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp_dir->b_ok);
  std::wstring str_deep{ntfscompact_test::temp_path(tmp_dir)};
  REQUIRE(make_deep_dir(str_deep));
  str_deep += L"\\long.bin";
  REQUIRE(write_compressible(str_deep) > 0);
  const std::wstring str_free{ntfscompact_test::temp_sub(tmp_dir, L"free.bin")};
  REQUIRE(write_compressible(str_free) > 0);
  const std::wstring str_deep_long{
      ntfscompact_test::temp_long_api_path(str_deep)};
  HoldChildPtr up_child{spawn_hold(str_hold_exe, str_deep_long, true)};
  REQUIRE(up_child);
  REQUIRE(wait_file_occupied(str_deep_long));

  ToolRun run_force{};
  REQUIRE(run_tool(L"--force-close-all \"" + str_deep_long + L"\" \"" +
                       ntfscompact_test::temp_long_api_path(str_free) + L"\"",
                   run_force));
  if (ntfscompact::process_is_elevated()) {
    REQUIRE_MESSAGE(run_force.i4_code == 0,
                    narrow(run_force.str_out + run_force.str_err));
    CHECK(has_text(run_force.str_out,
                   L"[压缩] " + str_deep + L"：已压缩为指定算法 xpress8k"));
    CHECK(has_text(lowercase_text(run_force.str_out),
                   hold_audit_needle(L"句柄强关", up_child->u4_pid)));
    CHECK(has_text(run_force.str_out,
                   L"共 2 个文件：压缩 2、重压 0、解压 0、跳过 0"));
    CHECK_FALSE(ntfscompact::probe_file_occupied(str_deep_long));
    CHECK(child_is_alive(up_child));
  } else {
    REQUIRE_MESSAGE(run_force.i4_code == 0,
                    narrow(run_force.str_out + run_force.str_err));
    CHECK(has_text(run_force.str_out,
                   L"[跳过] " + str_deep +
                       L"：占用不可释放：升级链尽后仍被占用（句柄强关需管理员权限"));
    CHECK(has_text(run_force.str_out,
                   L"[压缩] " + str_free + L"：已压缩为指定算法 xpress8k"));
    CHECK(has_text(run_force.str_out,
                   L"共 2 个文件：压缩 1、重压 0、解压 0、跳过 1"));
    CHECK(ntfscompact::probe_file_occupied(str_deep_long));
    CHECK(child_is_alive(up_child));
  }
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
