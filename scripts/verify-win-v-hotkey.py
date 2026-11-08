"""Win+V hook takeover + clipboard self-write pollution guard: minimal verification.

Pure ctypes / Win32. No Qt, no third-party packages, no build step.

Two things are verified:
  1. RegisterHotKey cannot own Win+V, but WH_KEYBOARD_LL can intercept and swallow it,
     with a VK_MENU_MASK (0xE8) injection to stop the Start menu from opening on Win release.
  2. Re-pasting a history item must NOT create a new history entry. The guard uses a
     GetClipboardSequenceNumber() window + settle delay + duplicate suppression + TTL.

Modes:
  python winv_verify.py --selftest [--simulate-winv]   automated, unattended
  python winv_verify.py --interactive [--timeout 60]    real Win+V presses, live log

Safety: global low-level hooks are always removed in a finally block; the hook callback
only does integer comparisons and lets every non-Win+V key through; interactive mode has
a hard timeout; no Ctrl+V is injected into whatever window the user has focused.
"""

import argparse
import ctypes
import sys
import time
from ctypes import wintypes
from ctypes import POINTER, Structure, Union, WinDLL, WINFUNCTYPE, byref, cast, sizeof

user32 = WinDLL("user32", use_last_error=True)
kernel32 = WinDLL("kernel32", use_last_error=True)

WH_KEYBOARD_LL = 13
HC_ACTION = 0
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_SYSKEYDOWN, WM_SYSKEYUP = 0x0104, 0x0105
WM_CLIPBOARDUPDATE = 0x031D
PM_REMOVE = 0x0001
VK_LWIN, VK_RWIN, VK_V = 0x5B, 0x5C, 0x56
VK_MENU_MASK = 0xE8
INPUT_KEYBOARD, KEYEVENTF_KEYUP = 1, 0x0002
CF_UNICODETEXT, GMEM_MOVEABLE = 13, 0x0002
HWND_MESSAGE = ctypes.c_void_p((-3) & ((1 << (ctypes.sizeof(ctypes.c_void_p) * 8)) - 1))
SETTLE_DELAY, SETTLE_LIMIT, OWN_WRITE_TTL = 0.10, 2.0, 3.0


class KBDLLHOOKSTRUCT(Structure):
    _fields_ = [("vkCode", wintypes.DWORD), ("scanCode", wintypes.DWORD),
                ("flags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class KEYBDINPUT(Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class INPUTUNION(Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]


class INPUT(Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", INPUTUNION)]


WNDPROC = WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_void_p, wintypes.UINT,
                      wintypes.WPARAM, wintypes.LPARAM)
HOOKPROC = WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)


class WNDCLASSEXW(Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("style", wintypes.UINT),
                ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", ctypes.c_void_p),
                ("hIcon", ctypes.c_void_p), ("hCursor", ctypes.c_void_p),
                ("hbrBackground", ctypes.c_void_p), ("lpszMenuName", wintypes.LPCWSTR),
                ("lpszClassName", wintypes.LPCWSTR), ("hIconSm", ctypes.c_void_p)]


user32.RegisterHotKey.argtypes = [ctypes.c_void_p, ctypes.c_int, wintypes.UINT, wintypes.UINT]
user32.RegisterHotKey.restype = wintypes.BOOL
user32.UnregisterHotKey.argtypes = [ctypes.c_void_p, ctypes.c_int]
user32.GetClipboardSequenceNumber.argtypes = []
user32.GetClipboardSequenceNumber.restype = wintypes.DWORD
user32.PeekMessageW.argtypes = [POINTER(wintypes.MSG), ctypes.c_void_p,
                                wintypes.UINT, wintypes.UINT, wintypes.UINT]
user32.PeekMessageW.restype = wintypes.BOOL
user32.TranslateMessage.argtypes = [POINTER(wintypes.MSG)]
user32.DispatchMessageW.argtypes = [POINTER(wintypes.MSG)]
user32.DispatchMessageW.restype = ctypes.c_ssize_t
user32.DefWindowProcW.argtypes = [ctypes.c_void_p, wintypes.UINT,
                                  wintypes.WPARAM, wintypes.LPARAM]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.RegisterClassExW.argtypes = [POINTER(WNDCLASSEXW)]
user32.RegisterClassExW.restype = wintypes.WORD
user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                   wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                                   ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
user32.CreateWindowExW.restype = ctypes.c_void_p
user32.DestroyWindow.argtypes = [ctypes.c_void_p]
user32.AddClipboardFormatListener.argtypes = [ctypes.c_void_p]
user32.AddClipboardFormatListener.restype = wintypes.BOOL
user32.RemoveClipboardFormatListener.argtypes = [ctypes.c_void_p]
user32.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, ctypes.c_void_p, wintypes.DWORD]
user32.SetWindowsHookExW.restype = ctypes.c_void_p
user32.UnhookWindowsHookEx.argtypes = [ctypes.c_void_p]
user32.CallNextHookEx.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                  wintypes.WPARAM, wintypes.LPARAM]
user32.CallNextHookEx.restype = ctypes.c_ssize_t
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
kernel32.GetModuleHandleW.restype = ctypes.c_void_p
user32.SendInput.argtypes = [wintypes.UINT, POINTER(INPUT), ctypes.c_int]
user32.SendInput.restype = wintypes.UINT
user32.OpenClipboard.argtypes = [ctypes.c_void_p]
user32.OpenClipboard.restype = wintypes.BOOL
user32.CloseClipboard.argtypes = []
user32.CloseClipboard.restype = wintypes.BOOL
user32.EmptyClipboard.argtypes = []
user32.EmptyClipboard.restype = wintypes.BOOL
user32.IsClipboardFormatAvailable.argtypes = [wintypes.UINT]
user32.IsClipboardFormatAvailable.restype = wintypes.BOOL
user32.GetClipboardData.argtypes = [wintypes.UINT]
user32.GetClipboardData.restype = ctypes.c_void_p
user32.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
user32.SetClipboardData.restype = ctypes.c_void_p
kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
kernel32.GlobalAlloc.restype = ctypes.c_void_p
kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
kernel32.GlobalFree.argtypes = [ctypes.c_void_p]


def seq():
    return user32.GetClipboardSequenceNumber()


def set_clipboard_text(text, hwnd):
    data = text.encode("utf-16-le") + b"\x00\x00"
    handle = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
    if not handle:
        raise OSError("GlobalAlloc failed")
    ptr = kernel32.GlobalLock(handle)
    ctypes.memmove(ptr, data, len(data))
    kernel32.GlobalUnlock(handle)
    for _ in range(20):
        if user32.OpenClipboard(hwnd):
            break
        time.sleep(0.01)
    else:
        kernel32.GlobalFree(handle)
        raise OSError("OpenClipboard failed")
    user32.EmptyClipboard()
    result = user32.SetClipboardData(CF_UNICODETEXT, handle)
    user32.CloseClipboard()
    if not result:
        kernel32.GlobalFree(handle)
        raise OSError("SetClipboardData failed")


def read_clipboard_text(hwnd):
    if not user32.IsClipboardFormatAvailable(CF_UNICODETEXT):
        return None
    for _ in range(20):
        if user32.OpenClipboard(hwnd):
            break
        time.sleep(0.01)
    else:
        return None
    try:
        handle = user32.GetClipboardData(CF_UNICODETEXT)
        if not handle:
            return None
        ptr = kernel32.GlobalLock(handle)
        text = ctypes.wstring_at(ptr) if ptr else None
        if ptr:
            kernel32.GlobalUnlock(handle)
        return text
    finally:
        user32.CloseClipboard()


def send_key(vk, up=False):
    item = INPUT()
    item.type = INPUT_KEYBOARD
    item.ki.wVk = vk
    item.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
    return user32.SendInput(1, byref(item), sizeof(INPUT)) == 1


def send_menu_mask():
    """Press+release an unassigned VK so the Win key release cannot open Start."""
    items = (INPUT * 2)()
    for i in range(2):
        items[i].type = INPUT_KEYBOARD
        items[i].ki.wVk = VK_MENU_MASK
    items[1].ki.dwFlags = KEYEVENTF_KEYUP
    return user32.SendInput(2, items, sizeof(INPUT)) == 2


class ClipboardHistoryGuard:
    """Portable logic to translate into C++/Qt: self-write filtering by sequence number.

    External copy  -> settle -> record.
    Own re-paste   -> settle -> seq equals the one registered after our write -> skip.
    Duplicate notify for the same seq -> skip. TTL prevents a stale marker eating a
    real user copy on the (extremely unlikely) event of sequence-number reuse.
    """

    def __init__(self, hwnd):
        self.hwnd = hwnd
        self.history = []
        self.own_write_seq = None
        self.own_write_deadline = 0.0
        self.handled_seq = 0
        self.pending_seq = None
        self.last_change = 0.0
        self.first_change = 0.0
        self.stats = {"external": 0, "self_skipped": 0, "duplicate": 0}
        self.trace = []

    def mark_own_write(self):
        self.own_write_seq = seq()
        self.own_write_deadline = time.monotonic() + OWN_WRITE_TTL

    def on_update(self):
        now = time.monotonic()
        current = seq()
        if current != self.pending_seq:
            self.pending_seq = current
            self.first_change = now
            self.last_change = now
        else:
            self.last_change = self.last_change or now

    def tick(self):
        if self.pending_seq is None:
            return None
        now = time.monotonic()
        if (now - self.last_change < SETTLE_DELAY
                and now - self.first_change < SETTLE_LIMIT):
            return None
        current = seq()
        if current != self.pending_seq:
            self.pending_seq = current
            self.last_change = now
            return None

        settled_seq = current
        self.pending_seq = None
        if settled_seq == self.handled_seq:
            self.stats["duplicate"] += 1
            self.trace.append(("duplicate", settled_seq))
            return "duplicate"
        self.handled_seq = settled_seq

        if (self.own_write_seq is not None
                and settled_seq == self.own_write_seq
                and now < self.own_write_deadline):
            self.own_write_seq = None
            self.stats["self_skipped"] += 1
            self.trace.append(("self", settled_seq))
            return "self"

        self.own_write_seq = None
        text = read_clipboard_text(self.hwnd)
        self.history.append((settled_seq, text))
        self.stats["external"] += 1
        self.trace.append(("external", settled_seq))
        return "external"


class Verifier:
    def __init__(self):
        self.hwnd = None
        self.hook = None
        self.listener_ok = False
        self.win_down = False
        self.winv_consumed = False
        self.winv_hits = 0
        self.mask_pending = False
        self.mask_sent = 0
        self.hook_callbacks = 0
        self.guard = None
        self.wnd_proc = None
        self.hook_proc = None
        self.class_atom = 0

    def start(self):
        self._wnd_proc = WNDPROC(self._on_window_message)
        self._hook_proc = HOOKPROC(self._on_keyboard)
        self.wnd_proc = self._wnd_proc
        self.hook_proc = self._hook_proc

        cls = WNDCLASSEXW()
        cls.cbSize = sizeof(WNDCLASSEXW)
        cls.lpfnWndProc = self._wnd_proc
        cls.lpszClassName = "WinVVerifyWindow"
        self.class_atom = user32.RegisterClassExW(byref(cls))
        if not self.class_atom:
            raise OSError("RegisterClassExW failed: %s" % ctypes.get_last_error())

        self.hwnd = user32.CreateWindowExW(0, "WinVVerifyWindow", "winv-verify", 0,
                                           0, 0, 0, 0, HWND_MESSAGE, None, None, None)
        if not self.hwnd:
            raise OSError("CreateWindowExW failed: %s" % ctypes.get_last_error())
        self.listener_ok = bool(user32.AddClipboardFormatListener(self.hwnd))
        if not self.listener_ok:
            raise OSError("AddClipboardFormatListener failed: %s" % ctypes.get_last_error())

        self.hook = user32.SetWindowsHookExW(WH_KEYBOARD_LL, self._hook_proc,
                                             kernel32.GetModuleHandleW(None), 0)
        if not self.hook:
            raise OSError("SetWindowsHookExW failed: %s" % ctypes.get_last_error())

        self.guard = ClipboardHistoryGuard(self.hwnd)
        # Create the message queue so the low-level hook gets dispatched.
        msg = wintypes.MSG()
        user32.PeekMessageW(byref(msg), None, 0, 0, 0)

    def _on_window_message(self, hwnd, message, wparam, lparam):
        if message == WM_CLIPBOARDUPDATE and self.guard is not None:
            self.guard.on_update()
            return 0
        return user32.DefWindowProcW(ctypes.c_void_p(hwnd), message, wparam, lparam)

    def _on_keyboard(self, code, wparam, lparam):
        try:
            if code == HC_ACTION:
                self.hook_callbacks += 1
                kb = cast(ctypes.c_void_p(lparam), POINTER(KBDLLHOOKSTRUCT)).contents
                vk = kb.vkCode
                down = wparam in (WM_KEYDOWN, WM_SYSKEYDOWN)
                up = wparam in (WM_KEYUP, WM_SYSKEYUP)

                if vk == VK_MENU_MASK:
                    return 1  # our own mask must not reach anyone
                if vk in (VK_LWIN, VK_RWIN):
                    if down:
                        self.win_down = True
                        self.winv_consumed = False
                    elif up:
                        self.win_down = False
                        if self.winv_consumed:
                            # Only a Win press that actually consumed Win+V needs masking;
                            # plain Win+D / Win+E must behave normally.
                            self.mask_pending = True
                            self.winv_consumed = False
                elif vk == VK_V and self.win_down:
                    if down:
                        self.winv_hits += 1
                        self.winv_consumed = True
                    return 1  # swallow both V down and V up -> no system Win+V panel
        except Exception:
            return user32.CallNextHookEx(None, code, wparam, lparam)
        return user32.CallNextHookEx(None, code, wparam, lparam)

    def pump(self, seconds, verbose=False):
        msg = wintypes.MSG()
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            while user32.PeekMessageW(byref(msg), None, 0, 0, PM_REMOVE):
                user32.TranslateMessage(byref(msg))
                user32.DispatchMessageW(byref(msg))
            if self.mask_pending:
                self.mask_pending = False
                if send_menu_mask():
                    self.mask_sent += 1
            outcome = self.guard.tick()
            if verbose and outcome == "external":
                print("    [记录] seq=%s 内容=%r" %
                      (self.guard.handled_seq, self.guard.history[-1][1]))
            elif verbose and outcome == "self":
                print("    [跳过] 自身回贴，未新增历史 (seq=%s)" % self.guard.handled_seq)
            time.sleep(0.005)

    def close(self):
        if self.hook:
            user32.UnhookWindowsHookEx(self.hook)
            self.hook = None
        if self.listener_ok and self.hwnd:
            user32.RemoveClipboardFormatListener(self.hwnd)
            self.listener_ok = False
        if self.hwnd:
            user32.DestroyWindow(self.hwnd)
            self.hwnd = None


def probe_register_hotkey():
    print("=== 步骤 1: RegisterHotKey 能否占有 Win+V ===")
    msg = wintypes.MSG()
    user32.PeekMessageW(byref(msg), None, 0, 0, 0)
    ctypes.set_last_error(0)
    ok = bool(user32.RegisterHotKey(None, 0xB201, 0x0008 | 0x4000, VK_V))
    err = ctypes.get_last_error()
    if ok:
        user32.UnregisterHotKey(None, 0xB201)
    print("  Win+V : %s  %s" % ("OK" if ok else "FAIL",
                                "registered" if ok else ctypes.FormatError(err).strip()))
    ctypes.set_last_error(0)
    ok2 = bool(user32.RegisterHotKey(None, 0xB202, 0x0002 | 0x0004 | 0x4000, VK_V))
    if ok2:
        user32.UnregisterHotKey(None, 0xB202)
    print("  Ctrl+Shift+V (兜底) : %s" % ("OK" if ok2 else "FAIL"))
    print()
    return not ok and ok2


def run_selftest(v, simulate_winv):
    print("=== 步骤 3: 自我写入污染验证（自动化）===")
    hwnd = v.hwnd
    backup = read_clipboard_text(hwnd)
    if backup is None:
        print("  [提示] 当前剪贴板无纯文本格式，结束时仅做文本恢复。")
    failures = []
    try:
        phases = [
            ("外部复制 3 条", [("external", "外部内容-%d" % i) for i in range(3)]),
            ("自身回贴 3 次", [("self", "回贴内容-%d" % i) for i in range(3)]),
            ("交错：外部/自身/外部/自身", [
                ("external", "交错-外-1"), ("self", "交错-回-1"),
                ("external", "交错-外-2"), ("self", "交错-回-2")]),
        ]
        expected_external = 0
        expected_self = 0
        for title, actions in phases:
            print("\n  -- %s --" % title)
            for kind, text in actions:
                set_clipboard_text(text, hwnd)
                if kind == "self":
                    v.guard.mark_own_write()
                v.pump(0.45, verbose=True)
                if kind == "external":
                    expected_external += 1
                else:
                    expected_self += 1
        v.pump(0.4)

        print("\n  -- 容量/高频: 20 条外部 + 10 次自身回贴（间隔 200ms）--")
        for i in range(30):
            if i % 3 == 2:
                set_clipboard_text("高频回贴-%d" % i, hwnd)
                v.guard.mark_own_write()
                expected_self += 1
            else:
                set_clipboard_text("高频外部-%d" % i, hwnd)
                expected_external += 1
            v.pump(0.2)
        v.pump(0.4)

        got = v.guard.stats
        print("\n  历史条目 (%d 条，仅列前 5 条):" % len(v.guard.history))
        for s, text in v.guard.history[:5]:
            print("    seq=%-6s %r" % (s, text))
        if len(v.guard.history) > 5:
            print("    ... 另 %d 条" % (len(v.guard.history) - 5))
        print("\n  统计: 外部记录=%d (期望 %d), 自身写入跳过=%d (期望 %d), 重复通知=%d" %
              (got["external"], expected_external, got["self_skipped"], expected_self,
               got["duplicate"]))
        if got["external"] != expected_external:
            failures.append("外部复制漏记或多记")
        if got["self_skipped"] != expected_self:
            failures.append("自身回贴未被全部过滤")
        if any(text is None or str(text).startswith(("回贴", "交错-回", "高频回贴"))
               for _, text in v.guard.history):
            failures.append("历史中混入了自身回贴内容")
        print("  结论: %s" % ("PASS  自我写入污染已消除" if not failures
                             else "FAIL  " + "; ".join(failures)))

        print("\n  -- 已知边界: 100ms 防抖窗口内连续 5 次复制 --")
        before = v.guard.stats["external"]
        for i in range(5):
            set_clipboard_text("极快复制-%d" % i, hwnd)
            v.pump(0.02)
        v.pump(0.4)
        captured = v.guard.stats["external"] - before
        print("  5 次复制实际记录 %d 条（时间防抖会合并同窗口内的写入，"
              "属已知取舍，不计入 PASS/FAIL）" % captured)
        print("  改进方向: 稳定后按内容指纹去重，而非只按时间合并；"
              "或缩短防抖到 30ms 并在序号变化时暂存内容摘要。")

        if simulate_winv:
            print("\n=== 步骤 4: 注入 Win+V 验证钩子拦截 ===")
            hits_before = v.winv_hits
            print("  [注意] 接下来 1 秒内会模拟一次 Win+V 按键。")
            send_key(VK_LWIN)
            send_key(VK_V)
            send_key(VK_V, up=True)
            send_key(VK_LWIN, up=True)
            v.pump(1.2)
            print("  钩子回调总次数 : %d" % v.hook_callbacks)
            print("  Win+V 命中次数 : %d (新增 %d)" %
                  (v.winv_hits, v.winv_hits - hits_before))
            print("  菜单掩码已发送 : %d 次" % v.mask_sent)
            if v.winv_hits - hits_before < 1:
                failures.append("钩子未捕获 Win+V")
            else:
                print("  结论: PASS  Win+V 已被钩子捕获并吞掉（系统面板不会弹出）")
        return failures
    finally:
        if backup is not None:
            try:
                set_clipboard_text(backup, hwnd)
                print("\n  [已恢复] 运行前的剪贴板纯文本内容。")
            except OSError:
                print("\n  [警告] 剪贴板文本恢复失败。")


def run_interactive(v, timeout):
    print("=== 交互验证 ===")
    print("  钩子已安装。请：")
    print("   1) 复制几段不同文字（Ctrl+C），观察下方 [记录] 日志")
    print("   2) 按 Win+V，观察 [拦截] 日志；系统自带面板不应弹出")
    print("   3) 按 Ctrl+C 结束等待，或等 %d 秒自动退出" % timeout)
    print("  退出后钩子会卸载，Win+V 恢复系统默认行为。\n")
    end = time.monotonic() + timeout
    last_hits = 0
    while time.monotonic() < end:
        v.pump(0.2, verbose=True)
        if v.winv_hits != last_hits:
            print("  [拦截] Win+V 命中 #%d，按键已吞掉" % v.winv_hits)
            last_hits = v.winv_hits
    print("\n  历史共 %d 条，其中自身写入被跳过 %d 次。" %
          (len(v.guard.history), v.guard.stats["self_skipped"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true", help="unattended verification")
    parser.add_argument("--interactive", action="store_true", help="wait for real Win+V")
    parser.add_argument("--simulate-winv", action="store_true",
                        help="in selftest, inject a Win+V key combo via SendInput")
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()
    if not (args.selftest or args.interactive):
        args.selftest = True

    ok_register = probe_register_hotkey()

    v = Verifier()
    failures = []
    try:
        v.start()
        print("=== 步骤 2: 安装 WH_KEYBOARD_LL + 剪贴板监听 ===")
        print("  钩子句柄            : %s" % ("OK" if v.hook else "FAIL"))
        print("  AddClipboardFormatListener : %s" % ("OK" if v.listener_ok else "FAIL"))
        print("  当前剪贴板序号      : %d\n" % seq())
        if not v.hook or not v.listener_ok:
            failures.append("钩子或剪贴板监听安装失败")

        if args.selftest:
            failures += run_selftest(v, args.simulate_winv)
        if args.interactive:
            run_interactive(v, args.timeout)
    finally:
        v.close()
        print("\n=== 清理 ===")
        print("  钩子已卸载、剪贴板监听已移除、隐藏窗口已销毁。")

    print("\n" + "=" * 62)
    if not ok_register:
        failures.append("RegisterHotKey 探测结果与预期不符")
    if failures:
        print("总结果: FAIL  ->  " + "; ".join(failures))
        return 1
    hook_note = ("Win+V 实测被钩子捕获并吞掉" if args.simulate_winv
                 else "钩子安装成功（未注入按键，拦截效果待 --simulate-winv 或交互验证）")
    print("总结果: PASS  %s；自我写入污染已消除" % hook_note)
    print("=" * 62)
    return 0


if __name__ == "__main__":
    sys.exit(main())
