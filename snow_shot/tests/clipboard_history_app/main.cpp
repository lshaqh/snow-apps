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
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>

namespace {

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
