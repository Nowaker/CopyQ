// SPDX-License-Identifier: GPL-3.0-or-later
#include "flycutsyncstate.h"

#include "item/flycutsyncformat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {

QString errorText(const QSqlQuery &query)
{
    return query.lastError().text();
}

bool copyFileReplacing(const QString &from, const QString &to)
{
    const QString temporary = to + QStringLiteral(".migrating");
    QFile::remove(temporary);
    if ( !QFile::copy(from, temporary) )
        return false;
    QFile::setPermissions(temporary, QFile::ReadOwner | QFile::WriteOwner);
    QFile::remove(to);
    return QFile::rename(temporary, to);
}

} // namespace

FlycutSyncState::~FlycutSyncState()
{
    if ( m_connectionName.isEmpty() )
        return;
    QSqlDatabase::database(m_connectionName, false).close();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool FlycutSyncState::open(const QString &stateDir, QString *error)
{
    if ( !QDir().mkpath(stateDir) ) {
        *error = QStringLiteral("cannot create %1").arg(stateDir);
        return false;
    }
    QFile::setPermissions(stateDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    m_connectionName = QStringLiteral("flycut-sync-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    const QString path = QDir(stateDir).filePath(QStringLiteral("state.sqlite"));
    db.setDatabaseName(path);
    if ( !db.open() ) {
        *error = QStringLiteral("cannot open %1: %2").arg(path, db.lastError().text());
        return false;
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);

    QSqlQuery query(db);
    const QStringList schema{
        QStringLiteral("PRAGMA journal_mode=WAL"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS clips (list INTEGER NOT NULL, id TEXT NOT NULL,"
                       " at INTEGER NOT NULL, machine TEXT NOT NULL, deleted INTEGER NOT NULL, hash TEXT,"
                       " PRIMARY KEY (list, id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS clips_by_hash ON clips (list, hash)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS blobs (sha256 TEXT PRIMARY KEY, machine TEXT NOT NULL,"
                       " address TEXT NOT NULL, sealed INTEGER NOT NULL)"),
    };
    for (const auto &statement : schema) {
        if ( !query.exec(statement) ) {
            *error = QStringLiteral("cannot prepare %1: %2").arg(path, errorText(query));
            return false;
        }
    }
    return true;
}

bool FlycutSyncState::isOpen() const
{
    return !m_connectionName.isEmpty() && QSqlDatabase::database(m_connectionName, false).isOpen();
}

bool FlycutSyncState::transaction()
{
    return QSqlDatabase::database(m_connectionName, false).transaction();
}

bool FlycutSyncState::commit()
{
    return QSqlDatabase::database(m_connectionName, false).commit();
}

void FlycutSyncState::rollback()
{
    QSqlDatabase::database(m_connectionName, false).rollback();
}

QString FlycutSyncState::meta(const QString &key) const
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    query.addBindValue(key);
    if ( query.exec() && query.next() )
        return query.value(0).toString();
    return {};
}

void FlycutSyncState::setMeta(const QString &key, const QString &value)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)"));
    query.addBindValue(key);
    query.addBindValue(value);
    query.exec();
}

std::optional<FlycutSyncState::Clip> FlycutSyncState::clip(int list, const QString &id) const
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("SELECT at, machine, deleted, hash FROM clips WHERE list = ? AND id = ?"));
    query.addBindValue(list);
    query.addBindValue(id);
    if ( !query.exec() || !query.next() )
        return std::nullopt;
    Clip clip;
    clip.at = query.value(0).toLongLong();
    clip.machine = query.value(1).toString();
    clip.deleted = query.value(2).toInt() != 0;
    clip.hash = query.value(3).toString();
    return clip;
}

void FlycutSyncState::setClip(int list, const QString &id, const Clip &clip)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO clips (list, id, at, machine, deleted, hash) VALUES (?, ?, ?, ?, ?, ?)"));
    query.addBindValue(list);
    query.addBindValue(id);
    query.addBindValue(clip.at);
    query.addBindValue(clip.machine);
    query.addBindValue(clip.deleted ? 1 : 0);
    query.addBindValue(clip.hash.isEmpty() ? QVariant() : QVariant(clip.hash));
    query.exec();
}

QString FlycutSyncState::liveIdForHash(int list, const QString &hash) const
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("SELECT id FROM clips WHERE list = ? AND hash = ? AND deleted = 0 ORDER BY at DESC LIMIT 1"));
    query.addBindValue(list);
    query.addBindValue(hash);
    if ( query.exec() && query.next() )
        return query.value(0).toString();
    return {};
}

QHash<QString, QString> FlycutSyncState::liveHashes(int list) const
{
    QHash<QString, QString> hashes;
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("SELECT hash, id FROM clips WHERE list = ? AND deleted = 0 AND hash IS NOT NULL ORDER BY at"));
    query.addBindValue(list);
    if ( query.exec() ) {
        // Ordered by time, so the newest clipping of a hash wins.
        while ( query.next() )
            hashes.insert(query.value(0).toString(), query.value(1).toString());
    }
    return hashes;
}

QHash<QString, QString> FlycutSyncState::liveClipsOlderThan(int list, qint64 at, const QString &machine) const
{
    QHash<QString, QString> clips;
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("SELECT id, hash FROM clips WHERE list = ? AND deleted = 0 AND (at < ? OR (at = ? AND machine < ?))"));
    query.addBindValue(list);
    query.addBindValue(at);
    query.addBindValue(at);
    query.addBindValue(machine);
    if ( query.exec() ) {
        while ( query.next() )
            clips.insert(query.value(0).toString(), query.value(1).toString());
    }
    return clips;
}

void FlycutSyncState::markDeleted(int list, const QStringList &ids)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("UPDATE clips SET deleted = 1 WHERE list = ? AND id = ?"));
    for (const auto &id : ids) {
        query.addBindValue(list);
        query.addBindValue(id);
        query.exec();
    }
}

qint64 FlycutSyncState::latestChange(int list) const
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("SELECT MAX(at) FROM clips WHERE list = ?"));
    query.addBindValue(list);
    if ( query.exec() && query.next() )
        return query.value(0).toLongLong();
    return 0;
}

std::optional<FlycutSyncState::Blob> FlycutSyncState::blob(const QString &sha256) const
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("SELECT machine, address, sealed FROM blobs WHERE sha256 = ?"));
    query.addBindValue(sha256);
    if ( !query.exec() || !query.next() )
        return std::nullopt;
    Blob blob;
    blob.machine = query.value(0).toString();
    blob.address = query.value(1).toString();
    blob.sealed = query.value(2).toInt() != 0;
    return blob;
}

void FlycutSyncState::setBlob(const QString &sha256, const Blob &blob)
{
    QSqlQuery query(QSqlDatabase::database(m_connectionName, false));
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO blobs (sha256, machine, address, sealed) VALUES (?, ?, ?, ?)"));
    query.addBindValue(sha256);
    query.addBindValue(blob.machine);
    query.addBindValue(blob.address);
    query.addBindValue(blob.sealed ? 1 : 0);
    query.exec();
}

bool FlycutSyncState::migrateFromBridge(const QString &bridgeDir, const QString &stateDir, QString *error)
{
    const QString bridgeStatePath = QDir(bridgeDir).filePath(QStringLiteral("state.sqlite"));
    if ( !meta(QStringLiteral("machine")).isEmpty() || bridgeDir.isEmpty() || !QFileInfo::exists(bridgeStatePath) )
        return true;

    const QString connectionName = m_connectionName + QStringLiteral("-bridge");
    struct BridgeConnection {
        QString name;
        ~BridgeConnection() {
            QSqlDatabase::database(name, false).close();
            QSqlDatabase::removeDatabase(name);
        }
    } bridgeConnection{connectionName};

    QSqlDatabase bridgeDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    bridgeDb.setDatabaseName(bridgeStatePath);
    bridgeDb.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if ( !bridgeDb.open() ) {
        *error = QStringLiteral("cannot open %1: %2").arg(bridgeStatePath, bridgeDb.lastError().text());
        return false;
    }

    QSqlQuery bridgeQuery(bridgeDb);
    QHash<QString, QString> bridgeMeta;
    if ( !bridgeQuery.exec(QStringLiteral("SELECT key, value FROM meta")) ) {
        *error = QStringLiteral("cannot read %1: %2").arg(bridgeStatePath, errorText(bridgeQuery));
        return false;
    }
    while ( bridgeQuery.next() )
        bridgeMeta.insert(bridgeQuery.value(0).toString(), bridgeQuery.value(1).toString());

    const QString machine = bridgeMeta.value(QStringLiteral("machine"));
    if ( machine.isEmpty() )
        return true;

    // Files first: if anything below fails, the next start migrates again over them.
    const QString ownLogName = machine + QStringLiteral(".jsonl");
    const QString bridgeLog = QDir(bridgeDir).filePath(ownLogName);
    QByteArray ownLog;
    if ( QFileInfo::exists(bridgeLog) ) {
        if ( !copyFileReplacing(bridgeLog, QDir(stateDir).filePath(ownLogName)) ) {
            *error = QStringLiteral("cannot copy %1").arg(bridgeLog);
            return false;
        }
        QFile file(bridgeLog);
        if ( file.open(QIODevice::ReadOnly) )
            ownLog = file.readAll();
    }

    const QDir bridgeBlobs(QDir(bridgeDir).filePath(QStringLiteral("blobs/") + machine));
    const QString ownBlobsPath = QDir(stateDir).filePath(QStringLiteral("blobs/") + machine);
    if ( !QDir().mkpath(ownBlobsPath) ) {
        *error = QStringLiteral("cannot create %1").arg(ownBlobsPath);
        return false;
    }
    const QStringList blobNames = bridgeBlobs.entryList(QDir::Files);
    for (const auto &name : blobNames) {
        if ( !FlycutSyncFormat::isBlobAddress(name) )
            continue;
        if ( !copyFileReplacing(bridgeBlobs.filePath(name), QDir(ownBlobsPath).filePath(name)) ) {
            *error = QStringLiteral("cannot copy %1").arg(bridgeBlobs.filePath(name));
            return false;
        }
    }

    if ( !transaction() ) {
        *error = QStringLiteral("cannot start a transaction");
        return false;
    }

    for (auto it = bridgeMeta.constBegin(); it != bridgeMeta.constEnd(); ++it) {
        const QString &key = it.key();
        if ( key.startsWith(QLatin1String("read:")) || key.startsWith(QLatin1String("sealed:"))
             || key == QLatin1String("sent") )
        {
            setMeta(key, it.value());
        } else if ( key == QLatin1String("clear") ) {
            setMeta(QStringLiteral("clear:0"), it.value());
        }
    }

    if ( !bridgeQuery.exec(QStringLiteral("SELECT id, at, machine, deleted, hash FROM clips")) ) {
        *error = QStringLiteral("cannot read %1: %2").arg(bridgeStatePath, errorText(bridgeQuery));
        rollback();
        return false;
    }
    while ( bridgeQuery.next() ) {
        Clip clip;
        clip.at = bridgeQuery.value(1).toLongLong();
        clip.machine = bridgeQuery.value(2).toString();
        clip.deleted = bridgeQuery.value(3).toInt() != 0;
        clip.hash = bridgeQuery.value(4).toString();
        setClip(FlycutSyncFormat::HistoryList, bridgeQuery.value(0).toString(), clip);
    }

    // Unsealed blobs are named by the SHA-256 of their data, so they can be fetched again by it.
    for (const QByteArray &line : ownLog.split('\n')) {
        if ( !line.contains("\"items\"") )
            continue;
        const QJsonArray items = QJsonDocument::fromJson(line).object().value(QStringLiteral("items")).toArray();
        for (const auto &item : items) {
            for (const auto &value : item.toArray()) {
                const QJsonObject representation = value.toObject();
                const QString address = representation.value(QStringLiteral("b")).toString();
                if ( FlycutSyncFormat::isBlobAddress(address) && !representation.value(QStringLiteral("s")).toBool() )
                    setBlob(address, Blob{machine, address, false});
            }
        }
    }

    setMeta(QStringLiteral("machine"), machine);
    setMeta(QStringLiteral("migrated-from"), bridgeDir);
    if ( !commit() ) {
        *error = QStringLiteral("cannot save the migrated state");
        rollback();
        return false;
    }
    return true;
}
