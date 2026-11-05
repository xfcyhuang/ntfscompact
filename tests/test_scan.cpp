// 路径展开与递归遍历单元测试（工单 #3）
// 缝（seam）：scan_paths——路径参数（文件/目录/通配符/长路径）进、
// 候选文件 + 跳过记录 + 错误记录出。真实临时目录，只读不改。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "scan.hpp"
#include "temp_dir.hpp"

#include <windows.h>

namespace {

std::wstring Basename(const std::wstring& path) {
    const std::size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::vector<std::wstring> Basenames(const ntfscompact::ScanOutcome& out) {
    std::vector<std::wstring> names;
    for (const auto& item : out.vec_items) names.push_back(Basename(item.str_display_path));
    std::sort(names.begin(), names.end());
    return names;
}

// 造目录 junction（不需要管理员权限）
bool MakeJunction(const std::wstring& link, const std::wstring& target) {
    const std::wstring cmd =
        L"cmd /c mklink /J \"" + link + L"\" \"" + target + L"\"";
    return _wsystem(cmd.c_str()) == 0;
}

}  // namespace

using namespace ntfscompact;

TEST_CASE("单文件路径参数：收录且大小正确") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"a.bin", 8192));

    ScanOutcome out;
    scan_paths({tmp.Sub(L"a.bin")}, out);
    CHECK(out.vec_errors.empty());
    REQUIRE(out.vec_items.size() == 1);
    CHECK(out.vec_items[0].u8_size == 8192);
    CHECK(out.vec_items[0].str_display_path == tmp.Sub(L"a.bin"));
    CHECK(out.vec_items[0].str_long_path.rfind(L"\\\\?\\", 0) == 0);  // 长路径形式
}

TEST_CASE("目录递归：含子目录全部文件，只收文件不收目录") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"top.txt", 8192));
    REQUIRE(tmp.MakeDir(L"sub"));
    REQUIRE(tmp.WriteFile(L"sub\\inner.dat", 8192));
    REQUIRE(tmp.MakeDir(L"sub\\deep"));
    REQUIRE(tmp.WriteFile(L"sub\\deep\\leaf.txt", 8192));

    ScanOutcome out;
    scan_paths({tmp.path()}, out);
    CHECK(out.vec_errors.empty());
    CHECK(out.vec_skipped.empty());
    const auto names = Basenames(out);
    std::vector<std::wstring> expected = {L"top.txt", L"inner.dat", L"leaf.txt"};
    std::sort(expected.begin(), expected.end());
    CHECK(names == expected);
}

TEST_CASE("不进 junction：防环且目标文件不重复计入") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"real.txt", 8192));
    REQUIRE(tmp.MakeDir(L"loop"));           // loop\inside 指回 tmp 自身
    REQUIRE(MakeJunction(tmp.Sub(L"loop\\inside"), tmp.path()));

    ScanOutcome out;
    scan_paths({tmp.path()}, out);
    CHECK(out.vec_errors.empty());
    const auto names = Basenames(out);
    REQUIRE(names.size() == 1);  // real.txt 只出现一次，未因环重复
    CHECK(names[0] == L"real.txt");
    // junction 目录本身被记为跳过且原因点名符号链接/junction
    REQUIRE(out.vec_skipped.size() == 1);
    CHECK(out.vec_skipped[0].str_display_path.find(L"loop\\inside") != std::wstring::npos);
    CHECK(out.vec_skipped[0].str_reason.find(L"junction") != std::wstring::npos);
}

TEST_CASE("junction 作为显式路径参数：跳过不进入，不算错误") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"real.txt", 8192));
    REQUIRE(MakeJunction(tmp.Sub(L"j"), tmp.path()));

    ScanOutcome out;
    scan_paths({tmp.Sub(L"j")}, out);
    CHECK(out.vec_errors.empty());
    CHECK(out.vec_items.empty());
    REQUIRE(out.vec_skipped.size() == 1);
    CHECK(out.vec_skipped[0].str_reason.find(L"junction") != std::wstring::npos);
}

TEST_CASE("通配符：仅匹配该层文件，不递归子目录") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"a.txt", 8192));
    REQUIRE(tmp.WriteFile(L"b.dat", 8192));
    REQUIRE(tmp.MakeDir(L"sub"));
    REQUIRE(tmp.WriteFile(L"sub\\c.txt", 8192));

    ScanOutcome out;
    scan_paths({tmp.Sub(L"*.txt")}, out);
    CHECK(out.vec_errors.empty());
    const auto names = Basenames(out);
    std::vector<std::wstring> expected = {L"a.txt"};
    CHECK(names == expected);  // b.dat 不匹配；sub\c.txt 在子目录不匹配
}

TEST_CASE("路径不存在：记入错误") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());

    ScanOutcome out;
    scan_paths({tmp.Sub(L"no-such-file.bin")}, out);
    CHECK(out.vec_items.empty());
    REQUIRE(out.vec_errors.size() == 1);
    CHECK(out.vec_errors[0].str_display_path.find(L"no-such-file.bin") !=
          std::wstring::npos);
    CHECK(out.vec_errors[0].str_message.find(L"无法访问") != std::wstring::npos);
}

TEST_CASE("长路径：不带 \\\\?\\ 前缀的超长路径参数可用") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    // 逐层造深目录，总长 > 260 字符
    std::wstring deep;
    for (int i = 0; i < 18; ++i) deep += L"\\level-0123456789";
    REQUIRE(tmp.MakeDir(deep.substr(1)));
    REQUIRE(tmp.WriteFile(deep.substr(1) + L"\\deep.txt", 8192));

    const std::wstring unprefixed = tmp.Sub(deep.substr(1));  // 无 \\?\ 前缀
    REQUIRE(unprefixed.size() > 260);

    ScanOutcome out;
    scan_paths({unprefixed}, out);          // 目录形式
    CHECK(out.vec_errors.empty());
    CHECK(Basenames(out) == std::vector<std::wstring>{L"deep.txt"});

    ScanOutcome out2;
    scan_paths({unprefixed + L"\\deep.txt"}, out2);  // 文件形式
    CHECK(out2.vec_errors.empty());
    REQUIRE(out2.vec_items.size() == 1);
    CHECK(out2.vec_items[0].u8_size == 8192);
}

// ---- 过滤判定（check_file_candidate 纯函数，合成属性覆盖无法在测试中真实构造的场景）----

namespace {

constexpr DWORD kReparseAttr = FILE_ATTRIBUTE_REPARSE_POINT;
constexpr DWORD kReparseTagSymlink = 0xA000000C;  // IO_REPARSE_TAG_SYMLINK
constexpr DWORD kReparseTagWof = 0x80000017;      // IO_REPARSE_TAG_WOF
constexpr DWORD kReparseTagMountPoint = 0xA0000003;

FileMetadata Meta(const std::wstring& path, std::uint64_t size, DWORD attrs = 0,
                  DWORD tag = 0) {
    FileMetadata m;
    m.str_display_path = path;
    m.u8_size = size;
    m.u4_attributes = attrs;
    m.u4_reparse_tag = tag;
    return m;
}

}  // namespace

TEST_CASE("过滤判定：常规大文件收录") {
    std::wstring reason;
    CHECK(check_file_candidate(Meta(L"C:\\d\\a.bin", 8192), &reason));
    CHECK(check_file_candidate(Meta(L"C:\\d\\b.txt", 4097), &reason));  // 边界外
}

TEST_CASE("过滤判定：系统关键文件黑名单（大小写不敏感）") {
    std::wstring reason;
    SUBCASE("pagefile.sys") {
        CHECK_FALSE(check_file_candidate(Meta(L"C:\\pagefile.sys", 1u << 30), &reason));
        CHECK(reason.find(L"系统关键") != std::wstring::npos);
    }
    SUBCASE("HIBERFIL.SYS 大写") {
        CHECK_FALSE(check_file_candidate(Meta(L"C:\\HIBERFIL.SYS", 1u << 30), &reason));
    }
    SUBCASE("swapfile.sys 在子目录") {
        CHECK_FALSE(
            check_file_candidate(Meta(L"C:\\d\\SwapFile.Sys", 1u << 20), &reason));
    }
    SUBCASE("同名前缀不算（pagefile1.sys）") {
        CHECK(check_file_candidate(Meta(L"C:\\pagefile1.sys", 8192), &reason));
    }
}

TEST_CASE("过滤判定：WinSxS 路径组件黑名单") {
    std::wstring reason;
    SUBCASE("组件精确匹配（大小写不敏感）") {
        CHECK_FALSE(check_file_candidate(
            Meta(L"C:\\Windows\\WinSxS\\amd64_microsoft.xml", 8192), &reason));
        CHECK(reason.find(L"WinSxS") != std::wstring::npos);
    }
    SUBCASE("winsxs 小写也命中") {
        CHECK_FALSE(
            check_file_candidate(Meta(L"C:\\winsxs\\f.dll", 8192), &reason));
    }
    SUBCASE("文件名本身叫 WinSxS 不误伤（按组件匹配）") {
        CHECK(check_file_candidate(Meta(L"C:\\d\\WinSxS", 8192), &reason));
    }
    SUBCASE("前缀相似不误伤（WinSxSBackup 目录名不同）") {
        CHECK(check_file_candidate(Meta(L"C:\\WinSxSBackup\\f.dll", 8192), &reason));
    }
}

TEST_CASE("过滤判定：EFS 加密文件跳过") {
    std::wstring reason;
    CHECK_FALSE(check_file_candidate(
        Meta(L"C:\\d\\secret.txt", 8192, FILE_ATTRIBUTE_ENCRYPTED), &reason));
    CHECK(reason.find(L"EFS") != std::wstring::npos);
}

TEST_CASE("过滤判定：非常规 reparse point 跳过，WOF reparse 放行") {
    std::wstring reason;
    SUBCASE("符号链接") {
        CHECK_FALSE(check_file_candidate(
            Meta(L"C:\\d\\link.txt", 8192, kReparseAttr, kReparseTagSymlink),
            &reason));
        CHECK(reason.find(L"reparse") != std::wstring::npos);
    }
    SUBCASE("junction 文件形态（mount point）") {
        CHECK_FALSE(check_file_candidate(
            Meta(L"C:\\d\\j", 8192, kReparseAttr, kReparseTagMountPoint), &reason));
    }
    SUBCASE("WOF reparse tag 放行（交现状检测判定）") {
        CHECK(check_file_candidate(
            Meta(L"C:\\d\\packed.bin", 8192, kReparseAttr, kReparseTagWof),
            &reason));
    }
}

TEST_CASE("过滤判定：≤4KB 跳过（4096 边界）") {
    std::wstring reason;
    CHECK_FALSE(check_file_candidate(Meta(L"C:\\d\\s.bin", 4096), &reason));
    CHECK(reason.find(L"4KB") != std::wstring::npos);
    CHECK(check_file_candidate(Meta(L"C:\\d\\s.bin", 4097), &reason));
}

TEST_CASE("端到端：黑名单与 ≤4KB 在遍历中生效且带原因") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(tmp.WriteFile(L"keep.bin", 8192));
    REQUIRE(tmp.WriteFile(L"small.bin", 100));
    REQUIRE(tmp.WriteFile(L"pagefile.sys", 1u << 20));
    REQUIRE(tmp.MakeDir(L"WinSxS"));
    REQUIRE(tmp.WriteFile(L"WinSxS\\inside.dll", 1u << 20));

    ScanOutcome out;
    scan_paths({tmp.path()}, out);
    CHECK(out.vec_errors.empty());
    CHECK(Basenames(out) == std::vector<std::wstring>{L"keep.bin"});

    REQUIRE(out.vec_skipped.size() == 3);
    bool saw_size = false, saw_critical = false, saw_winsxs = false;
    for (const auto& s : out.vec_skipped) {
        if (s.str_display_path.find(L"small.bin") != std::wstring::npos &&
            s.str_reason.find(L"4KB") != std::wstring::npos) {
            saw_size = true;
        }
        if (s.str_display_path.find(L"pagefile.sys") != std::wstring::npos &&
            s.str_reason.find(L"系统关键") != std::wstring::npos) {
            saw_critical = true;
        }
        // WinSxS 子树整树跳过：记录挂在该目录本身
        if (s.str_display_path.find(L"WinSxS") != std::wstring::npos &&
            s.str_reason.find(L"WinSxS") != std::wstring::npos) {
            saw_winsxs = true;
        }
    }
    CHECK(saw_size);
    CHECK(saw_critical);
    CHECK(saw_winsxs);
}
