#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dir_flag.hpp"
#include "temp_dir.hpp"
#include "util.hpp"

#include <windows.h>

namespace {

using ntfscompact::DirFlagFn;
using ntfscompact::clear_dir_flag;
using ntfscompact::make_real_dir_flag_clearer;
using ntfscompact_test::dir_is_marked;
using ntfscompact_test::mark_dir;
using ntfscompact_test::narrow;

}

TEST_CASE("make_real_dir_flag_clearer：工厂可用") {
  const std::expected<DirFlagFn, std::wstring> xe_fn{make_real_dir_flag_clearer()};
  if (!xe_fn.has_value()) {
    MESSAGE(narrow(xe_fn.error()).c_str());
  }
  REQUIRE(xe_fn.has_value());
}

TEST_CASE("clear_dir_flag：带标记目录经真实工厂清除，属性消失且新建文件不再自动压缩") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  const std::wstring str_dir{ntfscompact_test::temp_sub(tmp, L"marked")};
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"marked"));
  REQUIRE(mark_dir(str_dir));

  const std::expected<DirFlagFn, std::wstring> xe_fn{make_real_dir_flag_clearer()};
  REQUIRE(xe_fn.has_value());
  const DirFlagFn fn_clear{std::move(xe_fn).value()};
  const std::expected<void, std::wstring> xe_clear{
      fn_clear(ntfscompact_test::temp_long_api_path(str_dir))};
  if (!xe_clear.has_value()) {
    MESSAGE(narrow(xe_clear.error()).c_str());
  }
  REQUIRE(xe_clear.has_value());
  CHECK_FALSE(dir_is_marked(str_dir));

  REQUIRE(ntfscompact_test::temp_write_file(tmp, L"marked\\new.bin", 8192));
  CHECK_FALSE(dir_is_marked(ntfscompact_test::temp_sub(tmp, L"marked\\new.bin")));
}

TEST_CASE("clear_dir_flag：无标记目录幂等成功") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  const std::wstring str_dir{ntfscompact_test::temp_sub(tmp, L"plain")};
  REQUIRE(ntfscompact_test::temp_make_dir(tmp, L"plain"));
  REQUIRE_FALSE(dir_is_marked(str_dir));

  const std::expected<void, std::wstring> xe_clear{
      clear_dir_flag(ntfscompact_test::temp_long_api_path(str_dir))};
  if (!xe_clear.has_value()) {
    MESSAGE(narrow(xe_clear.error()).c_str());
  }
  REQUIRE(xe_clear.has_value());
  CHECK_FALSE(dir_is_marked(str_dir));
}

TEST_CASE("clear_dir_flag：不存在的目录返回错误") {
  ntfscompact_test::TempDir tmp{ntfscompact_test::make_temp_dir()};
  REQUIRE(tmp->b_ok);
  const std::expected<void, std::wstring> xe_clear{clear_dir_flag(
      ntfscompact_test::temp_long_api_path(
          ntfscompact_test::temp_sub(tmp, L"no-such-dir")))};
  CHECK_FALSE(xe_clear.has_value());
  if (!xe_clear.has_value()) {
    MESSAGE(narrow(xe_clear.error()).c_str());
  }
}
