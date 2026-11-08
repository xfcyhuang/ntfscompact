#pragma once

import std;

#include <windows.h>

namespace ntfscompact {

inline constexpr unsigned int kExitInterrupted{3};

using InterruptPoller = std::function<bool()>;
using ForceExitFn = std::function<void()>;
using SleepFn = std::function<void(unsigned int)>;
using WrapupSignalFn = std::function<void()>;

struct InterruptFlags {
  std::atomic<bool> b_requested{false};
  std::atomic<bool> b_finishing{false};
  std::atomic<bool> b_wrapup_complete{false};
};

struct InterruptWiring {
  InterruptPoller fn_poll{};
  WrapupSignalFn fn_wrapup_complete{};
};

inline bool interrupt_requested(const InterruptPoller& fn_poll) {
  return static_cast<bool>(fn_poll) && fn_poll();
}

inline BOOL deliver_console_event(InterruptFlags& fl_flags,
                                  const ForceExitFn& fn_force_exit) {
  if (fl_flags.b_finishing.load(std::memory_order_acquire)) {
    if (fn_force_exit) {
      fn_force_exit();
    }
    return TRUE;
  }
  fl_flags.b_requested.store(true, std::memory_order_release);
  return TRUE;
}

inline void mark_wrapup_complete(InterruptFlags& fl_flags) {
  fl_flags.b_wrapup_complete.store(true, std::memory_order_release);
}

inline BOOL await_wrapup_bounded(InterruptFlags& fl_flags,
                                 const ForceExitFn& fn_force_exit,
                                 const SleepFn& fn_sleep,
                                 unsigned int u4_budget_ms,
                                 unsigned int u4_step_ms) {
  for (unsigned int u4_waited{0}; u4_waited < u4_budget_ms;
       u4_waited += u4_step_ms) {
    if (fl_flags.b_finishing.load(std::memory_order_acquire)) {
      if (fn_force_exit) {
        fn_force_exit();
      }
      return TRUE;
    }
    if (fl_flags.b_wrapup_complete.load(std::memory_order_acquire)) {
      return TRUE;
    }
    if (fn_sleep) {
      fn_sleep(u4_step_ms);
    }
  }
  if (fn_force_exit) {
    fn_force_exit();
  }
  return TRUE;
}

inline BOOL deliver_system_stop_event(InterruptFlags& fl_flags,
                                      const ForceExitFn& fn_force_exit,
                                      const SleepFn& fn_sleep,
                                      unsigned int u4_budget_ms,
                                      unsigned int u4_step_ms) {
  fl_flags.b_requested.store(true, std::memory_order_release);
  return await_wrapup_bounded(fl_flags, fn_force_exit, fn_sleep, u4_budget_ms,
                              u4_step_ms);
}

inline bool observe_interrupt(InterruptFlags& fl_flags) {
  if (!fl_flags.b_requested.load(std::memory_order_acquire)) {
    return false;
  }
  fl_flags.b_finishing.store(true, std::memory_order_release);
  return true;
}

std::expected<InterruptWiring, std::wstring> install_interrupt_handler();

}
