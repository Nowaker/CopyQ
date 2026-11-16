// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QList>
#include <QVariantMap>


/**
 * Keeps the binary (non-text) data of a tab under a size limit by dropping
 * the binary formats of bulky old items; the items themselves stay, with their
 * text, and their label in mimeEvictedFormats.
 */

struct BinaryDataUsage {
    qint64 bytes = 0;
    double ageDays = 0;
    // Pinned items, or others that must not lose data.
    bool keep = false;
};

/// Formats counted as binary data: everything except text/* and CopyQ's own formats.
bool isBinaryDataFormat(const QString &format);

/**
 * Binary data size of an item and its age: from its copy time (mimeCopiedAt),
 * else from when its data was saved to data files, else from @a addedTime
 * (if invalid, the data is new).
 */
BinaryDataUsage binaryDataUsage(const QVariantMap &data, const QDateTime &now, const QDateTime &addedTime = {});

/**
 * Rows (newest first) whose binary data to evict when the total exceeds the limit,
 * highest score first until the total is under 90% of the limit:
 * score = bytes * (1 + age_days / 30). Never the 20 newest rows, anything saved
 * within the last 24 hours, or items to keep.
 */
QList<int> rowsToEvictBinaryData(const QList<BinaryDataUsage> &items, qint64 limitBytes);

/**
 * Item data without its binary formats, recording them in mimeEvictedFormats
 * (format, size, SHA-256) along with the item's label before eviction.
 */
QVariantMap withoutBinaryData(const QVariantMap &data);
