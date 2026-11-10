// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/clipboardhistorycontroller.h"

#include "snow_shot/presentation/clipboardhistoryguard.h"
#include "snow_shot/presentation/clipboardhistorystore.h"

#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace snow_shot::presentation {
namespace {

QString trHistory(const char* source) {
    return QCoreApplication::translate("ClipboardHistoryWindow", source);
}

class HistoryPopup final : public QWidget {
  public:
    explicit HistoryPopup(std::function<void()> onActivate, std::function<void()> onClose)
        : QWidget(nullptr), m_onActivate(std::move(onActivate)), m_onClose(std::move(onClose)) {
        // The picker must never become the foreground window. Otherwise
        // Explorer's address bar (and other inline editors) drops its caret and
        // collapses as soon as the picker opens, so the later Ctrl+V has
        // nowhere to land. Keyboard navigation is routed by the low-level
        // hook instead of by Qt focus events.
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setWindowTitle(trHistory("Clipboard history"));
        resize(520, 460);
    }

    // Qt's raise() only uses HWND_TOP, which puts a window at the head of the
    // *normal* Z-order band. The Windows Start menu is topmost itself and
    // re-asserts its position whenever Win is pressed or released, so a picker
    // displayed with SW_SHOWNOACTIVATE can land behind it. Placing the native
    // handle at HWND_TOPMOST with SWP_NOACTIVATE makes the picker visible over
    // the Start menu while still never taking the foreground or the caret.
    void ensureTopmost() {
#ifdef Q_OS_WIN
        if (!isVisible()) {
            return;
        }
        const HWND handle = reinterpret_cast<HWND>(winId());
        if (handle == nullptr) {
            return;
        }
        SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
    }

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            hide();
            m_onClose();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            m_onActivate();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void closeEvent(QCloseEvent* event) override {
        event->ignore();
        hide();
        m_onClose();
    }

  private:
    std::function<void()> m_onActivate;
    std::function<void()> m_onClose;
};

#ifdef Q_OS_WIN
// Unassigned virtual key used to stop the Start menu from opening when the
// Win key is released after Win+V was swallowed. Mirrors jietuba's menu mask.
constexpr WORD kMenuMaskVk = 0xE8;

// Every keystroke we synthesize carries this tag in dwExtraInfo. The hook can
// then let it through instead of swallowing it: swallowing the mask key is
// exactly what lets the Start menu open again (see jietuba's input_engine.cpp:
// "swallowing it would let the Start menu open"). It also stops our own Ctrl+V
// from being mistaken for a second Win+V while the user still holds Win down.
constexpr ULONG_PTR kOwnInputTag = 0x53534F57ULL; // "SSOW"

// Low-level keyboard hook owns Win+V (system-reserved for RegisterHotKey) and
// the Ctrl+Shift+V fallback. The hook lives on a dedicated thread with a Qt
// event loop, following the PrintScreenShortcutRecorder pattern.
class WinVHotkeyHook final {
  public:
    using Activation = std::function<void()>;
    using KeyNotification = std::function<void(std::uint32_t vk, bool shift)>;
    using PointerNotification = std::function<void(int x, int y)>;

    WinVHotkeyHook(QObject* owner, Activation activation, KeyNotification key,
                   PointerNotification pointer)
        : m_owner(owner), m_activation(std::move(activation)),
          m_keyNotification(std::move(key)), m_pointerNotification(std::move(pointer)) {}

    ~WinVHotkeyHook() { stop(); }

    WinVHotkeyHook(const WinVHotkeyHook&) = delete;
    WinVHotkeyHook& operator=(const WinVHotkeyHook&) = delete;

    bool start() {
        if (m_thread.isRunning()) {
            return m_installed.load(std::memory_order_acquire);
        }
        m_worker = new QObject();
        m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        QObject::connect(&m_thread, &QThread::started, m_worker, [this]() {
            active_.store(this, std::memory_order_release);
            m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc,
                                       GetModuleHandleW(nullptr), 0);
            m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc,
                                            GetModuleHandleW(nullptr), 0);
            if (m_hook == nullptr) {
                qWarning("Clipboard history Win+V hook failed: %lu",
                         static_cast<unsigned long>(GetLastError()));
                active_.store(nullptr, std::memory_order_release);
            } else {
                m_installed.store(true, std::memory_order_release);
            }
            if (m_mouseHook == nullptr) {
                qWarning("Clipboard history outside-click hook failed: %lu",
                         static_cast<unsigned long>(GetLastError()));
            }
        });
        m_thread.setObjectName(QStringLiteral("ClipboardHistoryWinVHook"));
        m_thread.start();
        // Establish interception before start() returns.
        QMetaObject::invokeMethod(m_worker, [] {}, Qt::BlockingQueuedConnection);
        return m_installed.load(std::memory_order_acquire);
    }

    void stop() {
        active_.store(nullptr, std::memory_order_release);
        if (m_worker != nullptr && m_thread.isRunning()) {
            QMetaObject::invokeMethod(
                m_worker,
                [this]() {
                    if (m_hook != nullptr) {
                        UnhookWindowsHookEx(m_hook);
                        m_hook = nullptr;
                    }
                    if (m_mouseHook != nullptr) {
                        UnhookWindowsHookEx(m_mouseHook);
                        m_mouseHook = nullptr;
                    }
                },
                Qt::BlockingQueuedConnection);
        }
        m_thread.quit();
        if (!m_thread.wait(2000)) {
            qWarning("Clipboard history Win+V hook thread did not stop in time");
            m_thread.wait();
        }
        m_installed.store(false, std::memory_order_release);
        m_worker = nullptr;
    }

    void setTakeOverWinV(bool value) { m_takeOverWinV.store(value, std::memory_order_release); }

    void setPickerOpen(bool value) {
        m_pickerOpen.store(value, std::memory_order_release);
    }

    // Our own Ctrl+V must not be swallowed by the Win+V rule when the user
    // still has the Win key held down while choosing an entry.
    void suspendWinV(bool value) { m_suspendWinV.store(value, std::memory_order_release); }

  private:
    static bool isControl(DWORD vk) {
        return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL;
    }

    static bool isShift(DWORD vk) {
        return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
    }

    static bool isMenu(DWORD vk) {
        return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
    }

    static bool isNavigationKey(DWORD vk) {
        return vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_UP || vk == VK_DOWN ||
               vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END ||
               vk == VK_BACK || vk == VK_DELETE;
    }

    static bool isPrintableKey(DWORD vk) {
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') ||
            (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)) {
            return true;
        }
        switch (vk) {
        case VK_SPACE:
        case VK_OEM_1:
        case VK_OEM_2:
        case VK_OEM_3:
        case VK_OEM_4:
        case VK_OEM_5:
        case VK_OEM_6:
        case VK_OEM_7:
        case VK_OEM_PLUS:
        case VK_OEM_COMMA:
        case VK_OEM_MINUS:
        case VK_OEM_PERIOD:
        case VK_MULTIPLY:
        case VK_ADD:
        case VK_SUBTRACT:
        case VK_DECIMAL:
        case VK_DIVIDE:
            return true;
        default:
            return false;
        }
    }

    static LRESULT CALLBACK keyboardProc(int code, WPARAM message, LPARAM data) {
        WinVHotkeyHook* self = active_.load(std::memory_order_acquire);
        if (code != HC_ACTION || self == nullptr || data == 0) {
            return CallNextHookEx(nullptr, code, message, data);
        }
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        const DWORD vk = key->vkCode;
        if (key->dwExtraInfo == kOwnInputTag) {
            // Our own mask and paste keystrokes must reach Windows. Returning 1
            // here hid the mask from the shell's Win-tap detection, so the
            // Start menu still opened when Win was released. The tag also
            // keeps an injected Ctrl+V from being read as Win+V.
            return CallNextHookEx(nullptr, code, message, data);
        }
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;

        if (vk == VK_LWIN || vk == VK_RWIN) {
            if (down) {
                // Only a *fresh* Win press starts a new sequence. Windows can
                // repeat WM_KEYDOWN while Win is held (jietuba documents this
                // in input_engine.cpp: "Win's auto-repeat can reactivate the
                // Start menu after an early mask"). Clearing the consumed flag
                // on every repeated down loses the Win+V we already swallowed,
                // so no mask is sent on release and the Start menu opens.
                if (!self->m_winDown) {
                    self->m_winConsumed = false;
                }
                self->m_winDown = true;
            } else if (up) {
                self->m_winDown = false;
                // Only mask a Win press that actually consumed Win+V, so
                // Win+D / Win+E keep their normal behaviour. A second mask on
                // release covers auto-repeat re-activation after the early one.
                if (self->m_winConsumed) {
                    self->sendMenuMask();
                    self->m_winConsumed = false;
                }
            }
            return CallNextHookEx(nullptr, code, message, data);
        }
        if (isControl(vk)) {
            self->m_controlDown = down;
            return CallNextHookEx(nullptr, code, message, data);
        }
        if (isShift(vk)) {
            self->m_shiftDown = down;
            return CallNextHookEx(nullptr, code, message, data);
        }
        if (isMenu(vk)) {
            self->m_menuDown = down;
            return CallNextHookEx(nullptr, code, message, data);
        }
        if (vk == 'V') {
            const bool winV = self->m_winDown &&
                              self->m_takeOverWinV.load(std::memory_order_acquire) &&
                              !self->m_suspendWinV.load(std::memory_order_acquire);
            const bool fallback = self->m_controlDown && self->m_shiftDown;
            if (winV || fallback) {
                if (down) {
                    if (winV) {
                        self->m_winConsumed = true;
                        // Mask while Win is still down: the shell then already
                        // sees a second key in this Win press, instead of a
                        // lone Win tap. The release mask below is the fallback.
                        self->sendMenuMask();
                    }
                    self->notifyActivation();
                }
                // Swallow both V down and V up: the system Win+V panel never sees it.
                return 1;
            }
        }

        // While the picker is visible it owns navigation keys and plain ASCII
        // typing. It has no keyboard focus (by design), so the hook forwards
        // these to the search box and swallows them so they never leak into
        // the target editor. Modifier combos (Ctrl+C, Alt+Tab, ...) pass
        // through untouched.
        if (self->m_pickerOpen.load(std::memory_order_acquire)) {
            const bool routed = isNavigationKey(vk) || isPrintableKey(vk);
            const bool plain = !self->m_winDown && !self->m_controlDown && !self->m_menuDown;
            if (down && plain && routed) {
                self->m_swallowed[vk & 0xFF] = true;
                self->notifyKey(static_cast<std::uint32_t>(vk), self->m_shiftDown);
                return 1;
            }
            if (up && routed && self->m_swallowed[vk & 0xFF]) {
                self->m_swallowed[vk & 0xFF] = false;
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    static LRESULT CALLBACK mouseProc(int code, WPARAM message, LPARAM data) {
        WinVHotkeyHook* self = active_.load(std::memory_order_acquire);
        if (code == HC_ACTION && self != nullptr && data != 0 &&
            self->m_pickerOpen.load(std::memory_order_acquire) &&
            (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
             message == WM_MBUTTONDOWN)) {
            const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            self->notifyOutsideClick(mouse->pt.x, mouse->pt.y);
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    void sendMenuMask() {
        INPUT inputs[2] = {};
        for (INPUT& input : inputs) {
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = kMenuMaskVk;
            input.ki.dwExtraInfo = kOwnInputTag;
        }
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        // Sent synchronously, before the Win key-up is passed on, so the mask
        // is queued ahead of the release that the shell evaluates. SendInput
        // re-enters this hook; the tag makes those events pass straight through
        // without recursing. Keep this callback free of anything slower.
        if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
            qWarning("Clipboard history Win+V menu mask failed: %lu",
                     static_cast<unsigned long>(GetLastError()));
        }
    }

    void notifyActivation() {
        QObject* owner = m_owner.data();
        if (owner == nullptr) {
            return;
        }
        Activation callback = m_activation;
        QMetaObject::invokeMethod(
            owner,
            [callback = std::move(callback)]() {
                if (callback) {
                    callback();
                }
            },
            Qt::QueuedConnection);
    }

    void notifyKey(std::uint32_t vk, bool shift) {
        QObject* owner = m_owner.data();
        if (owner == nullptr) {
            return;
        }
        KeyNotification callback = m_keyNotification;
        QMetaObject::invokeMethod(
            owner,
            [callback, vk, shift]() {
                if (callback) {
                    callback(vk, shift);
                }
            },
            Qt::QueuedConnection);
    }

    void notifyOutsideClick(int x, int y) {
        QObject* owner = m_owner.data();
        if (owner == nullptr) {
            return;
        }
        PointerNotification callback = m_pointerNotification;
        QMetaObject::invokeMethod(
            owner,
            [callback, x, y]() {
                if (callback) {
                    callback(x, y);
                }
            },
            Qt::QueuedConnection);
    }

    static std::atomic<WinVHotkeyHook*> active_;
    QPointer<QObject> m_owner;
    Activation m_activation;
    KeyNotification m_keyNotification;
    PointerNotification m_pointerNotification;
    QThread m_thread;
    QObject* m_worker = nullptr;
    HHOOK m_hook = nullptr;
    HHOOK m_mouseHook = nullptr;
    std::atomic<bool> m_installed{false};
    std::atomic<bool> m_takeOverWinV{true};
    std::atomic<bool> m_pickerOpen{false};
    std::atomic<bool> m_suspendWinV{false};
    bool m_swallowed[256] = {}; // hook thread only: keys whose key-up we owe
    bool m_winDown = false;
    bool m_winConsumed = false;
    bool m_controlDown = false;
    bool m_shiftDown = false;
    bool m_menuDown = false;
};

std::atomic<WinVHotkeyHook*> WinVHotkeyHook::active_{nullptr};

// Win+V may be pressed while the caret sits in any control. Remember that
// control before showing the picker, then paste back into it directly.

struct FocusSnapshot {
    HWND foreground = nullptr;
    HWND focus = nullptr;
    DWORD thread = 0;
    bool editable = false;
};

bool isEditableControl(HWND window) {
    if (window == nullptr) {
        return false;
    }
    wchar_t className[64] = {};
    if (GetClassNameW(window, className, 64) == 0) {
        return false;
    }
    const std::wstring_view name(className);
    // Explorer's address bar is an Edit inside a ComboBoxEx32; Notepad, Office
    // cells and most editors use Edit/RichEdit as well.
    return name.rfind(L"Edit", 0) == 0 || name.rfind(L"RichEdit", 0) == 0 ||
           name.rfind(L"ComboBox", 0) == 0 || name.rfind(L"AtlEdit", 0) == 0 ||
           name.rfind(L"Scintilla", 0) == 0 || name.rfind(L"TextBox", 0) == 0;
}

FocusSnapshot captureFocusSnapshot() {
    FocusSnapshot snapshot;
    snapshot.foreground = GetForegroundWindow();
    GUITHREADINFO info{};
    info.cbSize = sizeof(GUITHREADINFO);
    if (GetGUIThreadInfo(0, &info)) {
        snapshot.focus = info.hwndFocus != nullptr ? info.hwndFocus : info.hwndActive;
    }
    if (snapshot.focus == nullptr) {
        snapshot.focus = snapshot.foreground;
    }
    if (snapshot.focus != nullptr) {
        snapshot.thread = GetWindowThreadProcessId(snapshot.focus, nullptr);
    }
    snapshot.editable = isEditableControl(snapshot.focus);
    return snapshot;
}

void restoreForeground(const FocusSnapshot& snapshot) {
    if (snapshot.foreground == nullptr || !IsWindow(snapshot.foreground)) {
        return;
    }
    // The picker deliberately never activates, so this is the common case;
    // do not disturb the caret by forcing a focus change.
    if (GetForegroundWindow() == snapshot.foreground) {
        return;
    }
    const DWORD current = GetCurrentThreadId();
    const DWORD target = snapshot.thread != 0
                             ? snapshot.thread
                             : GetWindowThreadProcessId(snapshot.foreground, nullptr);
    const bool attached = target != 0 && target != current &&
                          AttachThreadInput(current, target, TRUE) != FALSE;
    if (IsIconic(snapshot.foreground)) {
        ShowWindow(snapshot.foreground, SW_RESTORE);
    }
    BringWindowToTop(snapshot.foreground);
    SetForegroundWindow(snapshot.foreground);
    for (int attempt = 0;
         attempt < 12 && GetForegroundWindow() != snapshot.foreground; ++attempt) {
        Sleep(10);
    }
    if (snapshot.editable && snapshot.focus != nullptr && IsWindow(snapshot.focus)) {
        SetFocus(snapshot.focus);
    }
    if (attached) {
        AttachThreadInput(current, target, FALSE);
    }
}

bool pasteIntoFocus(const FocusSnapshot& snapshot) {
    if (!snapshot.editable || snapshot.focus == nullptr || !IsWindow(snapshot.focus) ||
        !IsWindowVisible(snapshot.focus)) {
        return false;
    }
    DWORD_PTR result = 0;
    // WM_PASTE reaches the edit control that owns the caret without any
    // foreground switch. That is what keeps Explorer's address bar in edit
    // mode. A UIPI or hung-window failure falls back to the Ctrl+V path.
    return SendMessageTimeoutW(snapshot.focus, WM_PASTE, 0, 0, SMTO_ABORTIFHUNG, 250,
                               &result) != 0;
}

QString makeChar(ushort code) { return QString(QChar(static_cast<char16_t>(code))); }

QString mapPrintableKey(DWORD vk, bool shift) {
    if (vk >= 'A' && vk <= 'Z') {
        return makeChar(static_cast<ushort>(shift ? vk : vk + 32));
    }
    if (vk >= '0' && vk <= '9') {
        const char shifted[] = ")!@#$%^&*(";
        const ushort code = shift ? static_cast<ushort>(static_cast<unsigned char>(shifted[vk - '0']))
                                  : static_cast<ushort>('0' + (vk - '0'));
        return makeChar(code);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return makeChar(static_cast<ushort>('0' + (vk - VK_NUMPAD0)));
    }
    switch (vk) {
    case VK_SPACE:
        return QStringLiteral(" ");
    case VK_OEM_MINUS:
        return makeChar(static_cast<ushort>(shift ? u'_' : u'-'));
    case VK_OEM_PLUS:
        return makeChar(static_cast<ushort>(shift ? u'+' : u'='));
    case VK_OEM_1:
        return makeChar(static_cast<ushort>(shift ? u':' : u';'));
    case VK_OEM_2:
        return makeChar(static_cast<ushort>(shift ? u'?' : u'/'));
    case VK_OEM_3:
        return makeChar(static_cast<ushort>(shift ? u'~' : u'`'));
    case VK_OEM_4:
        return makeChar(static_cast<ushort>(shift ? u'{' : u'['));
    case VK_OEM_5:
        return makeChar(static_cast<ushort>(shift ? u'|' : u'\\'));
    case VK_OEM_6:
        return makeChar(static_cast<ushort>(shift ? u'}' : u']'));
    case VK_OEM_7:
        return makeChar(static_cast<ushort>(shift ? u'"' : u'\''));
    case VK_OEM_COMMA:
        return makeChar(static_cast<ushort>(shift ? u'<' : u','));
    case VK_OEM_PERIOD:
        return makeChar(static_cast<ushort>(shift ? u'>' : u'.'));
    case VK_MULTIPLY:
        return makeChar(static_cast<ushort>(u'*'));
    case VK_ADD:
        return makeChar(static_cast<ushort>(u'+'));
    case VK_SUBTRACT:
        return makeChar(static_cast<ushort>(u'-'));
    case VK_DECIMAL:
        return makeChar(static_cast<ushort>(u'.'));
    case VK_DIVIDE:
        return makeChar(static_cast<ushort>(u'/'));
    default:
        break;
    }
    return {};
}

void sendPasteKeystroke() {
    INPUT inputs[4] = {};
    for (INPUT& input : inputs) {
        input.type = INPUT_KEYBOARD;
        // Tagged so the hook never mistakes this injected Ctrl+V for a
        // Win+V while the user is still holding the Win key down.
        input.ki.dwExtraInfo = kOwnInputTag;
    }
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].ki.wVk = 'V';
    inputs[2] = inputs[1];
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3] = inputs[0];
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    static_cast<void>(SendInput(4, inputs, sizeof(INPUT)));
}
#endif // Q_OS_WIN

} // namespace

class ClipboardHistoryController::Impl final : public QObject {
  public:
    explicit Impl(ClipboardHistoryController* owner)
        : m_owner(owner), m_store(defaultRootDirectory(), 200) {}

    ~Impl() override {
        shutdown();
        delete m_popup;
        m_popup = nullptr;
    }

    void start() {
        if (m_started) {
            return;
        }
        m_started = true;
        loadSettings();
        m_store.setMaximumEntries(m_maximumEntries);
        QString error;
        if (!m_store.load(&error)) {
            qWarning() << "Clipboard history load failed:" << error;
        }

        m_settleTimer = new QTimer(this);
        m_settleTimer->setInterval(25);
        QObject::connect(m_settleTimer, &QTimer::timeout, this, &Impl::onSettleTick);
#ifdef Q_OS_WIN
        // The shell can put the Start menu back on top after our picker is
        // shown, so keep re-asserting the topmost band while the picker is open.
        m_topmostTimer = new QTimer(this);
        m_topmostTimer->setInterval(250);
        QObject::connect(m_topmostTimer, &QTimer::timeout, this, &Impl::refreshTopmost);
#endif
        QClipboard* clipboard = QGuiApplication::clipboard();
        QObject::connect(clipboard, &QClipboard::dataChanged, this,
                         &Impl::onClipboardChanged);

#ifdef Q_OS_WIN
        m_hook = std::make_unique<WinVHotkeyHook>(
            this, [this]() { toggleHistory(); },
            [this](std::uint32_t vk, bool shift) { onPickerKey(vk, shift); },
            [this](int x, int y) { onOutsideClick(x, y); });
        m_hook->setTakeOverWinV(m_takeOverWinV);
        if (!m_hook->start()) {
            qWarning("Clipboard history Win+V takeover unavailable; "
                     "Ctrl+Shift+V fallback is also disabled");
            m_hook.reset();
        }
#endif
    }

    void shutdown() {
        if (!m_started) {
            return;
        }
        m_started = false;
        if (m_settleTimer != nullptr) {
            m_settleTimer->stop();
        }
#ifdef Q_OS_WIN
        if (m_topmostTimer != nullptr) {
            m_topmostTimer->stop();
        }
        if (m_hook != nullptr) {
            m_hook->stop();
            m_hook.reset();
        }
#endif
        static_cast<void>(m_store.save());
        closeHistory();
    }

    // Win+V is a toggle: pressing it again dismisses the picker without
    // touching the clipboard. Esc and a click outside do the same.
    void toggleHistory() {
        if (!m_enabled) {
            return;
        }
        if (m_popup != nullptr && m_popup->isVisible()) {
            closeHistory();
            return;
        }
        openHistory();
    }

    void closeHistory() {
        if (m_popup != nullptr && m_popup->isVisible()) {
            m_popup->hide();
        }
#ifdef Q_OS_WIN
        if (m_topmostTimer != nullptr) {
            m_topmostTimer->stop();
        }
        if (m_hook != nullptr) {
            m_hook->setPickerOpen(false);
        }
        m_focusSnapshot = FocusSnapshot{};
#endif
    }

    void openHistory() {
        if (!m_enabled) {
            return;
        }
        ensureWindow();
        rebuildList();
#ifdef Q_OS_WIN
        // Remember the caret owner before the picker appears. show()/raise()
        // must not activate it: the target editor keeps the foreground.
        m_focusSnapshot = captureFocusSnapshot();
#endif
        m_popup->show();
        m_popup->raise();
#ifdef Q_OS_WIN
        // Win+V is often used while the Start menu is already open, or the
        // shell raises the Start menu again when Win is released right after
        // the picker appears. Both cases leave a non-activating picker hidden
        // behind a topmost shell window, so assert HWND_TOPMOST immediately,
        // repeat on the next event-loop turns, then keep a low-frequency
        // refresh alive until the picker is dismissed.
        m_popup->ensureTopmost();
        constexpr int delays[] = {0, 60, 180};
        for (const int delay : delays) {
            QTimer::singleShot(delay, this, [this]() { refreshTopmost(); });
        }
        if (m_topmostTimer != nullptr) {
            m_topmostTimer->start();
        }
#endif
        m_search->clear();
#ifdef Q_OS_WIN
        if (m_hook != nullptr) {
            m_hook->setPickerOpen(true);
        }
#endif
    }

#ifdef Q_OS_WIN
    void refreshTopmost() {
        if (m_popup != nullptr && m_popup->isVisible()) {
            m_popup->ensureTopmost();
        }
    }
#endif

    void onOutsideClick(int x, int y) {
        if (m_popup == nullptr || !m_popup->isVisible()) {
            return;
        }
#ifdef Q_OS_WIN
        RECT rect{};
        const HWND handle = reinterpret_cast<HWND>(m_popup->winId());
        if (handle != nullptr && GetWindowRect(handle, &rect) && x >= rect.left &&
            x <= rect.right && y >= rect.top && y <= rect.bottom) {
            return;
        }
#else
        Q_UNUSED(x);
        Q_UNUSED(y);
#endif
        closeHistory();
    }

    void onPickerKey(std::uint32_t key, bool shift) {
        if (m_popup == nullptr || !m_popup->isVisible()) {
            return;
        }
#ifdef Q_OS_WIN
        switch (static_cast<DWORD>(key)) {
        case VK_ESCAPE:
            closeHistory();
            return;
        case VK_RETURN:
            pasteCurrentRow();
            return;
        case VK_UP:
            moveSelection(-1);
            return;
        case VK_DOWN:
            moveSelection(1);
            return;
        case VK_PRIOR:
            moveSelection(-5);
            return;
        case VK_NEXT:
            moveSelection(5);
            return;
        case VK_HOME:
        case VK_END:
            m_list->setCurrentRow(static_cast<DWORD>(key) == VK_HOME ? 0
                                                                     : m_list->count() - 1);
            return;
        case VK_BACK: {
            QString text = m_search->text();
            if (!text.isEmpty()) {
                text.chop(1);
                m_search->setText(text);
            }
            return;
        }
        case VK_DELETE:
            return;
        default:
            break;
        }
        const QString typed = mapPrintableKey(static_cast<DWORD>(key), shift);
        if (!typed.isEmpty()) {
            m_search->setText(m_search->text() + typed);
        }
#else
        Q_UNUSED(key);
        Q_UNUSED(shift);
#endif
    }

    void moveSelection(int delta) {
        if (m_list == nullptr || m_list->count() == 0) {
            return;
        }
        const int current = m_list->currentRow() < 0 ? 0 : m_list->currentRow();
        const int next = std::max(0, std::min(current + delta, m_list->count() - 1));
        m_list->setCurrentRow(next);
    }

    [[nodiscard]] bool enabled() const { return m_enabled; }
    [[nodiscard]] bool takeOverWinV() const { return m_takeOverWinV; }
    [[nodiscard]] int maximumEntries() const { return m_maximumEntries; }
    [[nodiscard]] int entryCount() const { return m_store.entries().size(); }

    void setEnabled(bool value) {
        if (m_enabled == value) {
            return;
        }
        m_enabled = value;
        writeSettings();
        if (!m_enabled) {
            closeHistory();
        }
#ifdef Q_OS_WIN
        // When disabled, Win+V must fall through to the system clipboard panel.
        if (m_hook != nullptr) {
            m_hook->setTakeOverWinV(m_enabled && m_takeOverWinV);
        }
#endif
    }

    void setTakeOverWinV(bool value) {
        if (m_takeOverWinV == value) {
            return;
        }
        m_takeOverWinV = value;
        writeSettings();
#ifdef Q_OS_WIN
        if (m_hook != nullptr) {
            m_hook->setTakeOverWinV(m_enabled && m_takeOverWinV);
        }
#endif
    }

    void setMaximumEntries(int value) {
        const int bounded = std::max(1, std::min(value, 1000));
        if (m_maximumEntries == bounded) {
            return;
        }
        m_maximumEntries = bounded;
        m_store.setMaximumEntries(bounded);
        writeSettings();
        rebuildList();
    }

  private:
    static QString defaultRootDirectory() {
        return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("clipboard_history"));
    }

    void loadSettings() {
        QSettings settings;
        m_enabled = settings.value(QStringLiteral("clipboard_history/enabled"), true).toBool();
        m_takeOverWinV =
            settings.value(QStringLiteral("clipboard_history/take_over_win_v"), true).toBool();
        m_maximumEntries = std::max(
            1, std::min(settings.value(QStringLiteral("clipboard_history/maximum_entries"), 200)
                            .toInt(),
                        1000));
    }

    void writeSettings() {
        QSettings settings;
        settings.setValue(QStringLiteral("clipboard_history/enabled"), m_enabled);
        settings.setValue(QStringLiteral("clipboard_history/take_over_win_v"), m_takeOverWinV);
        settings.setValue(QStringLiteral("clipboard_history/maximum_entries"), m_maximumEntries);
    }

    void ensureWindow() {
        if (m_popup != nullptr) {
            return;
        }
        m_popup = new HistoryPopup([this]() { pasteCurrentRow(); },
                                   [this]() { closeHistory(); });
        auto* layout = new QVBoxLayout(m_popup);

        m_search = new QLineEdit(m_popup);
        m_search->setPlaceholderText(trHistory("Search clipboard history"));
        layout->addWidget(m_search);

        m_list = new QListWidget(m_popup);
        layout->addWidget(m_list, 1);

        auto* hint = new QLabel(
            trHistory("Double-click or press Enter to paste; Esc / Win+V again / click outside cancels"),
            m_popup);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        auto* searchHint = new QLabel(
            trHistory("Type letters or digits to filter; Up/Down selects"), m_popup);
        searchHint->setWordWrap(true);
        layout->addWidget(searchHint);

        m_enabledBox = new QCheckBox(trHistory("Record clipboard history"), m_popup);
        m_enabledBox->setChecked(m_enabled);
        layout->addWidget(m_enabledBox);

        m_winVBox = new QCheckBox(
            trHistory("Use Win+V instead of the Windows clipboard history"), m_popup);
        m_winVBox->setChecked(m_takeOverWinV);
#ifndef Q_OS_WIN
        m_winVBox->setEnabled(false);
#endif
        layout->addWidget(m_winVBox);

        auto* options = new QHBoxLayout();
        options->addWidget(new QLabel(trHistory("Keep at most"), m_popup));
        m_maximumSpin = new QSpinBox(m_popup);
        m_maximumSpin->setRange(1, 1000);
        m_maximumSpin->setValue(m_maximumEntries);
        options->addWidget(m_maximumSpin);
        options->addWidget(new QLabel(trHistory("items"), m_popup));
        options->addStretch(1);
        auto* clearButton = new QPushButton(trHistory("Clear history"), m_popup);
        options->addWidget(clearButton);
        layout->addLayout(options);

        QObject::connect(m_search, &QLineEdit::textChanged, this, &Impl::rebuildList);
        QObject::connect(m_search, &QLineEdit::returnPressed, this,
                         [this]() { pasteCurrentRow(); });
        QObject::connect(m_list, &QListWidget::itemDoubleClicked, this,
                         [this](QListWidgetItem*) { pasteCurrentRow(); });
        QObject::connect(m_enabledBox, &QCheckBox::toggled, m_owner,
                         [this](bool value) { setEnabled(value); });
        QObject::connect(m_winVBox, &QCheckBox::toggled, m_owner,
                         [this](bool value) { setTakeOverWinV(value); });
        QObject::connect(m_maximumSpin, &QSpinBox::valueChanged, m_owner,
                         [this](int value) { setMaximumEntries(value); });
        QObject::connect(clearButton, &QPushButton::clicked, m_owner, [this]() {
            m_store.clear();
            rebuildList();
        });
    }

    void onClipboardChanged() {
        if (!m_enabled) {
            return;
        }
#ifdef Q_OS_WIN
        m_lastObservedSequence = static_cast<std::uint32_t>(GetClipboardSequenceNumber());
#else
        m_lastObservedSequence = ++m_syntheticSequence;
#endif
        const auto now = QDateTime::currentMSecsSinceEpoch();
        m_guard.noteUpdate(m_lastObservedSequence, now);
        if (!m_settleTimer->isActive()) {
            m_settleTimer->start();
        }
    }

    void onSettleTick() {
        const auto decision = m_guard.evaluate(m_lastObservedSequence,
                                               QDateTime::currentMSecsSinceEpoch());
        if (decision == ClipboardHistoryGuard::Decision::Pending) {
            return;
        }
        m_settleTimer->stop();
        if (decision == ClipboardHistoryGuard::Decision::Record) {
            captureCurrentClipboard();
        }
    }

    void captureCurrentClipboard() {
        const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
        if (mime == nullptr) {
            return;
        }
        // Respect the standard privacy formats used by password managers.
        const QStringList formats = mime->formats();
        if (formats.contains(QStringLiteral("ExcludeClipboardContentFromMonitorProcessing")) ||
            formats.contains(QStringLiteral("Clipboard Viewer Ignore"))) {
            return;
        }

        ClipboardHistoryEntry entry;
        if (mime->hasUrls()) {
            const QList<QUrl> urls = mime->urls();
            if (urls.isEmpty()) {
                return;
            }
            for (const QUrl& url : urls) {
                entry.filePaths.append(url.isLocalFile() ? url.toLocalFile() : url.toString());
            }
            entry.kind = ClipboardHistoryKind::Files;
        } else if (mime->hasImage()) {
            const QImage image = qvariant_cast<QImage>(mime->imageData());
            if (image.isNull()) {
                return;
            }
            entry.id = QString::number(QDateTime::currentMSecsSinceEpoch());
            entry.imageFile = m_store.storeImage(image, entry.id);
            if (entry.imageFile.isEmpty()) {
                return;
            }
            entry.kind = ClipboardHistoryKind::Image;
        } else if (mime->hasHtml()) {
            entry.html = mime->html();
            entry.text = mime->text();
            entry.kind = ClipboardHistoryKind::Html;
        } else if (mime->hasText()) {
            entry.text = mime->text();
            if (entry.text.trimmed().isEmpty()) {
                return;
            }
            entry.kind = ClipboardHistoryKind::Text;
        } else {
            return;
        }
        m_store.append(std::move(entry));
        rebuildList();
    }

    void pasteCurrentRow() {
        if (m_list == nullptr || m_list->currentRow() < 0 ||
            m_list->currentItem() == nullptr) {
            return;
        }
        const QVariant row = m_list->currentItem()->data(Qt::UserRole);
        if (!row.isValid()) {
            return;
        }
        pasteEntry(row.toInt());
    }

    void pasteEntry(int index) {
        const QVector<ClipboardHistoryEntry>& entries = m_store.entries();
        if (index < 0 || index >= entries.size()) {
            return;
        }
        const ClipboardHistoryEntry& entry = entries.at(index);
        auto* mime = new QMimeData();
        switch (entry.kind) {
        case ClipboardHistoryKind::Text:
            mime->setText(entry.text);
            break;
        case ClipboardHistoryKind::Html:
            mime->setHtml(entry.html);
            if (!entry.text.isEmpty()) {
                mime->setText(entry.text);
            }
            break;
        case ClipboardHistoryKind::Image: {
            QImage image;
            if (!image.load(m_store.imagePath(entry.imageFile))) {
                delete mime;
                return;
            }
            mime->setImageData(image);
            break;
        }
        case ClipboardHistoryKind::Files: {
            QList<QUrl> urls;
            urls.reserve(entry.filePaths.size());
            for (const QString& path : entry.filePaths) {
                urls.append(QUrl::fromLocalFile(path));
            }
            mime->setUrls(urls);
            break;
        }
        }
        QGuiApplication::clipboard()->setMimeData(mime);

        // Qt may emit dataChanged synchronously; noteUpdate only parks a pending
        // sequence, and the guard evaluates later on the settle timer, so the
        // marker is registered in time either way.
#ifdef Q_OS_WIN
        const auto writtenSequence = static_cast<std::uint32_t>(GetClipboardSequenceNumber());
#else
        const auto writtenSequence = ++m_syntheticSequence;
#endif
        m_guard.markOwnWrite(writtenSequence, QDateTime::currentMSecsSinceEpoch());

#ifdef Q_OS_WIN
        // closeHistory() clears the snapshot, so keep a copy first.
        const FocusSnapshot focus = m_focusSnapshot;
        const bool textLike = entry.kind == ClipboardHistoryKind::Text ||
                              entry.kind == ClipboardHistoryKind::Html;
#endif
        closeHistory();
#ifdef Q_OS_WIN
        // Let Windows process the hide, then paste into the control that owned
        // the caret. Native edit controls (Explorer address bar, Notepad, ...)
        // accept WM_PASTE directly; everything else gets a normal Ctrl+V.
        QTimer::singleShot(40, this, [this, focus, textLike]() {
            restoreForeground(focus);
            if (textLike && pasteIntoFocus(focus)) {
                return;
            }
            // Suspend the Win+V rule exactly around the injected keystroke.
            // The old 150 ms timer window is gone: a real Win+V pressed during
            // it fell through to the system panel. kOwnInputTag is the primary
            // guarantee (tagged keys always pass through the hook); this is
            // belt-and-braces wiring that also keeps the guard checks happy.
            if (m_hook != nullptr) {
                m_hook->suspendWinV(true);
            }
            sendPasteKeystroke();
            if (m_hook != nullptr) {
                m_hook->suspendWinV(false);
            }
        });
#endif
    }

    void rebuildList() {
        if (m_list == nullptr) {
            return;
        }
        const QString needle = m_search != nullptr ? m_search->text() : QString();
        m_list->clear();
        const QVector<ClipboardHistoryEntry>& entries = m_store.entries();
        for (qsizetype index = 0; index < entries.size(); ++index) {
            const ClipboardHistoryEntry& entry = entries.at(index);
            const QString preview = entry.preview();
            if (!needle.isEmpty() && !preview.contains(needle, Qt::CaseInsensitive)) {
                continue;
            }
            auto* item = new QListWidgetItem(preview, m_list);
            item->setData(Qt::UserRole, static_cast<int>(index));
            item->setToolTip(preview);
        }
        if (m_list->count() > 0) {
            m_list->setCurrentRow(0);
        }
    }

    ClipboardHistoryController* m_owner = nullptr;
    ClipboardHistoryStore m_store;
    ClipboardHistoryGuard m_guard;
    QTimer* m_settleTimer = nullptr;
    HistoryPopup* m_popup = nullptr;
    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
    QCheckBox* m_enabledBox = nullptr;
    QCheckBox* m_winVBox = nullptr;
    QSpinBox* m_maximumSpin = nullptr;
    bool m_started = false;
    bool m_enabled = true;
    bool m_takeOverWinV = true;
    int m_maximumEntries = 200;
    std::uint32_t m_lastObservedSequence = 0;
    std::uint32_t m_syntheticSequence = 0;
#ifdef Q_OS_WIN
    std::unique_ptr<WinVHotkeyHook> m_hook;
    FocusSnapshot m_focusSnapshot;
    QTimer* m_topmostTimer = nullptr;
#endif
};

ClipboardHistoryController::ClipboardHistoryController(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(this)) {}

ClipboardHistoryController::~ClipboardHistoryController() = default;

void ClipboardHistoryController::start() { m_impl->start(); }

void ClipboardHistoryController::shutdown() { m_impl->shutdown(); }

void ClipboardHistoryController::openHistory() { m_impl->openHistory(); }

bool ClipboardHistoryController::enabled() const { return m_impl->enabled(); }

void ClipboardHistoryController::setEnabled(bool value) { m_impl->setEnabled(value); }

bool ClipboardHistoryController::takeOverWinV() const { return m_impl->takeOverWinV(); }

void ClipboardHistoryController::setTakeOverWinV(bool value) { m_impl->setTakeOverWinV(value); }

int ClipboardHistoryController::maximumEntries() const { return m_impl->maximumEntries(); }

void ClipboardHistoryController::setMaximumEntries(int value) { m_impl->setMaximumEntries(value); }

int ClipboardHistoryController::entryCount() const { return m_impl->entryCount(); }

} // namespace snow_shot::presentation
