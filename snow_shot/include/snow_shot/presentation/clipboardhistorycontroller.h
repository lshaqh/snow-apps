// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_CLIPBOARDHISTORYCONTROLLER_H
#define SNOW_SHOT_PRESENTATION_CLIPBOARDHISTORYCONTROLLER_H

#include <QObject>

#include <memory>

namespace snow_shot::presentation {

// Records clipboard changes into a bounded on-disk history and opens a picker
// with Win+V (or Ctrl+Shift+V as a fallback). On Windows the Win+V takeover
// uses a low-level keyboard hook because RegisterHotKey cannot own that
// system-reserved combination; on other platforms only the recorder is active
// until a platform-specific launcher is added.
//
// Re-pasted items are filtered with ClipboardHistoryGuard so the history never
// grows from its own writes. Settings live in QSettings rather than the
// configuration schema so the feature stays self-contained.
class ClipboardHistoryController final : public QObject {
  public:
    explicit ClipboardHistoryController(QObject* parent = nullptr);
    ~ClipboardHistoryController() override;

    void start();
    void shutdown();
    void openHistory();

    [[nodiscard]] bool enabled() const;
    void setEnabled(bool value);
    [[nodiscard]] bool takeOverWinV() const;
    void setTakeOverWinV(bool value);
    [[nodiscard]] int maximumEntries() const;
    void setMaximumEntries(int value);
    [[nodiscard]] int entryCount() const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_CLIPBOARDHISTORYCONTROLLER_H
