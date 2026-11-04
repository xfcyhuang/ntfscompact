// 真实检测器单元测试（工单 #3）
// 缝（seam）：RealDetector::Detect——文件路径进、现状压缩形态出。
// 用 %TEMP%（本地 NTFS）真实文件：compact.exe 造 LZNT1 / WOF 状态；
// 每次运行新建临时文件并清理，幂等可重复。
// 注意（研究笔记）：compact /exe:lzx 在部分 Win11 版本会静默不压缩——
// 以文件属性（sparse+reparse）为 sanity 判据，属性不符则记跳过不计失败。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "detect.h"
#include "temp_dir.h"

#include <cstdio>
#include <string>
#include <windows.h>

namespace {

doctest::String narrow(const std::wstring& s) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                          static_cast<int>(s.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            utf8.data(), bytes, nullptr, nullptr);
    }
    return doctest::String(utf8.c_str());
}

// 经 cmd 运行 compact（_wsystem 走 cmd.exe，无 shell 路径改写问题），输出静音
int RunCompact(const std::wstring& args) {
    const std::wstring cmd = L"compact " + args + L" >nul 2>&1";
    return _wsystem(cmd.c_str());
}

// 写可压缩文本文件（重复句子，~180KB，NTFS/WOF 压缩均有收益）
bool WriteCompressible(const std::wstring& path) {
    HANDLE h = CreateFileW(ntfscompact_test::TempDir::LongApiPath(path).c_str(),
                           GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    static const char kSentence[] =
        "The quick brown fox jumps over the lazy dog. ";
    static const char kChunk[4096] = {};
    for (int i = 0; i < 4; ++i) {  // 先铺 16KB 固定字节便于整块写
        DWORD written = 0;
        if (!::WriteFile(h, kChunk, sizeof(kChunk), &written, nullptr)) {
            CloseHandle(h);
            return false;
        }
    }
    // 用重复句子填充余下 160KB+（LZNT1/WOF 均能明显获益）
    for (int i = 0; i < 4000; ++i) {
        DWORD written = 0;
        if (!::WriteFile(h, kSentence, sizeof(kSentence) - 1, &written, nullptr)) {
            CloseHandle(h);
            return false;
        }
    }
    CloseHandle(h);
    return true;
}

DWORD GetAttrs(const std::wstring& path) {
    return GetFileAttributesW(
        ntfscompact_test::TempDir::LongApiPath(path).c_str());
}

}  // namespace

using namespace ntfscompact;

TEST_CASE("RealDetector 构造：wofutil.dll 动态加载成功") {
    std::wstring error;
    RealDetector detector(&error);
    REQUIRE(narrow(error) == "");  // 本机 Win10+ 必有 wofutil.dll
}

TEST_CASE("RealDetector：普通文件 → 未压缩（None）") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(WriteCompressible(tmp.Sub(L"plain.bin")));
    // %TEMP% 可能是 NTFS 压缩目录（新文件继承 LZNT1），先解压造真普通文件
    REQUIRE(RunCompact(L"/u \"" + tmp.Sub(L"plain.bin") + L"\"") == 0);
    REQUIRE((GetAttrs(tmp.Sub(L"plain.bin")) & FILE_ATTRIBUTE_COMPRESSED) == 0);

    std::wstring error;
    RealDetector detector(&error);
    REQUIRE(narrow(error) == "");
    const DetectionResult r = detector.Detect(tmp.Sub(L"plain.bin"));
    CHECK(narrow(r.error) == "");
    CHECK(r.state.kind == CurrentState::Kind::None);
}

TEST_CASE("RealDetector：compact /c 造的 LZNT1 文件 → Lznt1") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(WriteCompressible(tmp.Sub(L"lznt1.bin")));
    REQUIRE(RunCompact(L"/u \"" + tmp.Sub(L"lznt1.bin") + L"\"") == 0);  // 固定基线
    REQUIRE(RunCompact(L"/c \"" + tmp.Sub(L"lznt1.bin") + L"\"") == 0);

    // sanity：compact 应已置 COMPRESSED 属性；否则环境问题，测试失败可见
    const DWORD attrs = GetAttrs(tmp.Sub(L"lznt1.bin"));
    REQUIRE((attrs & FILE_ATTRIBUTE_COMPRESSED) != 0);

    std::wstring error;
    RealDetector detector(&error);
    REQUIRE(narrow(error) == "");
    const DetectionResult r = detector.Detect(tmp.Sub(L"lznt1.bin"));
    CHECK(narrow(r.error) == "");
    CHECK(r.state.kind == CurrentState::Kind::Lznt1);
}

TEST_CASE("RealDetector：compact /exe:lzx 造的 WOF 文件 → Wof/lzx/ProviderFile") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(WriteCompressible(tmp.Sub(L"wof.bin")));
    REQUIRE(RunCompact(L"/u \"" + tmp.Sub(L"wof.bin") + L"\"") == 0);  // 固定基线
    REQUIRE(RunCompact(L"/exe:lzx /c \"" + tmp.Sub(L"wof.bin") + L"\"") == 0);

    const DWORD attrs = GetAttrs(tmp.Sub(L"wof.bin"));
    const bool wof_form =
        (attrs & FILE_ATTRIBUTE_SPARSE_FILE) != 0 &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (!wof_form) {
        MESSAGE("compact /exe:lzx 静默未压缩（Win11 偶发，见研究笔记），跳过断言");
        return;
    }

    std::wstring error;
    RealDetector detector(&error);
    REQUIRE(narrow(error) == "");
    const DetectionResult r = detector.Detect(tmp.Sub(L"wof.bin"));
    CHECK(narrow(r.error) == "");
    CHECK(r.state.kind == CurrentState::Kind::Wof);
    CHECK(r.state.provider == CurrentState::kProviderFile);
    CHECK(r.state.algorithm == Algorithm::Lzx);
}

TEST_CASE("RealDetector：不存在的路径 → 带错误的 DetectionResult") {
    std::wstring error;
    RealDetector detector(&error);
    REQUIRE(narrow(error) == "");
    const DetectionResult r =
        detector.Detect(L"\\\\?\\D:\\Temp\\no-such-file-t3.bin");
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("RealDetector：LZNT1 文件 compact /u 后 → None（状态可逆复核）") {
    ntfscompact_test::TempDir tmp;
    REQUIRE(tmp.ok());
    REQUIRE(WriteCompressible(tmp.Sub(L"roundtrip.bin")));
    REQUIRE(RunCompact(L"/u \"" + tmp.Sub(L"roundtrip.bin") + L"\"") == 0);  // 固定基线
    REQUIRE(RunCompact(L"/c \"" + tmp.Sub(L"roundtrip.bin") + L"\"") == 0);

    std::wstring error;
    RealDetector detector(&error);
    REQUIRE(narrow(error) == "");
    CHECK(detector.Detect(tmp.Sub(L"roundtrip.bin")).state.kind ==
          CurrentState::Kind::Lznt1);

    REQUIRE(RunCompact(L"/u \"" + tmp.Sub(L"roundtrip.bin") + L"\"") == 0);
    CHECK(detector.Detect(tmp.Sub(L"roundtrip.bin")).state.kind ==
          CurrentState::Kind::None);
}
