// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/clipboardhistorystore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace snow_shot::presentation {
namespace {

constexpr int kStoreVersion = 1;
constexpr const char* kIndexFileName = "clipboard_history.json";
constexpr const char* kImageDirectoryName = "clipboard_history_images";

QString kindToString(ClipboardHistoryKind kind) {
    switch (kind) {
    case ClipboardHistoryKind::Text:
        return QStringLiteral("text");
    case ClipboardHistoryKind::Html:
        return QStringLiteral("html");
    case ClipboardHistoryKind::Image:
        return QStringLiteral("image");
    case ClipboardHistoryKind::Files:
        return QStringLiteral("files");
    }
    return QStringLiteral("text");
}

ClipboardHistoryKind kindFromString(const QString& value) {
    if (value == QLatin1String("html")) {
        return ClipboardHistoryKind::Html;
    }
    if (value == QLatin1String("image")) {
        return ClipboardHistoryKind::Image;
    }
    if (value == QLatin1String("files")) {
        return ClipboardHistoryKind::Files;
    }
    return ClipboardHistoryKind::Text;
}

QJsonObject entryToJson(const ClipboardHistoryEntry& entry) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), entry.id);
    object.insert(QStringLiteral("kind"), kindToString(entry.kind));
    object.insert(QStringLiteral("text"), entry.text);
    object.insert(QStringLiteral("html"), entry.html);
    object.insert(QStringLiteral("imageFile"), entry.imageFile);
    object.insert(QStringLiteral("filePaths"), QJsonArray::fromStringList(entry.filePaths));
    object.insert(QStringLiteral("createdUtcMs"), entry.createdUtcMs);
    return object;
}

ClipboardHistoryEntry entryFromJson(const QJsonObject& object) {
    ClipboardHistoryEntry entry;
    entry.id = object.value(QStringLiteral("id")).toString();
    entry.kind = kindFromString(object.value(QStringLiteral("kind")).toString());
    entry.text = object.value(QStringLiteral("text")).toString();
    entry.html = object.value(QStringLiteral("html")).toString();
    entry.imageFile = object.value(QStringLiteral("imageFile")).toString();
    const QJsonArray paths = object.value(QStringLiteral("filePaths")).toArray();
    for (const QJsonValue& value : paths) {
        if (value.isString()) {
            entry.filePaths.append(value.toString());
        }
    }
    entry.createdUtcMs = static_cast<std::int64_t>(
        object.value(QStringLiteral("createdUtcMs")).toDouble());
    return entry;
}

void assignError(QString* error, const QString& message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

QString ClipboardHistoryEntry::preview() const {
    if (!text.isEmpty()) {
        QString result = text.simplified();
        if (result.size() > 120) {
            result.truncate(120);
            result.append(QStringLiteral("..."));
        }
        return result;
    }
    if (!imageFile.isEmpty()) {
        return imageFile;
    }
    if (!filePaths.isEmpty()) {
        return filePaths.first();
    }
    return html.simplified().left(120);
}

ClipboardHistoryStore::ClipboardHistoryStore(QString rootDirectory, int maximumEntries)
    : m_rootDirectory(std::move(rootDirectory)),
      m_maximumEntries(std::max(1, maximumEntries)) {}

QString ClipboardHistoryStore::imageDirectory() const {
    return QDir(m_rootDirectory).filePath(QStringLiteral(kImageDirectoryName));
}

QString ClipboardHistoryStore::imagePath(const QString& imageFile) const {
    if (imageFile.isEmpty()) {
        return {};
    }
    return QDir(imageDirectory()).filePath(QFileInfo(imageFile).fileName());
}

bool ClipboardHistoryStore::load(QString* error) {
    QFile file(QDir(m_rootDirectory).filePath(QStringLiteral(kIndexFileName)));
    if (!file.exists()) {
        m_entries.clear();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        assignError(error, QStringLiteral("Cannot open clipboard history index for reading"));
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (document.isNull() || !document.object().value(QStringLiteral("version")).isDouble()) {
        assignError(error, QStringLiteral("Clipboard history index is not valid JSON"));
        return false;
    }
    const QJsonArray array = document.object().value(QStringLiteral("entries")).toArray();
    QVector<ClipboardHistoryEntry> loaded;
    loaded.reserve(array.size());
    for (const QJsonValue& value : array) {
        if (!value.isObject()) {
            continue;
        }
        ClipboardHistoryEntry entry = entryFromJson(value.toObject());
        if (entry.id.isEmpty()) {
            continue;
        }
        loaded.append(std::move(entry));
    }
    m_entries = std::move(loaded);
    if (m_entries.size() > m_maximumEntries) {
        m_entries.resize(m_maximumEntries);
        static_cast<void>(save(error));
    }
    return true;
}

bool ClipboardHistoryStore::save(QString* error) const {
    if (!QDir().mkpath(m_rootDirectory)) {
        assignError(error, QStringLiteral("Cannot create clipboard history directory"));
        return false;
    }
    QJsonArray array;
    for (const ClipboardHistoryEntry& entry : m_entries) {
        array.append(entryToJson(entry));
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kStoreVersion);
    root.insert(QStringLiteral("entries"), array);

    QSaveFile file(QDir(m_rootDirectory).filePath(QStringLiteral(kIndexFileName)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        assignError(error, QStringLiteral("Cannot open clipboard history index for writing"));
        return false;
    }
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0 || !file.commit()) {
        assignError(error, QStringLiteral("Cannot write clipboard history index"));
        return false;
    }
    return true;
}

void ClipboardHistoryStore::append(ClipboardHistoryEntry entry) {
    if (entry.id.isEmpty()) {
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (entry.createdUtcMs == 0) {
        entry.createdUtcMs = QDateTime::currentMSecsSinceEpoch();
    }
    // Newest first. Drop an older duplicate of the same content so repeated
    // Ctrl+C of an unchanged selection does not consume the retention budget.
    for (qsizetype index = 0; index < m_entries.size();) {
        const ClipboardHistoryEntry& existing = m_entries.at(index);
        if (existing.kind == entry.kind && existing.text == entry.text &&
            existing.html == entry.html && existing.imageFile == entry.imageFile &&
            existing.filePaths == entry.filePaths) {
            m_entries.removeAt(index);
            continue;
        }
        ++index;
    }
    m_entries.prepend(std::move(entry));
    while (m_entries.size() > m_maximumEntries) {
        m_entries.removeLast();
    }
    static_cast<void>(save());
    removeImagesNotReferenced();
}

void ClipboardHistoryStore::clear() {
    m_entries.clear();
    static_cast<void>(save());
    removeImagesNotReferenced();
}

void ClipboardHistoryStore::setMaximumEntries(int value) {
    m_maximumEntries = std::max(1, value);
    if (m_entries.size() > m_maximumEntries) {
        m_entries.resize(m_maximumEntries);
        static_cast<void>(save());
        removeImagesNotReferenced();
    }
}

QString ClipboardHistoryStore::storeImage(const QImage& image, const QString& id) {
    if (image.isNull()) {
        return {};
    }
    const QString directory = imageDirectory();
    if (!QDir().mkpath(directory)) {
        return {};
    }
    const QString fileName = id + QStringLiteral(".png");
    if (!image.save(QDir(directory).filePath(fileName), "PNG")) {
        return {};
    }
    return fileName;
}

void ClipboardHistoryStore::removeImagesNotReferenced() {
    QDir directory(imageDirectory());
    if (!directory.exists()) {
        return;
    }
    QSet<QString> referenced;
    for (const ClipboardHistoryEntry& entry : m_entries) {
        if (!entry.imageFile.isEmpty()) {
            referenced.insert(QFileInfo(entry.imageFile).fileName());
        }
    }
    const QStringList filters{QStringLiteral("*.png")};
    for (const QString& name : directory.entryList(filters, QDir::Files)) {
        if (!referenced.contains(name)) {
            directory.remove(name);
        }
    }
}

} // namespace snow_shot::presentation
