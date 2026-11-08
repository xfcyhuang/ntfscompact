#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "cli.hpp"

namespace {

doctest::String narrow(const std::wstring& str_wide) {
  std::string str_out{};
  str_out.reserve(str_wide.size());
  for (wchar_t ch_wide : str_wide) {
    str_out.push_back(ch_wide < 128 ? static_cast<char>(ch_wide) : '?');
  }
  return doctest::String(str_out.c_str());
}

}

using ntfscompact::Algorithm;
using ntfscompact::parse_args;

TEST_CASE("路径列表：默认算法 xpress8k，开关全关") {
  const auto pr_result{parse_args({L"C:\\data", L"D:\\日志.txt"})};
  REQUIRE(pr_result.b_ok);
  REQUIRE(pr_result.opt_options.vec_paths.size() == 2);
  CHECK(narrow(pr_result.opt_options.vec_paths[0]) == "C:\\data");
  CHECK(narrow(pr_result.opt_options.vec_paths[1]) == "D:\\??.txt");
  CHECK(pr_result.opt_options.e_algorithm == Algorithm::kXpress8k);
  CHECK_FALSE(pr_result.opt_options.b_uncompress);
  CHECK_FALSE(pr_result.opt_options.b_dry_run);
  CHECK_FALSE(pr_result.opt_options.b_quiet);
  CHECK_FALSE(pr_result.opt_options.b_verbose);
}

TEST_CASE("-a 接受四个指定算法值") {
  SUBCASE("xpress4k") {
    const auto pr_result{parse_args({L"-a", L"xpress4k", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.e_algorithm == Algorithm::kXpress4k);
  }
  SUBCASE("xpress8k") {
    const auto pr_result{parse_args({L"-a", L"xpress8k", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.e_algorithm == Algorithm::kXpress8k);
  }
  SUBCASE("xpress16k") {
    const auto pr_result{parse_args({L"-a", L"xpress16k", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.e_algorithm == Algorithm::kXpress16k);
  }
  SUBCASE("lzx") {
    const auto pr_result{parse_args({L"-a", L"lzx", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.e_algorithm == Algorithm::kLzx);
  }
}

TEST_CASE("-a 非法值或缺值为用法错误（退出码 2 语义）") {
  SUBCASE("非法值 zip") {
    const auto pr_result{parse_args({L"-a", L"zip", L"p"})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
  SUBCASE("非法值大小写不符 XPRESS4K") {
    const auto pr_result{parse_args({L"-a", L"XPRESS4K", L"p"})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
  SUBCASE("-a 在末尾缺值") {
    const auto pr_result{parse_args({L"p", L"-a"})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
}

TEST_CASE("--force-close-all 默认关，显式传入置位") {
  SUBCASE("默认关") {
    const auto pr_result{parse_args({L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK_FALSE(pr_result.opt_options.b_force_close_all);
  }
  SUBCASE("显式置位") {
    const auto pr_result{parse_args({L"--force-close-all", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.b_force_close_all);
  }
  SUBCASE("与 -n 组合") {
    const auto pr_result{parse_args({L"-n", L"--force-close-all", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.b_dry_run);
    CHECK(pr_result.opt_options.b_force_close_all);
  }
}

TEST_CASE("开关 -u -n -q -v 各自置位") {
  SUBCASE("-u 解压") {
    const auto pr_result{parse_args({L"-u", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.b_uncompress);
    CHECK_FALSE(pr_result.opt_options.b_dry_run);
    CHECK_FALSE(pr_result.opt_options.b_quiet);
    CHECK_FALSE(pr_result.opt_options.b_verbose);
  }
  SUBCASE("-n 干跑") {
    const auto pr_result{parse_args({L"-n", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.b_dry_run);
    CHECK_FALSE(pr_result.opt_options.b_uncompress);
  }
  SUBCASE("-q 静默") {
    const auto pr_result{parse_args({L"-q", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.b_quiet);
    CHECK_FALSE(pr_result.opt_options.b_uncompress);
  }
  SUBCASE("-v 详细输出") {
    const auto pr_result{parse_args({L"-v", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.b_verbose);
    CHECK_FALSE(pr_result.opt_options.b_uncompress);
  }
}

TEST_CASE("开关组合混合解析（验收样例 -a lzx -n -v p1 p2）") {
  SUBCASE("选项在前，多路径在后") {
    const auto pr_result{
        parse_args({L"-a", L"lzx", L"-n", L"-v", L"path1", L"path2"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.e_algorithm == Algorithm::kLzx);
    CHECK(pr_result.opt_options.b_dry_run);
    CHECK(pr_result.opt_options.b_verbose);
    CHECK_FALSE(pr_result.opt_options.b_quiet);
    CHECK_FALSE(pr_result.opt_options.b_uncompress);
    REQUIRE(pr_result.opt_options.vec_paths.size() == 2);
    CHECK(narrow(pr_result.opt_options.vec_paths[0]) == "path1");
    CHECK(narrow(pr_result.opt_options.vec_paths[1]) == "path2");
  }
  SUBCASE("开关穿插在路径之间") {
    const auto pr_result{parse_args({L"p1", L"-q", L"-a", L"xpress4k", L"p2"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.b_quiet);
    CHECK(pr_result.opt_options.e_algorithm == Algorithm::kXpress4k);
    REQUIRE(pr_result.opt_options.vec_paths.size() == 2);
    CHECK(narrow(pr_result.opt_options.vec_paths[0]) == "p1");
    CHECK(narrow(pr_result.opt_options.vec_paths[1]) == "p2");
  }
}

TEST_CASE("无路径参数与未知选项为用法错误（退出码 2 语义）") {
  SUBCASE("无任何参数") {
    const auto pr_result{parse_args({})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
  SUBCASE("只有开关没有路径") {
    const auto pr_result{parse_args({L"-v", L"-q"})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
  SUBCASE("未知短选项") {
    const auto pr_result{parse_args({L"-x", L"p"})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
  SUBCASE("未知长选项") {
    const auto pr_result{parse_args({L"--bogus", L"p"})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
}

TEST_CASE("-u 与 -a 同现为用法错误（工单 #5：解压模式不指定算法）") {
  SUBCASE("-u 在前 -a 在后") {
    const auto pr_result{parse_args({L"-u", L"-a", L"lzx", L"p"})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
  SUBCASE("-a 在前 -u 在后") {
    const auto pr_result{parse_args({L"-a", L"lzx", L"-u", L"p"})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
  SUBCASE("-u -a lzx 无路径也报互斥（优先于缺路径）") {
    const auto pr_result{parse_args({L"-u", L"-a", L"lzx"})};
    CHECK_FALSE(pr_result.b_ok);
    CHECK_FALSE(pr_result.str_error.empty());
  }
  SUBCASE("仅 -u 无 -a 仍合法") {
    const auto pr_result{parse_args({L"-u", L"p"})};
    REQUIRE(pr_result.b_ok);
    CHECK(pr_result.opt_options.b_uncompress);
  }
}

TEST_CASE("--help 与 --version 请求") {
  SUBCASE("--help 无路径也合法") {
    const auto pr_result{parse_args({L"--help"})};
    CHECK(pr_result.b_ok);
    CHECK(pr_result.b_want_help);
    CHECK_FALSE(pr_result.b_want_version);
  }
  SUBCASE("--version 无路径也合法") {
    const auto pr_result{parse_args({L"--version"})};
    CHECK(pr_result.b_ok);
    CHECK(pr_result.b_want_version);
    CHECK_FALSE(pr_result.b_want_help);
  }
}

TEST_CASE("用法文本覆盖全部选项与算法取值") {
  const std::wstring str_usage{ntfscompact::usage_text()};
  CHECK(str_usage.find(L"xpress4k") != std::wstring::npos);
  CHECK(str_usage.find(L"xpress8k") != std::wstring::npos);
  CHECK(str_usage.find(L"xpress16k") != std::wstring::npos);
  CHECK(str_usage.find(L"lzx") != std::wstring::npos);
  CHECK(str_usage.find(L"-a") != std::wstring::npos);
  CHECK(str_usage.find(L"-u") != std::wstring::npos);
  CHECK(str_usage.find(L"-n") != std::wstring::npos);
  CHECK(str_usage.find(L"-q") != std::wstring::npos);
  CHECK(str_usage.find(L"-v") != std::wstring::npos);
  CHECK(str_usage.find(L"--force-close-all") != std::wstring::npos);
  CHECK(str_usage.find(L"--help") != std::wstring::npos);
  CHECK(str_usage.find(L"--version") != std::wstring::npos);
}

TEST_CASE("用法文本说明中断两段式协议与退出码 3 及内核卡死不可救") {
  const std::wstring str_usage{ntfscompact::usage_text()};
  CHECK(str_usage.find(L"Ctrl+C") != std::wstring::npos);
  CHECK(str_usage.find(L"当前项边界停机") != std::wstring::npos);
  CHECK(str_usage.find(L"升级链全程") != std::wstring::npos);
  CHECK(str_usage.find(L"部分汇总") != std::wstring::npos);
  CHECK(str_usage.find(L"再次事件立即强制退出") != std::wstring::npos);
  CHECK(str_usage.find(L"约 4s 预算内等待收尾完成") != std::wstring::npos);
  CHECK(str_usage.find(L"无法中断，无解") != std::wstring::npos);
  CHECK(str_usage.find(L"3 已被 Ctrl+C 中断") != std::wstring::npos);
}

