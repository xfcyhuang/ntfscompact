#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "scan.hpp"
#include "temp_dir.hpp"
#include "util.hpp"

#include <windows.h>

namespace {

std::wstring basename(const std::wstring& str_path) {
  const std::size_t u8_pos{str_path.find_last_of(L"\\/")};
  return u8_pos == std::wstring::npos ? str_path : str_path.substr(u8_pos + 1);
}

std::vector<std::wstring> basenames(const ntfscompact::ScanOutcome& so_scan) {
  std::vector<std::wstring> vec_names{};
  for (const auto& si_item : so_scan.vec_items) {
    vec_names.push_back(basename(si_item.str_display_path));
  }
  std::sort(vec_names.begin(), vec_names.end());
  return vec_names;
}

bool make_junction(const std::wstring& str_link, const std::wstring& str_target) {
  const std::wstring str_cmd{
      L"cmd /c mklink /J \"" + str_link + L"\" \"" + str_target + L"\""};
  return _wsystem(str_cmd.c_str()) == 0;
}

}

using ntfscompact::FileMetadata;
using ntfscompact::FlaggedDir;
using ntfscompact::ScanOutcome;
using ntfscompact::check_file_candidate;
using ntfscompact::scan_paths;

TEST_CASE("单文件路径参数：收录且大小正确") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"a.bin", 8192));

  ScanOutcome so_scan{};
  scan_paths({ntfscompact_test::temp_sub(tmp, L"a.bin")}, so_scan);
  CHECK(so_scan.vec_errors.empty());
  REQUIRE(so_scan.vec_items.size() == 1);
  CHECK(so_scan.vec_items[0].u8_size == 8192);
  CHECK(so_scan.vec_items[0].str_display_path ==
        ntfscompact_test::temp_sub(tmp, L"a.bin"));
  CHECK(so_scan.vec_items[0].str_long_path.rfind(L"\\\\?\\", 0) == 0);
}

TEST_CASE("目录递归：含子目录全部文件，只收文件不收目录") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"top.txt", 8192));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"sub"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"sub\\inner.dat", 8192));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"sub\\deep"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"sub\\deep\\leaf.txt", 8192));

  ScanOutcome so_scan{};
  scan_paths({ntfscompact_test::temp_path(tmp)}, so_scan);
  CHECK(so_scan.vec_errors.empty());
  CHECK(so_scan.vec_skipped.empty());
  const auto vec_names{basenames(so_scan)};
  std::vector<std::wstring> vec_expected{L"top.txt", L"inner.dat", L"leaf.txt"};
  std::sort(vec_expected.begin(), vec_expected.end());
  CHECK(vec_names == vec_expected);
}

TEST_CASE("不进 junction：防环且目标文件不重复计入") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"real.txt", 8192));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"loop"));
  REQUIRE(make_junction(ntfscompact_test::temp_sub(tmp, L"loop\\inside"),
                        ntfscompact_test::temp_path(tmp)));

  ScanOutcome so_scan{};
  scan_paths({ntfscompact_test::temp_path(tmp)}, so_scan);
  CHECK(so_scan.vec_errors.empty());
  const auto vec_names{basenames(so_scan)};
  REQUIRE(vec_names.size() == 1);
  CHECK(vec_names[0] == L"real.txt");
  REQUIRE(so_scan.vec_skipped.size() == 1);
  CHECK(so_scan.vec_skipped[0].str_display_path.find(L"loop\\inside") !=
        std::wstring::npos);
  CHECK(so_scan.vec_skipped[0].str_reason.find(L"junction") !=
        std::wstring::npos);
}

TEST_CASE("junction 作为显式路径参数：跳过不进入，不算错误") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"real.txt", 8192));
  REQUIRE(make_junction(ntfscompact_test::temp_sub(tmp, L"j"),
                        ntfscompact_test::temp_path(tmp)));

  ScanOutcome so_scan{};
  scan_paths({ntfscompact_test::temp_sub(tmp, L"j")}, so_scan);
  CHECK(so_scan.vec_errors.empty());
  CHECK(so_scan.vec_items.empty());
  REQUIRE(so_scan.vec_skipped.size() == 1);
  CHECK(so_scan.vec_skipped[0].str_reason.find(L"junction") !=
        std::wstring::npos);
}

TEST_CASE("通配符：仅匹配该层文件，不递归子目录") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"a.txt", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"b.dat", 8192));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"sub"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"sub\\c.txt", 8192));

  ScanOutcome so_scan{};
  scan_paths({ntfscompact_test::temp_sub(tmp, L"*.txt")}, so_scan);
  CHECK(so_scan.vec_errors.empty());
  const auto vec_names{basenames(so_scan)};
  const std::vector<std::wstring> vec_expected{L"a.txt"};
  CHECK(vec_names == vec_expected);
}

TEST_CASE("路径不存在：记入错误") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);

  ScanOutcome so_scan{};
  scan_paths({ntfscompact_test::temp_sub(tmp, L"no-such-file.bin")}, so_scan);
  CHECK(so_scan.vec_items.empty());
  REQUIRE(so_scan.vec_errors.size() == 1);
  CHECK(so_scan.vec_errors[0].str_display_path.find(L"no-such-file.bin") !=
        std::wstring::npos);
  CHECK(so_scan.vec_errors[0].str_message.find(L"无法访问") !=
        std::wstring::npos);
}

TEST_CASE("长路径：不带 \\\\?\\ 前缀的超长路径参数可用") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  std::wstring str_deep{};
  for (int i4_level{0}; i4_level < 18; ++i4_level) {
    str_deep += L"\\level-0123456789";
  }
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, str_deep.substr(1)));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, str_deep.substr(1) + L"\\deep.txt",
                                            8192));

  const std::wstring str_unprefixed{
      ntfscompact_test::temp_sub(tmp, str_deep.substr(1))};
  REQUIRE(str_unprefixed.size() > 260);

  ScanOutcome so_scan{};
  scan_paths({str_unprefixed}, so_scan);
  CHECK(so_scan.vec_errors.empty());
  CHECK(basenames(so_scan) == std::vector<std::wstring>{L"deep.txt"});

  ScanOutcome so_scan_second{};
  scan_paths({str_unprefixed + L"\\deep.txt"}, so_scan_second);
  CHECK(so_scan_second.vec_errors.empty());
  REQUIRE(so_scan_second.vec_items.size() == 1);
  CHECK(so_scan_second.vec_items[0].u8_size == 8192);
}

namespace {

constexpr DWORD kReparseAttr{FILE_ATTRIBUTE_REPARSE_POINT};
constexpr DWORD kReparseTagSymlink{0xA000000C};
constexpr DWORD kReparseTagWof{0x80000017};
constexpr DWORD kReparseTagMountPoint{0xA0000003};

FileMetadata make_metadata(const std::wstring& str_path, std::uint64_t u8_size,
                           DWORD u4_attributes = 0, DWORD u4_tag = 0) {
  FileMetadata fm_meta{};
  fm_meta.str_display_path = str_path;
  fm_meta.u8_size = u8_size;
  fm_meta.u4_attributes = u4_attributes;
  fm_meta.u4_reparse_tag = u4_tag;
  return fm_meta;
}

}

TEST_CASE("过滤判定：常规大文件收录") {
  std::wstring str_reason{};
  CHECK(check_file_candidate(make_metadata(L"C:\\d\\a.bin", 8192), &str_reason));
  CHECK(check_file_candidate(make_metadata(L"C:\\d\\b.txt", 4097), &str_reason));
}

TEST_CASE("过滤判定：系统关键文件黑名单（大小写不敏感）") {
  std::wstring str_reason{};
  SUBCASE("pagefile.sys") {
    CHECK_FALSE(check_file_candidate(
        make_metadata(L"C:\\pagefile.sys", 1u << 30), &str_reason));
    CHECK(str_reason.find(L"系统关键") != std::wstring::npos);
  }
  SUBCASE("HIBERFIL.SYS 大写") {
    CHECK_FALSE(check_file_candidate(
        make_metadata(L"C:\\HIBERFIL.SYS", 1u << 30), &str_reason));
  }
  SUBCASE("swapfile.sys 在子目录") {
    CHECK_FALSE(check_file_candidate(
        make_metadata(L"C:\\d\\SwapFile.Sys", 1u << 20), &str_reason));
  }
  SUBCASE("同名前缀不算（pagefile1.sys）") {
    CHECK(check_file_candidate(make_metadata(L"C:\\pagefile1.sys", 8192),
                               &str_reason));
  }
}

TEST_CASE("过滤判定：WinSxS 路径组件黑名单") {
  std::wstring str_reason{};
  SUBCASE("组件精确匹配（大小写不敏感）") {
    CHECK_FALSE(check_file_candidate(
        make_metadata(L"C:\\Windows\\WinSxS\\amd64_microsoft.xml", 8192),
        &str_reason));
    CHECK(str_reason.find(L"WinSxS") != std::wstring::npos);
  }
  SUBCASE("winsxs 小写也命中") {
    CHECK_FALSE(check_file_candidate(
        make_metadata(L"C:\\winsxs\\f.dll", 8192), &str_reason));
  }
  SUBCASE("文件名本身叫 WinSxS 不误伤（按组件匹配）") {
    CHECK(check_file_candidate(make_metadata(L"C:\\d\\WinSxS", 8192),
                               &str_reason));
  }
  SUBCASE("前缀相似不误伤（WinSxSBackup 目录名不同）") {
    CHECK(check_file_candidate(make_metadata(L"C:\\WinSxSBackup\\f.dll", 8192),
                               &str_reason));
  }
}

TEST_CASE("过滤判定：EFS 加密文件跳过") {
  std::wstring str_reason{};
  CHECK_FALSE(check_file_candidate(
      make_metadata(L"C:\\d\\secret.txt", 8192, FILE_ATTRIBUTE_ENCRYPTED),
      &str_reason));
  CHECK(str_reason.find(L"EFS") != std::wstring::npos);
}

TEST_CASE("过滤判定：非常规 reparse point 跳过，WOF reparse 放行") {
  std::wstring str_reason{};
  SUBCASE("符号链接") {
    CHECK_FALSE(check_file_candidate(
        make_metadata(L"C:\\d\\link.txt", 8192, kReparseAttr, kReparseTagSymlink),
        &str_reason));
    CHECK(str_reason.find(L"reparse") != std::wstring::npos);
  }
  SUBCASE("junction 文件形态（mount point）") {
    CHECK_FALSE(check_file_candidate(
        make_metadata(L"C:\\d\\j", 8192, kReparseAttr, kReparseTagMountPoint),
        &str_reason));
  }
  SUBCASE("WOF reparse tag 放行（交现状检测判定）") {
    CHECK(check_file_candidate(
        make_metadata(L"C:\\d\\packed.bin", 8192, kReparseAttr, kReparseTagWof),
        &str_reason));
  }
}

TEST_CASE("过滤判定：≤4KB 跳过（4096 边界）") {
  std::wstring str_reason{};
  CHECK_FALSE(
      check_file_candidate(make_metadata(L"C:\\d\\s.bin", 4096), &str_reason));
  CHECK(str_reason.find(L"4KB") != std::wstring::npos);
  CHECK(check_file_candidate(make_metadata(L"C:\\d\\s.bin", 4097), &str_reason));
}

TEST_CASE("端到端：黑名单与 ≤4KB 在遍历中生效且带原因") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"keep.bin", 8192));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"small.bin", 100));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"pagefile.sys", 1u << 20));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"WinSxS"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"WinSxS\\inside.dll", 1u << 20));

  ScanOutcome so_scan{};
  scan_paths({ntfscompact_test::temp_path(tmp)}, so_scan);
  CHECK(so_scan.vec_errors.empty());
  CHECK(basenames(so_scan) == std::vector<std::wstring>{L"keep.bin"});

  REQUIRE(so_scan.vec_skipped.size() == 3);
  bool b_saw_size{false};
  bool b_saw_critical{false};
  bool b_saw_winsxs{false};
  for (const auto& sk_skip : so_scan.vec_skipped) {
    if (sk_skip.str_display_path.find(L"small.bin") != std::wstring::npos &&
        sk_skip.str_reason.find(L"4KB") != std::wstring::npos) {
      b_saw_size = true;
    }
    if (sk_skip.str_display_path.find(L"pagefile.sys") != std::wstring::npos &&
        sk_skip.str_reason.find(L"系统关键") != std::wstring::npos) {
      b_saw_critical = true;
    }
    if (sk_skip.str_display_path.find(L"WinSxS") != std::wstring::npos &&
        sk_skip.str_reason.find(L"WinSxS") != std::wstring::npos) {
      b_saw_winsxs = true;
    }
  }
  CHECK(b_saw_size);
  CHECK(b_saw_critical);
  CHECK(b_saw_winsxs);
}

bool is_marked_compressed(const std::wstring& str_path) {
  return (GetFileAttributesW(ntfscompact_test::temp_long_api_path(str_path).c_str()) &
          FILE_ATTRIBUTE_COMPRESSED) != 0;
}

TEST_CASE("目录压缩标记：收集带标记目录（含显式根，排除 reparse 与 WinSxS）") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"marked"));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"marked\\inner"));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"plain"));
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"WinSxS"));
  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"plain\\g.bin", 8192));
  REQUIRE(make_junction(ntfscompact_test::temp_sub(tmp, L"link"),
                        ntfscompact_test::temp_path(tmp)));

  const std::wstring str_marked{ntfscompact_test::temp_sub(tmp, L"marked")};
  const std::wstring str_inner{ntfscompact_test::temp_sub(tmp, L"marked\\inner")};
  const std::wstring str_winsxs{ntfscompact_test::temp_sub(tmp, L"WinSxS")};
  REQUIRE(ntfscompact_test::run_compact(L"/c \"" + str_marked + L"\"") == 0);
  REQUIRE(ntfscompact_test::run_compact(L"/c \"" + str_inner + L"\"") == 0);
  REQUIRE(ntfscompact_test::run_compact(L"/c \"" + str_winsxs + L"\"") == 0);
  REQUIRE(is_marked_compressed(str_marked));
  REQUIRE(is_marked_compressed(str_inner));
  REQUIRE(is_marked_compressed(str_winsxs));

  SUBCASE("遍历：仅收集带标记的普通目录，reparse 与 WinSxS 不收") {
    ScanOutcome so_scan{};
    scan_paths({ntfscompact_test::temp_path(tmp)}, so_scan);
    CHECK(so_scan.vec_errors.empty());
    REQUIRE(so_scan.vec_flagged_dirs.size() == 2);
    bool b_has_marked{false};
    bool b_has_inner{false};
    for (const FlaggedDir& fd_flag : so_scan.vec_flagged_dirs) {
      CHECK(fd_flag.str_long_path.rfind(L"\\\\?\\", 0) == 0);
      b_has_marked = b_has_marked || fd_flag.str_display_path == str_marked;
      b_has_inner = b_has_inner || fd_flag.str_display_path == str_inner;
    }
    CHECK(b_has_marked);
    CHECK(b_has_inner);
  }
  SUBCASE("显式根目录本身带标记也收集") {
    ScanOutcome so_scan{};
    scan_paths({str_marked}, so_scan);
    CHECK(so_scan.vec_errors.empty());
    REQUIRE(so_scan.vec_flagged_dirs.size() == 2);
    bool b_has_root{false};
    for (const FlaggedDir& fd_flag : so_scan.vec_flagged_dirs) {
      b_has_root = b_has_root || fd_flag.str_display_path == str_marked;
    }
    CHECK(b_has_root);
  }
  SUBCASE("无标记目录零收集") {
    ScanOutcome so_scan{};
    scan_paths({ntfscompact_test::temp_sub(tmp, L"plain")}, so_scan);
    CHECK(so_scan.vec_errors.empty());
    CHECK(so_scan.vec_flagged_dirs.empty());
  }
}
