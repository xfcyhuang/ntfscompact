// 命令行解析单元测试（工单 #2）
// 缝（seam）：ParseArgs 纯函数——argv 进、ParseResult 出，无 I/O。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "cli.hpp"

#include <string>
#include <vector>

namespace {

// 宽字符串转窄字符串，仅用于失败信息展示（测试路径含中文时显示近似）
doctest::String narrow(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return doctest::String(out.c_str());
}

}  // namespace

using ntfscompact::Algorithm;
using ntfscompact::ParseArgs;

TEST_CASE("路径列表：默认算法 xpress8k，开关全关") {
    const auto r = ParseArgs({L"C:\\data", L"D:\\日志.txt"});
    REQUIRE(r.ok);
    REQUIRE(r.options.paths.size() == 2);
    CHECK(narrow(r.options.paths[0]) == "C:\\data");
    CHECK(narrow(r.options.paths[1]) == "D:\\??.txt");
    CHECK(r.options.algorithm == Algorithm::Xpress8k);
    CHECK_FALSE(r.options.uncompress);
    CHECK_FALSE(r.options.dry_run);
    CHECK_FALSE(r.options.quiet);
    CHECK_FALSE(r.options.verbose);
}

TEST_CASE("-a 接受四个指定算法值") {
    SUBCASE("xpress4k") {
        const auto r = ParseArgs({L"-a", L"xpress4k", L"p"});
        REQUIRE(r.ok);
        CHECK(r.options.algorithm == Algorithm::Xpress4k);
    }
    SUBCASE("xpress8k") {
        const auto r = ParseArgs({L"-a", L"xpress8k", L"p"});
        REQUIRE(r.ok);
        CHECK(r.options.algorithm == Algorithm::Xpress8k);
    }
    SUBCASE("xpress16k") {
        const auto r = ParseArgs({L"-a", L"xpress16k", L"p"});
        REQUIRE(r.ok);
        CHECK(r.options.algorithm == Algorithm::Xpress16k);
    }
    SUBCASE("lzx") {
        const auto r = ParseArgs({L"-a", L"lzx", L"p"});
        REQUIRE(r.ok);
        CHECK(r.options.algorithm == Algorithm::Lzx);
    }
}

TEST_CASE("-a 非法值或缺值为用法错误（退出码 2 语义）") {
    SUBCASE("非法值 zip") {
        const auto r = ParseArgs({L"-a", L"zip", L"p"});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    SUBCASE("非法值大小写不符 XPRESS4K") {
        const auto r = ParseArgs({L"-a", L"XPRESS4K", L"p"});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    SUBCASE("-a 在末尾缺值") {
        const auto r = ParseArgs({L"p", L"-a"});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
}

TEST_CASE("开关 -u -n -q -v 各自置位") {
    SUBCASE("-u 解压") {
        const auto r = ParseArgs({L"-u", L"p"});
        REQUIRE(r.ok);
        CHECK(r.options.uncompress);
        CHECK_FALSE(r.options.dry_run);
        CHECK_FALSE(r.options.quiet);
        CHECK_FALSE(r.options.verbose);
    }
    SUBCASE("-n 干跑") {
        const auto r = ParseArgs({L"-n", L"p"});
        REQUIRE(r.ok);
        CHECK(r.options.dry_run);
        CHECK_FALSE(r.options.uncompress);
    }
    SUBCASE("-q 静默") {
        const auto r = ParseArgs({L"-q", L"p"});
        REQUIRE(r.ok);
        CHECK(r.options.quiet);
        CHECK_FALSE(r.options.uncompress);
    }
    SUBCASE("-v 详细") {
        const auto r = ParseArgs({L"-v", L"p"});
        REQUIRE(r.ok);
        CHECK(r.options.verbose);
        CHECK_FALSE(r.options.uncompress);
    }
}

TEST_CASE("开关组合混合解析（验收样例 -a lzx -n -v p1 p2）") {
    SUBCASE("选项在前，多路径在后") {
        const auto r = ParseArgs({L"-a", L"lzx", L"-n", L"-v", L"path1", L"path2"});
        REQUIRE(r.ok);
        CHECK(r.options.algorithm == Algorithm::Lzx);
        CHECK(r.options.dry_run);
        CHECK(r.options.verbose);
        CHECK_FALSE(r.options.quiet);
        CHECK_FALSE(r.options.uncompress);
        REQUIRE(r.options.paths.size() == 2);
        CHECK(narrow(r.options.paths[0]) == "path1");
        CHECK(narrow(r.options.paths[1]) == "path2");
    }
    SUBCASE("开关穿插在路径之间") {
        const auto r = ParseArgs({L"p1", L"-q", L"-a", L"xpress4k", L"p2"});
        REQUIRE(r.ok);
        CHECK(r.options.quiet);
        CHECK(r.options.algorithm == Algorithm::Xpress4k);
        REQUIRE(r.options.paths.size() == 2);
        CHECK(narrow(r.options.paths[0]) == "p1");
        CHECK(narrow(r.options.paths[1]) == "p2");
    }
}

TEST_CASE("无路径参数与未知选项为用法错误（退出码 2 语义）") {
    SUBCASE("无任何参数") {
        const auto r = ParseArgs({});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    SUBCASE("只有开关没有路径") {
        const auto r = ParseArgs({L"-v", L"-q"});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    SUBCASE("未知短选项") {
        const auto r = ParseArgs({L"-x", L"p"});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    SUBCASE("未知长选项") {
        const auto r = ParseArgs({L"--bogus", L"p"});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
}

TEST_CASE("-u 与 -a 同现为用法错误（工单 #5：解压模式不指定算法）") {
    SUBCASE("-u 在前 -a 在后") {
        const auto r = ParseArgs({L"-u", L"-a", L"lzx", L"p"});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    SUBCASE("-a 在前 -u 在后") {
        const auto r = ParseArgs({L"-a", L"lzx", L"-u", L"p"});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    SUBCASE("-u -a lzx 无路径也报互斥（优先于缺路径）") {
        const auto r = ParseArgs({L"-u", L"-a", L"lzx"});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    SUBCASE("仅 -u 无 -a 仍合法") {
        const auto r = ParseArgs({L"-u", L"p"});
        REQUIRE(r.ok);
        CHECK(r.options.uncompress);
    }
}

TEST_CASE("--help 与 --version 请求") {
    SUBCASE("--help 无路径也合法") {
        const auto r = ParseArgs({L"--help"});
        CHECK(r.ok);
        CHECK(r.want_help);
        CHECK_FALSE(r.want_version);
    }
    SUBCASE("--version 无路径也合法") {
        const auto r = ParseArgs({L"--version"});
        CHECK(r.ok);
        CHECK(r.want_version);
        CHECK_FALSE(r.want_help);
    }
}

TEST_CASE("用法文本覆盖全部选项与算法取值") {
    const std::wstring usage = ntfscompact::UsageText();
    CHECK(usage.find(L"xpress4k") != std::wstring::npos);
    CHECK(usage.find(L"xpress8k") != std::wstring::npos);
    CHECK(usage.find(L"xpress16k") != std::wstring::npos);
    CHECK(usage.find(L"lzx") != std::wstring::npos);
    CHECK(usage.find(L"-a") != std::wstring::npos);
    CHECK(usage.find(L"-u") != std::wstring::npos);
    CHECK(usage.find(L"-n") != std::wstring::npos);
    CHECK(usage.find(L"-q") != std::wstring::npos);
    CHECK(usage.find(L"-v") != std::wstring::npos);
    CHECK(usage.find(L"--help") != std::wstring::npos);
    CHECK(usage.find(L"--version") != std::wstring::npos);
}
