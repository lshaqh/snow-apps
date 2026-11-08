// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tray-resident host for the Win+V clipboard-history controller. It links the
// very same translation units that snow_shot::ApplicationController starts,
// so the standalone build and the integrated build cannot drift apart.

#include "snow_shot/presentation/clipboardhistorycontroller.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QTranslator>

#include <cstring>

namespace {

// The portable build ships no .qm files (windeployqt runs with
// --no-translations), so QCoreApplication::translate() would otherwise fall
// back to the English source strings. Install a compiled-in Simplified
// Chinese catalogue for the ClipboardHistoryWindow context instead. The
// integrated snow_shot build loads its real .qm catalogues and is unaffected.
class ChineseTranslator final : public QTranslator {
  public:
    bool isEmpty() const override { return false; }

    QString translate(const char* context, const char* sourceText,
                      const char* /*disambiguation*/ = nullptr,
                      int /*n*/ = -1) const override {
        if (context == nullptr || sourceText == nullptr ||
            std::strcmp(context, "ClipboardHistoryWindow") != 0) {
            return {};
        }
        return catalogue().value(QString::fromUtf8(sourceText));
    }

  private:
    static const QHash<QString, QString>& catalogue() {
        static const QHash<QString, QString> map = {
            {QStringLiteral("Clipboard history"), QStringLiteral("剪贴板历史")},
            {QStringLiteral("Search clipboard history"), QStringLiteral("搜索剪贴板历史")},
            {QStringLiteral("Double-click or press Enter to paste; Esc / Win+V again / click outside cancels"),
             QStringLiteral("双击或按回车粘贴；按 Esc、再按一次 Win+V 或点击窗口外取消")},
            {QStringLiteral("Type letters or digits to filter; Up/Down selects"),
             QStringLiteral("输入字母或数字筛选，上下键选择")},
            {QStringLiteral("Record clipboard history"), QStringLiteral("记录剪贴板历史")},
            {QStringLiteral("Use Win+V instead of the Windows clipboard history"),
             QStringLiteral("使用 Win+V 替代 Windows 自带剪贴板历史")},
            {QStringLiteral("Keep at most"), QStringLiteral("最多保留")},
            {QStringLiteral("items"), QStringLiteral("条")},
            {QStringLiteral("Clear history"), QStringLiteral("清空历史")},
        };
        return map;
    }
};

QIcon makeTrayIcon() {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x2d, 0x7f, 0xf9));
    painter.drawRoundedRect(4, 4, 56, 56, 14, 14);

    QFont font = painter.font();
    font.setPixelSize(34);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("V"));
    painter.end();

    return QIcon(pixmap);
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Must be installed before any widget is created: the picker's labels,
    // placeholder and checkboxes resolve their text at construction time.
    ChineseTranslator translator;
    QCoreApplication::installTranslator(&translator);

    QApplication::setOrganizationName(QStringLiteral("snow-apps"));
    QApplication::setApplicationName(QStringLiteral("SnowShotClipboardHistory"));
    QApplication::setQuitOnLastWindowClosed(false);

    snow_shot::presentation::ClipboardHistoryController controller;
    controller.start();

    QSystemTrayIcon tray(makeTrayIcon());
    tray.setToolTip(QStringLiteral("Snow Shot 剪贴板历史 - Win+V / Ctrl+Shift+V"));

    QMenu menu;
    QAction* openAction = menu.addAction(QStringLiteral("打开剪贴板历史"));
    QAction* takeOverAction = menu.addAction(QStringLiteral("接管 Win+V"));
    takeOverAction->setCheckable(true);
    takeOverAction->setChecked(controller.takeOverWinV());
    menu.addSeparator();
    QAction* quitAction = menu.addAction(QStringLiteral("退出"));

    QObject::connect(openAction, &QAction::triggered, &controller,
                     [&controller]() { controller.openHistory(); });
    QObject::connect(takeOverAction, &QAction::toggled, &controller,
                     [&controller](bool checked) { controller.setTakeOverWinV(checked); });
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &controller,
                     [&controller](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger ||
                             reason == QSystemTrayIcon::DoubleClick) {
                             controller.openHistory();
                         }
                     });

    tray.setContextMenu(&menu);
    tray.show();

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::information(
            nullptr, QStringLiteral("Snow Shot 剪贴板历史"),
            QStringLiteral("未检测到系统托盘，程序仍会在后台记录剪贴板。\n"
                           "可用 Ctrl+Shift+V 唤起历史窗口。"));
    }

    const int result = app.exec();
    controller.shutdown();
    return result;
}
