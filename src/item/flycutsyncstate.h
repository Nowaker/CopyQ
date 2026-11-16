// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QSqlDatabase>
#include <QString>

#include <optional>

/**
 * What the Flycut sync remembers between runs, in <state dir>/state.sqlite:
 *
 * - clips: the newest change seen for every clipping of a list (time, machine, deleted)
 *   and the hash of its item key, to recognise items that carry no clipping id;
 * - meta: this machine's id, read positions in other machines' logs ("read:<machine>"),
 *   how much of the own log the server has ("sent"), clear markers ("clear:<list>")
 *   and which credential rules version the own log was sealed with ("sealed:<version>");
 * - blobs: where on the server the data with a given SHA-256 can be fetched again.
 *
 * The meta keys and the clips columns follow flycut-copyq-bridge's state, which
 * migrateFromBridge() takes over.
 */
class FlycutSyncState final
{
public:
    struct Clip {
        qint64 at = 0;
        QString machine;
        bool deleted = false;
        QString hash;
    };

    struct Blob {
        QString machine;
        QString address;
        bool sealed = false;
    };

    FlycutSyncState() = default;
    ~FlycutSyncState();
    FlycutSyncState(const FlycutSyncState &) = delete;
    FlycutSyncState &operator=(const FlycutSyncState &) = delete;

    /// Opens (creating if needed) state.sqlite in the directory; the connection belongs to the calling thread.
    bool open(const QString &stateDir, QString *error);

    bool isOpen() const;

    bool transaction();
    bool commit();
    void rollback();

    QString meta(const QString &key) const;
    void setMeta(const QString &key, const QString &value);

    std::optional<Clip> clip(int list, const QString &id) const;
    void setClip(int list, const QString &id, const Clip &clip);

    /// Newest live clipping with the item key hash.
    QString liveIdForHash(int list, const QString &hash) const;

    /// Item key hash -> clipping id of live clippings (the newest for each hash).
    QHash<QString, QString> liveHashes(int list) const;

    /// Live clippings last changed before (at, machine): what a clear removes. Id -> hash.
    QHash<QString, QString> liveClipsOlderThan(int list, qint64 at, const QString &machine) const;

    /// Marks clippings deleted without changing their time (a clear covers them).
    void markDeleted(int list, const QStringList &ids);

    /// Newest change time in the list.
    qint64 latestChange(int list) const;

    std::optional<Blob> blob(const QString &sha256) const;
    void setBlob(const QString &sha256, const Blob &blob);

    /**
     * Takes over flycut-copyq-bridge's state directory: machine id, read positions, upload
     * position, clear marker, sealing marker, clippings, own log and own blobs. Does nothing
     * (and returns true) if this state already has a machine id or the directory has no state.
     */
    bool migrateFromBridge(const QString &bridgeDir, const QString &stateDir, QString *error);

private:
    QString m_connectionName;
};
