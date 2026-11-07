#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "interrupt.hpp"

namespace {

using ntfscompact::ForceExitFn;
using ntfscompact::InterruptFlags;
using ntfscompact::InterruptPoller;
using ntfscompact::deliver_console_event;
using ntfscompact::interrupt_requested;
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
