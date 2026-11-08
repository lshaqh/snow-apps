// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/clipboardhistorycontroller.h"

#include <QCoreApplication>

// Link-time reachability probe. The listener is intentionally not started:
// a CI runner has no interactive desktop session or usable clipboard.
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    snow_shot::presentation::ClipboardHistoryController controller;
    (void)controller.enabled();
    (void)controller.maximumEntries();
    (void)controller.entryCount();
    return 0;
}
