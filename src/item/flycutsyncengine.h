// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "item/flycutsyncformat.h"
#include "item/flycutsyncstate.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>

class FlycutSyncCrypto;
class QNetworkAccessManager;
class QTimer;

struct FlycutSyncSettings {
    QString serverUrl;
    QByteArray token;
    /// Where the state, this machine's log and its blobs are kept.
    QString stateDir;
    /// flycut-copyq-bridge's state directory, taken over on the first start (empty: none).
    QString bridgeStateDir;
};

/**
 * Syncs CopyQ with flycut-sync-server, the way Flycut does on macOS (FlycutEngine/FlycutFolderSync.h,
 * sync-server/README.md), as one more machine with its own log on the server.
 *
 * - This machine's changes (recordPut(), recordDel()) are appended to its own log as put/del events;
 *   the log goes to the server after the blobs its lines refer to (POST with X-Log-Offset, or PUT
 *   when the server's copy doesn't match). Removals that only keep local history short write nothing.
 * - Other machines' logs are read incrementally; for every clipping the event with the greatest
 *   (at, machine) wins, and the outcome is reported per list with remoteChanges().
 * - Credentials (CredentialDetector) are sealed with keys derived from the token: text as "enc",
 *   pasteboard data as sealed blobs ("s": true). Other machines' sealed data is opened.
 *
 * The engine runs in its own thread and talks to the server synchronously; the GUI side
 * (FlycutSync) calls it with queued calls and gets results as signals.
 */
class FlycutSyncEngine final : public QObject
{
    Q_OBJECT

public:
    struct Response {
        int status = 0;
        QByteArray body;
        QHash<QByteArray, QByteArray> headers;
        QString error;
    };

    struct SearchResult {
        int matched = 0;
        QVector<QVariantMap> items;
        QString error;
    };

    explicit FlycutSyncEngine(QObject *parent = nullptr);
    ~FlycutSyncEngine();

    /// Opens the state (migrating the bridge's), prepares the own log and starts polling.
    bool start(const FlycutSyncSettings &settings);
    void stop();

    QString machineId() const { return m_machine; }

    /// Reads other machines' changes and uploads this machine's now (unless a cycle is running).
    void syncNow();

    /// Runs a cycle that starts after this call; cycleFinished() names the token once it ends.
    void requestSync(qulonglong token);

    /// A clipping was copied, changed or moved in the list (data: the CopyQ item data).
    void recordPut(int list, const QString &id, const QVariantMap &data, double rank);

    /// A clipping was deleted by the user. Without an id, the live clipping with the item key hash is deleted.
    void recordDel(int list, const QString &id, const QString &keyHash);

    /// One-time sign-in link for the server's web history (POST /v1/web/admission).
    QString admissionUrl(QString *error);

    /// Data of evicted formats fetched from the server again (format -> data); formats it hasn't got are left out.
    QVariantMap fetchFormats(const QVector<FlycutSyncFormat::EvictedFormat> &formats);

    /// GET /v1/archive/search; every clipping returned as CopyQ item data.
    SearchResult searchArchive(const QString &query, int list, int limit, int offset);

    /// A put event (as in the logs) as CopyQ item data: sealed text and data opened, blobs fetched.
    QVariantMap itemDataFromEvent(const QJsonObject &event, QString *error);

    QString lastStatus() const { return m_status; }
    bool lastStatusOk() const { return m_statusOk; }

    static QString createMachineId(const QString &hostName);

signals:
    /**
     * Changes from other machines to apply to a list. Each put is a map with "id", "rank" and
     * "data" (CopyQ item data with the clipping id and rank set), oldest first. Removals name
     * clipping ids, and item key hashes for items that may carry no id.
     */
    void remoteChanges(int list, const QVariantList &puts, const QStringList &removedIds, const QStringList &removedHashes);

    /// Item key hash -> clipping id of the history's live clippings, for items without an id.
    void liveHashesLoaded(const QVariantMap &hashToId);

    void statusChanged(const QString &status, bool ok);

    /// A sync cycle ended (successfully or not), with the tokens of requestSync() calls it served.
    void cycleFinished(const QVariantList &tokens);

private:
    struct PendingPut {
        QJsonObject event;
        QString text;
    };

    struct ListChanges {
        QHash<QString, PendingPut> puts;
        QSet<QString> removedIds;
        QSet<QString> removedHashes;
    };

    Response request(const QByteArray &method, const QString &path, const QByteArray &body = QByteArray(),
                     const QList<QPair<QByteArray, QByteArray>> &headers = {}, int timeoutMs = 20000);

    void setStatus(const QString &status, bool ok);
    bool runCycle(QString *error);
    bool fetchLog(const QString &machine, qint64 size, QVector<QJsonObject> *events, QString *error);
    void applyEvent(const QJsonObject &event, ListChanges *changes);
    bool uploadOwnLog(qint64 serverSize, QString *error);
    bool uploadBlobs(const QByteArray &lines, const QSet<QString> *known, QString *error);
    bool serverBlobs(QSet<QString> *blobs, QString *error);
    QByteArray fetchBlob(const QString &machine, const QString &address, QString *error);
    QVariantMap formatsOfEvent(const QJsonObject &event, const QString &machine, QString *error);
    QVariantMap pendingPutData(const QString &id, const PendingPut &put, QString *error);

    QString ownLogPath() const;
    QString ownBlobsPath() const;
    QString storeOwnBlob(const QByteArray &data);
    QByteArray ownBlob(const QString &address) const;
    bool appendToOwnLog(const QByteArray &lines);
    bool replaceOwnLog(const QByteArray &data);
    QByteArray lineForEvent(const QJsonObject &event) const;
    qint64 nextLocalAt(qint64 seen);
    bool needsSealing(const QJsonObject &event) const;
    bool sealEvent(QJsonObject *event);
    QPair<qint64, QString> clearMarker(int list) const;

    void sealOwnLogIfNeeded();
    void compactOwnLogIfNeeded();
    void removeUnreferencedOwnBlobs();
    void scheduleSync(int delayMs);
    void putClipping(int list, const QString &id, const QVariantMap &data, double rank);
    void runDeferred();

    FlycutSyncSettings m_settings;
    FlycutSyncState m_state;
    std::unique_ptr<FlycutSyncCrypto> m_crypto;
    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_soonTimer = nullptr;
    QString m_machine;
    QString m_hostName;
    qint64 m_lastLocalAt = 0;
    QSet<QString> m_otherMachines;
    QString m_status;
    bool m_statusOk = false;
    bool m_running = false;
    bool m_inCycle = false;
    bool m_syncAgain = false;
    QVariantList m_waitingTokens;
    QVector<std::function<void()>> m_deferred;
};
