#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "interrupt.hpp"

namespace {

using ntfscompact::ForceExitFn;
using ntfscompact::InterruptFlags;
using ntfscompact::InterruptPoller;
using ntfscompact::SleepFn;
using ntfscompact::deliver_console_event;
using ntfscompact::deliver_system_stop_event;
using ntfscompact::interrupt_requested;
using ntfscompact::mark_wrapup_complete;
using ntfscompact::observe_interrupt;

}

TEST_CASE("interrupt_requested：未接线轮询器恒不中断，接线轮询器透传判定") {
  const InterruptPoller fn_none{};
  CHECK_FALSE(interrupt_requested(fn_none));

  bool b_asked{false};
  const InterruptPoller fn_wired{[&b_asked]() {
    b_asked = true;
    return true;
  }};
  CHECK(interrupt_requested(fn_wired));
  CHECK(b_asked);
}

TEST_CASE("deliver_console_event：首事件置位不强制退出，未收尾期再事件仍只置位") {
  InterruptFlags fl_flags{};
  int i4_force_calls{0};
  const ForceExitFn fn_force{[&i4_force_calls]() { ++i4_force_calls; }};

  CHECK(deliver_console_event(fl_flags, fn_force) == TRUE);
  CHECK(fl_flags.b_requested.load());
  CHECK(i4_force_calls == 0);

  CHECK(deliver_console_event(fl_flags, fn_force) == TRUE);
  CHECK(fl_flags.b_requested.load());
  CHECK(i4_force_calls == 0);
}

TEST_CASE("observe_interrupt：未请求返回假不置收尾，请求返回真并置收尾") {
  InterruptFlags fl_flags{};
  CHECK_FALSE(observe_interrupt(fl_flags));
  CHECK_FALSE(fl_flags.b_finishing.load());

  fl_flags.b_requested.store(true);
  CHECK(observe_interrupt(fl_flags));
  CHECK(fl_flags.b_finishing.load());
}

TEST_CASE("两段式协议：收尾期再事件走强制退出缝且保留判定") {
  InterruptFlags fl_flags{};
  int i4_force_calls{0};
  const ForceExitFn fn_force{[&i4_force_calls]() { ++i4_force_calls; }};

  CHECK(deliver_console_event(fl_flags, fn_force) == TRUE);
  CHECK(observe_interrupt(fl_flags));
  CHECK(deliver_console_event(fl_flags, fn_force) == TRUE);
  CHECK(i4_force_calls == 1);
  CHECK(deliver_console_event(fl_flags, fn_force) == TRUE);
  CHECK(i4_force_calls == 2);
}

TEST_CASE("中断退出码常量为 3") {
  CHECK(ntfscompact::kExitInterrupted == 3);
}

TEST_CASE("mark_wrapup_complete：置位收尾完成标志") {
  InterruptFlags fl_flags{};
  CHECK_FALSE(fl_flags.b_wrapup_complete.load());
  mark_wrapup_complete(fl_flags);
  CHECK(fl_flags.b_wrapup_complete.load());
}

TEST_CASE("deliver_system_stop_event：系统事件置位请求，预算内收尾完成后不强制退出直接返回") {
  InterruptFlags fl_flags{};
  int i4_force_calls{0};
  const ForceExitFn fn_force{[&i4_force_calls]() { ++i4_force_calls; }};
  unsigned int u4_sleep_calls{0};
  const SleepFn fn_sleep{[&fl_flags, &u4_sleep_calls](unsigned int) {
    ++u4_sleep_calls;
    mark_wrapup_complete(fl_flags);
  }};

  CHECK(deliver_system_stop_event(fl_flags, fn_force, fn_sleep, 4000, 50) ==
        TRUE);
  CHECK(fl_flags.b_requested.load());
  CHECK(fl_flags.b_wrapup_complete.load());
  CHECK(i4_force_calls == 0);
  CHECK(u4_sleep_calls == 1);
}

TEST_CASE("deliver_system_stop_event：预算耗尽收尾未完成——注入强制退出恰一次") {
  InterruptFlags fl_flags{};
  int i4_force_calls{0};
  const ForceExitFn fn_force{[&i4_force_calls]() { ++i4_force_calls; }};
  unsigned int u4_sleep_calls{0};
  const SleepFn fn_sleep{
      [&u4_sleep_calls](unsigned int) { ++u4_sleep_calls; }};

  CHECK(deliver_system_stop_event(fl_flags, fn_force, fn_sleep, 200, 50) ==
        TRUE);
  CHECK(i4_force_calls == 1);
  CHECK(u4_sleep_calls == 4);
  CHECK_FALSE(fl_flags.b_wrapup_complete.load());
}

TEST_CASE("deliver_system_stop_event：收尾已完成——不再等待不强制退出") {
  InterruptFlags fl_flags{};
  mark_wrapup_complete(fl_flags);
  int i4_force_calls{0};
  const ForceExitFn fn_force{[&i4_force_calls]() { ++i4_force_calls; }};
  unsigned int u4_sleep_calls{0};
  const SleepFn fn_sleep{
      [&u4_sleep_calls](unsigned int) { ++u4_sleep_calls; }};

  CHECK(deliver_system_stop_event(fl_flags, fn_force, fn_sleep, 4000, 50) ==
        TRUE);
  CHECK(i4_force_calls == 0);
  CHECK(u4_sleep_calls == 0);
}

TEST_CASE("deliver_system_stop_event：等待期出现收尾期再事件——立即强制退出恰一次") {
  InterruptFlags fl_flags{};
  int i4_force_calls{0};
  const ForceExitFn fn_force{[&i4_force_calls]() { ++i4_force_calls; }};
  unsigned int u4_sleep_calls{0};
  const SleepFn fn_sleep{[&fl_flags, &u4_sleep_calls](unsigned int) {
    ++u4_sleep_calls;
    fl_flags.b_finishing.store(true);
  }};

  CHECK(deliver_system_stop_event(fl_flags, fn_force, fn_sleep, 4000, 50) ==
        TRUE);
  CHECK(i4_force_calls == 1);
  CHECK(u4_sleep_calls == 1);
}

TEST_CASE("deliver_system_stop_event：入口即收尾期再事件——强制退出不等待") {
  InterruptFlags fl_flags{};
  fl_flags.b_finishing.store(true);
  int i4_force_calls{0};
  const ForceExitFn fn_force{[&i4_force_calls]() { ++i4_force_calls; }};
  unsigned int u4_sleep_calls{0};
  const SleepFn fn_sleep{
      [&u4_sleep_calls](unsigned int) { ++u4_sleep_calls; }};

  CHECK(deliver_system_stop_event(fl_flags, fn_force, fn_sleep, 4000, 50) ==
        TRUE);
  CHECK(i4_force_calls == 1);
  CHECK(u4_sleep_calls == 0);
}
