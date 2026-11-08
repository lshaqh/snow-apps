"""Static + behavioural verification for the clipboard-history integration.

This is the offline half of the check. It verifies:
  * every new file is registered in CMakeLists.txt and exists on disk,
  * ApplicationController wiring is present,
  * project-local includes resolve,
  * include guards match filenames,
  * Windows-only types stay inside Q_OS_WIN blocks,
  * every translatable source string has a filled catalog entry,
  * the C++ ClipboardHistoryGuard state machine reproduces every assertion in
    tests/clipboard_history_guard_tests.cpp (a faithful Python mirror).

Real compilation is done by the GitHub Actions workflow
.github/workflows/clipboard-history-verify.yml.

Run from anywhere: python scripts/verify-clipboard-history.py
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# scripts/ -> repository root
ROOT = Path(__file__).resolve().parents[1]
SHOT = ROOT / "snow_shot"
FAILURES = []
CHECKS = 0


def check(condition, label):
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(label)
    print("  %-4s %s" % ("PASS" if condition else "FAIL", label))


def text(path):
    return path.read_text(encoding="utf-8")


NEW_FILES = [
    "include/snow_shot/presentation/clipboardhistoryguard.h",
    "include/snow_shot/presentation/clipboardhistorystore.h",
    "include/snow_shot/presentation/clipboardhistorycontroller.h",
    "src/presentation/services/clipboardhistorystore.cpp",
    "src/presentation/services/clipboardhistorycontroller.cpp",
    "tests/clipboard_history_guard_tests.cpp",
]

print("=== 1. 新文件与 CMake 登记 ===")
for relative in NEW_FILES:
    check((SHOT / relative).is_file(), "文件存在: %s" % relative)
cmake = text(SHOT / "CMakeLists.txt")
for relative in NEW_FILES[:-1]:
    check(relative in cmake, "CMake 已登记: %s" % relative)
check("clipboard_history_guard_tests.cpp" in cmake, "CMake 已登记单元测试")
check("snow-shot-clipboard-history-guard-tests" in cmake, "CMake 已注册 ctest 目标")

print("\n=== 2. ApplicationController 挂接 ===")
controller = text(SHOT / "src/app/applicationcontroller.cpp")
for needle, label in [
    ("#include \"snow_shot/presentation/clipboardhistorycontroller.h\"", "include 新控制器"),
    ("presentation::ClipboardHistoryController clipboardHistory;", "Impl 常驻成员"),
    ("clipboardHistory.start();", "start() 启动监听"),
    ("&presentation::ClipboardHistoryController::shutdown", "aboutToQuit 停止监听"),
]:
    check(needle in controller, label)

print("\n=== 3. 项目内 include 解析 ===")
for relative in [NEW_FILES[3], NEW_FILES[4], NEW_FILES[5]]:
    source = text(SHOT / relative)
    for header in re.findall(r'#include "(snow_shot/[^"]+)"', source):
        check((SHOT / "include" / header).is_file(), "%s -> %s" % (relative.split("/")[-1], header))

print("\n=== 4. 头文件 guard / Windows 隔离 ===")
for relative in NEW_FILES[:3]:
    source = text(SHOT / relative)
    expected = "SNOW_SHOT_PRESENTATION_" + Path(relative).stem.upper() + "_H"
    check(source.count("#ifndef " + expected) == 1, "%s guard 定义" % Path(relative).name)
    check(source.rstrip().endswith("#endif // " + expected), "%s guard 收尾注释" % Path(relative).name)
cpp = text(SHOT / NEW_FILES[4])
windows_only = ["HHOOK", "KBDLLHOOKSTRUCT", "SetWindowsHookExW", "SendInput", "INPUT inputs",
                "GetClipboardSequenceNumber", "SetForegroundWindow", "VK_CONTROL"]
for token in windows_only:
    before = cpp[:cpp.find("#ifdef Q_OS_WIN")] if "#ifdef Q_OS_WIN" in cpp else ""
    check(token not in before, "Win32 符号在 Windows 块外: %s" % token)
check(cpp.count("#ifdef Q_OS_WIN") >= 2 and cpp.count("#endif // Q_OS_WIN") >= 1,
      "Windows 条件编译配对")

print("\n=== 5. i18n 文案覆盖 ===")
sources = set(re.findall(r'trHistory\("([^"]+)"\)', cpp))
catalogs = {}
for locale in ("en_US", "zh_CN", "zh_TW"):
    path = SHOT / ("i18n/settings/snow_shot_settings_%s.ts" % locale)
    root = ET.parse(path).getroot()
    entries = {}
    for context in root.findall("context"):
        if context.findtext("name") != "ClipboardHistoryWindow":
            continue
        for message in context.findall("message"):
            entries[message.findtext("source")] = message.findtext("translation") or ""
    catalogs[locale] = entries
check(len(sources) == 8, "源码可翻译文案共 8 条（实际 %d）" % len(sources))
for locale, entries in catalogs.items():
    missing = sorted(sources - set(entries))
    check(not missing, "%s 覆盖全部文案（缺失: %s）" % (locale, missing))
    empty = sorted(k for k, v in entries.items() if not v.strip())
    check(not empty, "%s 无空翻译（空: %s）" % (locale, empty))
    check(all(v.strip() for v in entries.values()), "%s 无 unfinished 翻译" % locale)
check("ClipboardHistoryWindow" in text(SHOT / "i18n/modules.json"),
      "modules.json 已分配新 context")

print("\n=== 6. Guard 状态机镜像复跑（对照 C++ 单测断言）===")


class Guard:
    SETTLE_DELAY, SETTLE_LIMIT, TTL = 100, 2000, 3000
    PENDING, RECORD, SKIP_OWN, SKIP_DUP = "Pending", "Record", "SkipOwnWrite", "SkipDuplicate"

    def __init__(self):
        self.pending = None
        self.handled = 0
        self.own_seq = None
        self.deadline = 0
        self.first = 0
        self.last = 0

    def mark_own(self, seq, now):
        self.own_seq = seq
        self.deadline = now + self.TTL

    def note(self, seq, now):
        if self.pending is None or self.pending != seq:
            self.pending, self.first, self.last = seq, now, now

    def evaluate(self, seq, now):
        if self.pending is None:
            return self.PENDING
        if now - self.last < self.SETTLE_DELAY and now - self.first < self.SETTLE_LIMIT:
            return self.PENDING
        if seq != self.pending:
            self.pending, self.last = seq, now
            return self.PENDING
        settled, self.pending = seq, None
        if self.handled != 0 and self.handled == settled:
            return self.SKIP_DUP
        self.handled = settled
        if self.own_seq is not None and self.own_seq == settled and now < self.deadline:
            self.own_seq = None
            return self.SKIP_OWN
        self.own_seq = None
        return self.RECORD

    def reset(self):
        self.__init__()


g = Guard()
recorded = skipped = 0
sequence = 100
now = 1000


def external():
    global sequence, now, recorded
    sequence += 1
    now += 1000
    g.note(sequence, now)
    a = g.evaluate(sequence, now)
    b = g.evaluate(sequence, now + Guard.SETTLE_DELAY)
    recorded += 1
    return a == Guard.PENDING and b == Guard.RECORD


def repaste():
    global sequence, now, skipped
    sequence += 1
    now += 1000
    g.mark_own(sequence, now)
    g.note(sequence, now)
    result = g.evaluate(sequence, now + Guard.SETTLE_DELAY)
    skipped += 1
    return result == Guard.SKIP_OWN


results = [external(), external(), repaste(), repaste(), external(), repaste(), external(),
           repaste()]
check(all(results), "8 个交错场景：外部记录 / 自身回贴跳过")
check(g.handled == 108, "handledSequence 跟踪到 108（实际 %d）" % g.handled)
g.note(108, 9000)
check(g.evaluate(108, 9100) == Guard.SKIP_DUP, "重复通知被去重")
g.mark_own(200, 10000)
expired = 10000 + Guard.TTL + 1
g.note(201, expired)
check(g.evaluate(201, expired + Guard.SETTLE_DELAY) == Guard.RECORD, "过期标记不吞真实复制")
g.note(300, 20000)
check(g.evaluate(301, 20100) == Guard.PENDING, "静置期间序号变化继续等待")
g.note(301, 20100)
check(g.evaluate(301, 20200) == Guard.RECORD, "最终稳定序号只记录一次")
g.note(400, 30000)
check(g.evaluate(400, 30000 + Guard.SETTLE_LIMIT + 1) == Guard.RECORD, "静置上限兜底")
g.reset()
check(g.pending is None and g.handled == 0, "reset 清空状态")

g2 = Guard()
seq, clock = 1000, 0
ext = own = 0
for i in range(30):
    seq += 1
    clock += 200
    if i % 3 == 2:
        g2.mark_own(seq, clock)
        g2.note(seq, clock)
        outcome = g2.evaluate(seq, clock + Guard.SETTLE_DELAY)
        own += outcome == Guard.SKIP_OWN
    else:
        g2.note(seq, clock)
        g2.evaluate(seq, clock)
        outcome = g2.evaluate(seq, clock + Guard.SETTLE_DELAY)
        ext += outcome == Guard.RECORD
check(ext == 20 and own == 10, "高频 30 次：外部 20 记录 / 自身 10 跳过（%d/%d）" % (ext, own))

print("\n" + "=" * 64)
print("检查项 %d，失败 %d" % (CHECKS, len(FAILURES)))
if FAILURES:
    for item in FAILURES:
        print("  FAIL: " + item)
    sys.exit(1)
print("总结果: PASS  集成产物静态一致，自我写入过滤逻辑与单测断言自洽")
print("=" * 64)
