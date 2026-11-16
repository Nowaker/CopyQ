// SPDX-License-Identifier: GPL-3.0-or-later
#include "itemeviction.h"

#include "common/mimetypes.h"
#include "item/itemlabel.h"
#include "item/serialize.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace {

constexpr int newestRowsKept = 20;
constexpr double recentDaysKept = 1.0;
constexpr double scoreAgeScaleDays = 30.0;

} // namespace

bool isBinaryDataFormat(const QString &format)
{
    return !format.startsWith(QLatin1String("text/"))
        && !format.startsWith(QLatin1String(COPYQ_MIME_PREFIX));
}

BinaryDataUsage binaryDataUsage(const QVariantMap &data, const QDateTime &now, const QDateTime &addedTime)
{
    BinaryDataUsage usage;
    bool hasCopiedAt = false;
    const qint64 copiedAtMs = data.value(mimeCopiedAt).toByteArray().toLongLong(&hasCopiedAt);
    const QDateTime copiedAt = hasCopiedAt ? QDateTime::fromMSecsSinceEpoch(copiedAtMs) : QDateTime();
    bool hasAge = false;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if ( !isBinaryDataFormat(it.key()) )
            continue;

        usage.bytes += itemDataSize(it.value());

        QDateTime saved = copiedAt.isValid() ? copiedAt : itemDataSavedTime(it.value());
        if ( !saved.isValid() )
            saved = addedTime;
        const double ageDays = saved.isValid() ? saved.secsTo(now) / 86400.0 : 0.0;
        usage.ageDays = hasAge ? std::min(usage.ageDays, ageDays) : ageDays;
        hasAge = true;
    }
    return usage;
}

QList<int> rowsToEvictBinaryData(const QList<BinaryDataUsage> &items, qint64 limitBytes)
{
    if (limitBytes < 0)
        return {};

    qint64 totalBytes = 0;
    for (const auto &item : items)
        totalBytes += item.bytes;
    if (totalBytes <= limitBytes)
        return {};

    struct Candidate { int row; double score; };
    QList<Candidate> candidates;
    for (int row = newestRowsKept; row < items.size(); ++row) {
        const auto &item = items[row];
        if (item.keep || item.bytes <= 0 || item.ageDays < recentDaysKept)
            continue;
        candidates.append({row, item.bytes * (1.0 + item.ageDays / scoreAgeScaleDays)});
    }

    // Highest score first; on a tie the older (lower) row first.
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        if (a.score > b.score || b.score > a.score)
            return a.score > b.score;
        return a.row > b.row;
    });

    const qint64 targetBytes = limitBytes / 10 * 9;
    QList<int> rows;
    for (const auto &candidate : candidates) {
        if (totalBytes <= targetBytes)
            break;
        rows.append(candidate.row);
        totalBytes -= items[candidate.row].bytes;
    }
    return rows;
}

QVariantMap withoutBinaryData(const QVariantMap &data)
{
    QJsonObject evicted = QJsonDocument::fromJson(data.value(mimeEvictedFormats).toByteArray()).object();
    if ( evicted.value(QLatin1String("label")).toString().isEmpty() )
        evicted.insert(QLatin1String("label"), itemLabel(data));

    QJsonArray formats = evicted.value(QLatin1String("formats")).toArray();
    QVariantMap newData;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if ( !isBinaryDataFormat(it.key()) ) {
            newData.insert(it.key(), it.value());
            continue;
        }

        const QByteArray bytes = it.value().toByteArray();
        formats.append(QJsonObject{
            {QLatin1String("format"), it.key()},
            {QLatin1String("size"), static_cast<double>(bytes.size())},
            {QLatin1String("sha256"), QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
        });
    }

    evicted.insert(QLatin1String("formats"), formats);
    newData.insert(mimeEvictedFormats, QJsonDocument(evicted).toJson(QJsonDocument::Compact));
    return newData;
}
