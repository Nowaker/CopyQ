// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <optional>

/**
 * Pieces of the Flycut sync format (FlycutEngine/FlycutFolderSync.h) that don't need
 * the network or the sync state: event ordering, item identity, pasteboard types.
 */
namespace FlycutSyncFormat {

enum List { HistoryList = 0, FavoritesList = 1 };

/// Item data formats the sync keeps in CopyQ items (private, so they don't change item hashes).
extern const QLatin1String mimeClipId;
extern const QLatin1String mimeRank;
extern const QLatin1String mimeSignature;
/// Where and when a clipping was copied: machine, app (as the event's "m" and "app") and time in ms (ASCII digits).
extern const QLatin1String mimeMachine;
extern const QLatin1String mimeApp;
extern const QLatin1String mimeCopiedAt;

/// Whether a change made at (at, machine) beats one made at (otherAt, otherMachine).
bool isNewer(qint64 at, const QString &machine, qint64 otherAt, const QString &otherMachine);

/// macOS pasteboard type Flycut stores for a MIME type; other MIME types travel under their own name.
QString pasteboardType(const QString &mime);

/// MIME type for a pasteboard type, or empty if CopyQ has no use for it.
QString mimeForPasteboardType(const QString &type);

/// The formats of item data that are shared: everything but CopyQ's own.
QVariantMap syncedFormats(const QVariantMap &data);

/**
 * What tells items apart, as flycut-copyq-bridge did: the text, or for an item without
 * text "\x01" + the SHA-256 of its first format by name.
 */
QString itemKey(const QVariantMap &formats);

/// SHA-256 (hex) of itemKey(), the "hash" the sync state keeps per clipping.
QString keyHash(const QVariantMap &formats);

QString sha256Hex(const QByteArray &data);

bool isBlobAddress(const QString &text);

/// Blob addresses that the put events of a log refer to.
QSet<QString> blobAddresses(const QByteArray &log);

struct EvictedFormat {
    QString format;
    qint64 size = 0;
    QString sha256;
};

/// Formats whose data was dropped from the item to save space, with the SHA-256 of the data.
QVector<EvictedFormat> evictedFormats(const QVariantMap &data);

/**
 * Fingerprint of an item's shared content: format names and SHA-256 of their data,
 * evicted formats included as if still present, so evicting data doesn't look like an edit.
 */
QByteArray contentSignature(const QVariantMap &data);

/**
 * Rank for an item placed between two others, as Flycut ranks: midpoint between neighbours,
 * one below the newer one at the bottom, and the current time (or just above the older one) on top.
 */
double rankBetween(std::optional<double> newerRank, std::optional<double> olderRank, double now);

/**
 * Row for an item of the given rank in a list whose rows have these ranks (newest first).
 * Rows without a rank (items from before sync) are skipped over by recent items (rank within
 * the last hour, which go above them) and kept above older ones, as the bridge placed them.
 */
int rowForRank(const QVector<std::optional<double>> &ranks, double rank, double now);

} // namespace FlycutSyncFormat
