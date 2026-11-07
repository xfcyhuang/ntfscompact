import std;

#include <windows.h>
#include <shellapi.h>

namespace {

struct HoldState {
  HANDLE h_file{INVALID_HANDLE_VALUE};
  bool b_stubborn{false};
};

HoldState g_state{};

LRESULT CALLBACK hold_window_procedure(HWND h_wnd, UINT u_msg, WPARAM w_param,
                                       LPARAM l_param) {
  switch (u_msg) {
    case WM_CLOSE:
      if (!g_state.b_stubborn) {
        DestroyWindow(h_wnd);
      }
      return 0;
    case WM_ENDSESSION:
      if (w_param != 0 && !g_state.b_stubborn) {
        DestroyWindow(h_wnd);
      }
      return 0;
    case WM_DESTROY:
      if (g_state.h_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_state.h_file);
        g_state.h_file = INVALID_HANDLE_VALUE;
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h_wnd, u_msg, w_param, l_param);
}

}

int WINAPI wWinMain(HINSTANCE h_instance, HINSTANCE, PWSTR, int) {
  int i_argc{0};
  wchar_t** arr_argv{CommandLineToArgvW(GetCommandLineW(), &i_argc)};
  if (arr_argv == nullptr || i_argc < 2) {
    return 2;
  }
  g_state.h_file = CreateFileW(arr_argv[1], GENERIC_READ, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
  if (g_state.h_file == INVALID_HANDLE_VALUE) {
    LocalFree(arr_argv);
    return 3;
  }
  if (i_argc >= 3 && std::wcscmp(arr_argv[2], L"--stubborn") == 0) {
    g_state.b_stubborn = true;
  }
  LocalFree(arr_argv);
  WNDCLASSEXW wc_hold{};
  wc_hold.cbSize = sizeof(wc_hold);
  wc_hold.lpfnWndProc = hold_window_procedure;
  wc_hold.hInstance = h_instance;
  wc_hold.lpszClassName = L"ntfscompact_hold_window";
  if (RegisterClassExW(&wc_hold) == 0) {
    return 4;
  }
  const HWND h_wnd{CreateWindowExW(0, L"ntfscompact_hold_window",
                                   L"ntfscompact hold", WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   CW_USEDEFAULT, nullptr, nullptr, h_instance,
                                   nullptr)};
  if (h_wnd == nullptr) {
    return 5;
  }
  ShowWindow(h_wnd, SW_HIDE);
  MSG msg_hold{};
  while (GetMessageW(&msg_hold, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg_hold);
    DispatchMessageW(&msg_hold);
  }
  return static_cast<int>(msg_hold.wParam);
}
