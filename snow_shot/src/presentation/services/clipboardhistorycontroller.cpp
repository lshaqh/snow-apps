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
#include <functional>
#include <memory>
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
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setWindowTitle(trHistory("Clipboard history"));
        resize(520, 460);
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

// Low-level keyboard hook owns Win+V (system-reserved for RegisterHotKey) and
// the Ctrl+Shift+V fallback. The hook lives on a dedicated thread with a Qt
// event loop, following the PrintScreenShortcutRecorder pattern.
class WinVHotkeyHook final {
  public:
    using Activation = std::function<void()>;

    WinVHotkeyHook(QObject* owner, Activation activation)
        : m_owner(owner), m_activation(std::move(activation)) {}

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
            if (m_hook == nullptr) {
                qWarning("Clipboard history Win+V hook failed: %lu",
                         static_cast<unsigned long>(GetLastError()));
                active_.store(nullptr, std::memory_order_release);
            } else {
                m_installed.store(true, std::memory_order_release);
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

  private:
    static bool isControl(DWORD vk) {
        return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL;
    }

    static bool isShift(DWORD vk) {
        return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
    }

    static LRESULT CALLBACK keyboardProc(int code, WPARAM message, LPARAM data) {
        WinVHotkeyHook* self = active_.load(std::memory_order_acquire);
        if (code != HC_ACTION || self == nullptr || data == 0) {
            return CallNextHookEx(nullptr, code, message, data);
        }
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        const DWORD vk = key->vkCode;
        if (vk == kMenuMaskVk) {
            return 1; // our own mask must not reach anyone
        }
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;

        if (vk == VK_LWIN || vk == VK_RWIN) {
            if (down) {
                self->m_winDown = true;
                self->m_winConsumed = false;
            } else if (up) {
                self->m_winDown = false;
                // Only mask a Win press that actually consumed Win+V, so
                // Win+D / Win+E keep their normal behaviour.
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
        if (vk == 'V') {
            const bool winV = self->m_winDown &&
                              self->m_takeOverWinV.load(std::memory_order_acquire);
            const bool fallback = self->m_controlDown && self->m_shiftDown;
            if (winV || fallback) {
                if (down) {
                    if (winV) {
                        self->m_winConsumed = true;
                    }
                    self->notifyActivation();
                }
                // Swallow both V down and V up: the system Win+V panel never sees it.
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    void sendMenuMask() {
        INPUT inputs[2] = {};
        for (INPUT& input : inputs) {
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = kMenuMaskVk;
        }
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        static_cast<void>(SendInput(2, inputs, sizeof(INPUT)));
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

    static std::atomic<WinVHotkeyHook*> active_;
    QPointer<QObject> m_owner;
    Activation m_activation;
    QThread m_thread;
    QObject* m_worker = nullptr;
    HHOOK m_hook = nullptr;
    std::atomic<bool> m_installed{false};
    std::atomic<bool> m_takeOverWinV{true};
    bool m_winDown = false;
    bool m_winConsumed = false;
    bool m_controlDown = false;
    bool m_shiftDown = false;
};

std::atomic<WinVHotkeyHook*> WinVHotkeyHook::active_{nullptr};

void sendPasteKeystroke() {
    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
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
        QClipboard* clipboard = QGuiApplication::clipboard();
        QObject::connect(clipboard, &QClipboard::dataChanged, this,
                         &Impl::onClipboardChanged);

#ifdef Q_OS_WIN
        m_hook = std::make_unique<WinVHotkeyHook>(this, [this]() { openHistory(); });
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
        if (m_hook != nullptr) {
            m_hook->stop();
            m_hook.reset();
        }
#endif
        static_cast<void>(m_store.save());
        if (m_popup != nullptr) {
            m_popup->hide();
        }
    }

    void openHistory() {
        if (!m_enabled) {
            return;
        }
        ensureWindow();
        rebuildList();
#ifdef Q_OS_WIN
        m_previousForeground = GetForegroundWindow();
#endif
        m_popup->show();
        m_popup->raise();
        m_popup->activateWindow();
        m_search->clear();
        m_search->setFocus();
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
        m_popup = new HistoryPopup([this]() { pasteCurrentRow(); }, []() {});
        auto* layout = new QVBoxLayout(m_popup);

        m_search = new QLineEdit(m_popup);
        m_search->setPlaceholderText(trHistory("Search clipboard history"));
        layout->addWidget(m_search);

        m_list = new QListWidget(m_popup);
        layout->addWidget(m_list, 1);

        auto* hint = new QLabel(trHistory("Double-click an item or press Enter to paste"),
                                m_popup);
        layout->addWidget(hint);

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

        if (m_popup != nullptr) {
            m_popup->hide();
        }
#ifdef Q_OS_WIN
        if (m_previousForeground != nullptr) {
            SetForegroundWindow(m_previousForeground);
        }
        sendPasteKeystroke();
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
    HWND m_previousForeground = nullptr;
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
