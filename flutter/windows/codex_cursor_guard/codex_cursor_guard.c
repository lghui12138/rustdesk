#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#define CODEX_MAX_HOOKS 64
#define CODEX_CURSOR_SUBCLASS_ID 0x434F444558435552ULL

typedef struct CodexCursorHook {
  HWND window;
} CodexCursorHook;

static CodexCursorHook g_hooks[CODEX_MAX_HOOKS];
static volatile LONG g_hook_count = 0;
static volatile LONG g_hidden = 0;
static HCURSOR g_previous_cursor = NULL;
static HWINEVENTHOOK g_window_event_hook = NULL;

static void CodexUntrackWindow(HWND window) {
  LONG count = InterlockedCompareExchange(&g_hook_count, 0, 0);
  for (LONG index = 0; index < count; ++index) {
    if (g_hooks[index].window != window) {
      continue;
    }
    for (LONG next = index + 1; next < count; ++next) {
      g_hooks[next - 1] = g_hooks[next];
    }
    g_hooks[count - 1].window = NULL;
    InterlockedExchange(&g_hook_count, count - 1);
    return;
  }
}

static LRESULT CALLBACK CodexCursorGuardProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data) {
  (void)subclass_id;
  (void)reference_data;

  if (InterlockedCompareExchange(&g_hidden, 0, 0) != 0 &&
      message == WM_SETCURSOR &&
      LOWORD(lparam) == HTCLIENT) {
    SetCursor(NULL);
    return TRUE;
  }

  LRESULT result = DefSubclassProc(window, message, wparam, lparam);
  /*
   * Flutter or a cursor plugin may call SetCursor while processing the
   * movement itself. Reassert after the original subclass chain so the local
   * hardware arrow cannot be painted over the remote cursor.
   */
  if (InterlockedCompareExchange(&g_hidden, 0, 0) != 0 &&
      message == WM_MOUSEMOVE) {
    SetCursor(NULL);
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(
        window,
        CodexCursorGuardProc,
        CODEX_CURSOR_SUBCLASS_ID);
    CodexUntrackWindow(window);
  }
  return result;
}

static BOOL CodexHookWindow(HWND window) {
  LONG count = InterlockedCompareExchange(&g_hook_count, 0, 0);
  for (LONG index = 0; index < count; ++index) {
    if (g_hooks[index].window == window) {
      return SetWindowSubclass(
          window,
          CodexCursorGuardProc,
          CODEX_CURSOR_SUBCLASS_ID,
          0);
    }
  }
  if (count >= CODEX_MAX_HOOKS) {
    return FALSE;
  }
  if (!SetWindowSubclass(
          window,
          CodexCursorGuardProc,
          CODEX_CURSOR_SUBCLASS_ID,
          0)) {
    return FALSE;
  }

  g_hooks[count].window = window;
  InterlockedExchange(&g_hook_count, count + 1);
  return TRUE;
}

static BOOL CALLBACK CodexHookChildWindow(HWND window, LPARAM lparam) {
  (void)lparam;
  DWORD window_process = 0;
  GetWindowThreadProcessId(window, &window_process);
  if (window_process == GetCurrentProcessId()) {
    CodexHookWindow(window);
  }
  return TRUE;
}

static BOOL CALLBACK CodexHookTopLevelWindow(HWND window, LPARAM lparam) {
  (void)lparam;
  DWORD window_process = 0;
  GetWindowThreadProcessId(window, &window_process);
  if (window_process != GetCurrentProcessId() || !IsWindowVisible(window)) {
    return TRUE;
  }

  CodexHookWindow(window);
  EnumChildWindows(window, CodexHookChildWindow, 0);
  return TRUE;
}

static void CALLBACK CodexWindowEventProc(
    HWINEVENTHOOK hook,
    DWORD event,
    HWND window,
    LONG object_id,
    LONG child_id,
    DWORD event_thread,
    DWORD event_time) {
  (void)hook;
  (void)child_id;
  (void)event_thread;
  (void)event_time;

  if (InterlockedCompareExchange(&g_hidden, 0, 0) == 0 ||
      window == NULL ||
      (event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW) ||
      (object_id != OBJID_WINDOW && object_id != OBJID_CLIENT)) {
    return;
  }
  DWORD window_process = 0;
  GetWindowThreadProcessId(window, &window_process);
  if (window_process != GetCurrentProcessId()) {
    return;
  }
  CodexHookWindow(window);
  EnumChildWindows(window, CodexHookChildWindow, 0);
  SetCursor(NULL);
}

static void CodexRestoreHooks(void) {
  if (g_window_event_hook != NULL) {
    UnhookWinEvent(g_window_event_hook);
    g_window_event_hook = NULL;
  }
  LONG count = InterlockedCompareExchange(&g_hook_count, 0, 0);
  for (LONG index = count - 1; index >= 0; --index) {
    HWND window = g_hooks[index].window;
    if (window != NULL && IsWindow(window)) {
      RemoveWindowSubclass(
          window,
          CodexCursorGuardProc,
          CODEX_CURSOR_SUBCLASS_ID);
    }
    g_hooks[index].window = NULL;
  }
  InterlockedExchange(&g_hook_count, 0);
}

__declspec(dllexport) int __cdecl codex_cursor_guard_set_hidden(int hidden) {
  if (hidden != 0) {
    BOOL first_hide =
        InterlockedCompareExchange(&g_hidden, 1, 0) == 0;
    if (first_hide) {
      InterlockedExchange(&g_hook_count, 0);
      g_window_event_hook = SetWinEventHook(
          EVENT_OBJECT_CREATE,
          EVENT_OBJECT_SHOW,
          NULL,
          CodexWindowEventProc,
          GetCurrentProcessId(),
          0,
          WINEVENT_OUTOFCONTEXT);
    }
    /*
     * Fullscreen Flutter can create or replace child HWNDs after relative
     * mode starts. Re-enumerate on every acquire/reassert so WM_SETCURSOR is
     * owned for those late windows as well.
     */
    EnumWindows(CodexHookTopLevelWindow, 0);
    HCURSOR previous_cursor = SetCursor(NULL);
    if (first_hide) {
      g_previous_cursor = previous_cursor;
    }
    return (int)InterlockedCompareExchange(&g_hook_count, 0, 0);
  }

  if (InterlockedCompareExchange(&g_hidden, 0, 1) != 0) {
    CodexRestoreHooks();
    SetCursor(g_previous_cursor);
    g_previous_cursor = NULL;
  }
  return 0;
}

__declspec(dllexport) int __cdecl codex_cursor_guard_hook_count(void) {
  return (int)InterlockedCompareExchange(&g_hook_count, 0, 0);
}
