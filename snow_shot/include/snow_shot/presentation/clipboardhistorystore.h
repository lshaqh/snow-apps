// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_CLIPBOARDHISTORYSTORE_H
#define SNOW_SHOT_PRESENTATION_CLIPBOARDHISTORYSTORE_H

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace snow_shot::presentation {

enum class ClipboardHistoryKind {
    Text,
    Html,
    Image,
    Files,
};

struct ClipboardHistoryEntry {
    QString id;
    ClipboardHistoryKind kind = ClipboardHistoryKind::Text;
    QString text;
    QString html;
    // File name relative to ClipboardHistoryStore::imageDirectory().
    QString imageFile;
    QStringList filePaths;
    std::int64_t createdUtcMs = 0;

    [[nodiscard]] QString preview() const;
};

// JSON index plus one PNG per image entry. Kept separate from the screenshot
// history so clipboard data follows its own retention policy.
class ClipboardHistoryStore final {
  public:
    explicit ClipboardHistoryStore(QString rootDirectory, int maximumEntries = 200);

    ClipboardHistoryStore(const ClipboardHistoryStore&) = delete;
    ClipboardHistoryStore& operator=(const ClipboardHistoryStore&) = delete;

    [[nodiscard]] bool load(QString* error = nullptr);
    [[nodiscard]] bool save(QString* error = nullptr) const;

    void append(ClipboardHistoryEntry entry);
    void clear();
    void setMaximumEntries(int value);

    [[nodiscard]] const QVector<ClipboardHistoryEntry>& entries() const { return entries_; }
    [[nodiscard]] int maximumEntries() const { return maximumEntries_; }
    [[nodiscard]] QString imageDirectory() const;
    [[nodiscard]] QString imagePath(const QString& imageFile) const;

    // Writes `image` as PNG and returns its relative file name, or an empty
    // string when the write failed.
    [[nodiscard]] QString storeImage(const QImage& image, const QString& id);

  private:
    void removeImagesNotReferenced();

    QString m_rootDirectory;
    int m_maximumEntries = 200;
    QVector<ClipboardHistoryEntry> m_entries;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_CLIPBOARDHISTORYSTORE_H
