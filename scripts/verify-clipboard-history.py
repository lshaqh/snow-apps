"""Static + behavioural verification for the clipboard-history integration.

This is the offline half of the check. It verifies:
  * every new file is registered in CMakeLists.txt and exists on disk,
  * ApplicationController wiring is present,
  * project-local includes resolve,
  * include guards match filenames,
  * Windows-only types stay inside Q_OS_WIN blocks,
  * every translatable source string has a filled catalog entry,
  * the two MSVC compile errors seen in CI run #1 never come back
    (QStringLiteral with a variable argument, undeclared accessor members),
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
check(len(sources) == 9, "源码可翻译文案共 9 条（实际 %d）" % len(sources))
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

print("\n=== 7. CI 编译错误防回归 ===")
# QStringLiteral / QLatin1String are macros that require a string literal.
# Passing a constexpr `const char*` variable expands into invalid C++
# (MSVC: C2146 "missing ')' before identifier"). CI run #1 caught three of
# these in clipboardhistorystore.cpp.
macro_misuse = []
scanned = NEW_FILES + ["tests/clipboard_history_smoke/main.cpp"]
for relative in scanned:
    source = text(SHOT / relative)
    for macro in ("QStringLiteral", "QLatin1String"):
        for match in re.finditer(r"\b%s\(\s*([^,\)\s]+)" % macro, source):
            argument = match.group(1)
            if not argument.startswith('"'):
                macro_misuse.append("%s: %s(%s)" % (relative, macro, argument))
check(not macro_misuse,
      "QStringLiteral/QLatin1String 只接受字符串字面量（误用: %s）"
      % (", ".join(macro_misuse) if macro_misuse else "无"))

# CI run #1 also hit C2065: inline accessors returned entries_ /
# maximumEntries_ while the members are named m_entries / m_maximumEntries.
store_header = text(SHOT / NEW_FILES[1])
declared = set(re.findall(r"\b(m_\w+)\s*(?:=[^;]*)?;", store_header))
returned = set(re.findall(r"\breturn\s+(\w+)\s*;", store_header))
undeclared = sorted(returned - declared)
check(declared, "clipboardhistorystore.h 能解析出 m_ 成员声明")
check(not undeclared,
      "clipboardhistorystore.h 内联访问器只返回已声明成员（未声明: %s）"
      % (", ".join(undeclared) if undeclared else "无"))

print("\n=== 8. Win+V 交互与焦点回归护栏 ===")
for needle, label in [
    ("Qt::WindowDoesNotAcceptFocus", "历史窗口不接收键盘焦点"),
    ("WA_ShowWithoutActivating", "show() 不激活窗口"),
    ("GetGUIThreadInfo", "打开前快照输入焦点控件"),
    ("WM_PASTE", "文本优先直接投递给编辑控件"),
    ("SendMessageTimeoutW", "WM_PASTE 有超时保护"),
    ("AttachThreadInput", "兜底路径可跨线程恢复前台"),
    ("WH_MOUSE_LL", "点击窗口外部可取消"),
    ("VK_ESCAPE", "Esc 可取消"),
    ("toggleHistory()", "再按一次 Win+V 关闭"),
    ("setPickerOpen(true)", "打开时启用键盘转发"),
    ("setPickerOpen(false)", "关闭时停用键盘转发"),
    ("restoreForeground(focus)", "回贴前恢复原前台窗口"),
    ("suspendWinV(true)", "回贴时临时挂起 Win+V 拦截"),
    ("suspendWinV(false)", "回贴后恢复 Win+V 拦截"),
    ("isMenu(vk)", "Alt 组合键不被历史窗口误吞"),
    ("!self->m_menuDown", "Alt 按住时放行系统快捷键"),
]:
    check(needle in cpp, label)
for forbidden, label in [
    ("m_popup->activateWindow()", "不再调用 activateWindow 抢焦点"),
    ("m_search->setFocus()", "不再把焦点塞进搜索框"),
    ("m_previousForeground", "旧前台窗口裸句柄已移除"),
    ("SetForegroundWindow(m_previousForeground)", "旧的裸 SetForegroundWindow 已移除"),
]:
    check(forbidden not in cpp, label)

print("\n=== 9. 独立托盘包简体中文界面护栏 ===")
# The portable build ships no .qm files, so it needs a compiled-in catalogue.
# Guard against drift: every trHistory() source must be mapped, and every
# embedded translation must match the zh_CN.ts catalogue exactly.
app_main = text(SHOT / "tests/clipboard_history_app/main.cpp")
pairs = re.findall(
    r'\{\s*QStringLiteral\("([^"]*)"\)\s*,\s*QStringLiteral\("([^"]*)"\)\s*\}',
    app_main)
embedded = {source: translation for source, translation in pairs}
check("class ChineseTranslator" in app_main and "QTranslator" in app_main,
      "独立包内置中文 QTranslator")
check("QCoreApplication::installTranslator(&translator)" in app_main,
      "在任何控件创建之前安装翻译器")
check("bool isEmpty() const override { return false; }" in app_main,
      "覆盖 isEmpty()，避免 Qt 跳过未加载 .qm 的翻译器")
check('std::strcmp(context, "ClipboardHistoryWindow")' in app_main,
      "翻译器只作用于 ClipboardHistoryWindow context")
missing = sorted(sources - set(embedded))
extra = sorted(set(embedded) - sources)
check(not missing and not extra,
      "内置映射与 9 条文案一一对应（缺失: %s / 多余: %s）" % (missing, extra))
zh_cn = catalogs["zh_CN"]
mismatch = sorted(k for k in sources
                  if k in embedded and embedded[k] != (zh_cn.get(k) or ""))
check(not mismatch, "内置译文与 zh_CN.ts 逐条一致（不一致: %s）" % mismatch)
check(all(any("\u4e00" <= ch <= "\u9fff" for ch in v) for v in embedded.values()),
      "所有内置译文均为中文")
check("打开剪贴板历史" in app_main and "退出" in app_main,
      "托盘菜单本身保持中文")

print("\n" + "=" * 64)
print("检查项 %d，失败 %d" % (CHECKS, len(FAILURES)))
if FAILURES:
    for item in FAILURES:
        print("  FAIL: " + item)
    sys.exit(1)
print("总结果: PASS  集成产物静态一致，自我写入过滤逻辑与单测断言自洽")
print("=" * 64)
