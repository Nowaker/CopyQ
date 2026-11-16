// SPDX-License-Identifier: GPL-3.0-or-later
#include "flycutsyncengine.h"

#include "common/log.h"
#include "common/mimetypes.h"
#include "item/credentialdetector.h"
#include "item/flycutsynccrypto.h"
#include "item/itemlabel.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace FlycutSyncFormat;

namespace {

constexpr int tailLength = 32;  // bytes of a log kept to recognise it was rewritten
constexpr qsizetype maxTextLength = 1'000'000;
constexpr qint64 maxItemBytes = 64LL << 20;  // an item's formats beyond this are dropped, largest first, never its text
constexpr int unreferencedBlobSeconds = 600;
constexpr int pollIntervalMs = 2000;
constexpr int compactionThreshold = 1000;

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString encoded(const QString &pathPart)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(pathPart));
}

std::optional<qint64> integerValue(const QJsonValue &value)
{
    if ( !value.isDouble() )
        return std::nullopt;
    const double number = value.toDouble();
    const auto integer = static_cast<qint64>(number);
    if ( std::abs(number - static_cast<double>(integer)) > 0 )
        return std::nullopt;
    return integer;
}

QString report(const QString &text, LogLevel level)
{
    log(QStringLiteral("Flycut sync: ") + text, level);
    return text;
}

} // namespace

FlycutSyncEngine::FlycutSyncEngine(QObject *parent)
    : QObject(parent)
{
    m_hostName = QSysInfo::machineHostName().split('.').value(0);
}

FlycutSyncEngine::~FlycutSyncEngine() = default;

QString FlycutSyncEngine::createMachineId(const QString &hostName)
{
    QString host = hostName.split('.').value(0).toLower();
    host.remove(QRegularExpression(QStringLiteral("[^a-z0-9-]")));
    if ( host.isEmpty() )
        host = QStringLiteral("linux");
    return host + QStringLiteral("-copyq-") + QUuid::createUuid().toString(QUuid::Id128).left(8);
}

bool FlycutSyncEngine::start(const FlycutSyncSettings &settings)
{
    stop();
    m_settings = settings;
    while ( m_settings.serverUrl.endsWith('/') )
        m_settings.serverUrl.chop(1);

    QString error;
    if ( !m_state.isOpen() && !m_state.open(m_settings.stateDir, &error) ) {
        setStatus(report(error, LogError), false);
        return false;
    }
    if ( !m_state.migrateFromBridge(m_settings.bridgeStateDir, m_settings.stateDir, &error) ) {
        setStatus(report(QStringLiteral("cannot take over flycut-copyq-bridge's state: ") + error, LogError), false);
        return false;
    }

    m_machine = m_state.meta(QStringLiteral("machine"));
    if ( m_machine.isEmpty() ) {
        m_machine = createMachineId(QSysInfo::machineHostName());
        m_state.setMeta(QStringLiteral("machine"), m_machine);
    } else if ( !m_state.meta(QStringLiteral("migrated-from")).isEmpty() ) {
        report(QStringLiteral("continuing as %1").arg(m_machine), LogNote);
    }
    QDir().mkpath(ownBlobsPath());

    m_crypto = std::make_unique<FlycutSyncCrypto>(m_settings.token);
    if ( !m_crypto->isValid() )
        report(QStringLiteral("AES-256-CTR is not available; credentials are not shared"), LogWarning);

    if ( !m_network )
        m_network = new QNetworkAccessManager(this);
    if ( !m_pollTimer ) {
        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(pollIntervalMs);
        connect(m_pollTimer, &QTimer::timeout, this, &FlycutSyncEngine::syncNow);
        m_soonTimer = new QTimer(this);
        m_soonTimer->setSingleShot(true);
        connect(m_soonTimer, &QTimer::timeout, this, &FlycutSyncEngine::syncNow);
    }

    sealOwnLogIfNeeded();
    compactOwnLogIfNeeded();
    removeUnreferencedOwnBlobs();

    QVariantMap hashes;
    const auto liveHashes = m_state.liveHashes(HistoryList);
    for (auto it = liveHashes.constBegin(); it != liveHashes.constEnd(); ++it)
        hashes.insert(it.key(), it.value());
    emit liveHashesLoaded(hashes);

    m_running = true;
    m_pollTimer->start();
    scheduleSync(0);
    return true;
}

void FlycutSyncEngine::stop()
{
    m_running = false;
    if (m_pollTimer) {
        m_pollTimer->stop();
        m_soonTimer->stop();
    }
}

void FlycutSyncEngine::scheduleSync(int delayMs)
{
    if (m_running && m_soonTimer && (!m_soonTimer->isActive() || m_soonTimer->remainingTime() > delayMs))
        m_soonTimer->start(delayMs);
}

void FlycutSyncEngine::setStatus(const QString &status, bool ok)
{
    if (status == m_status && ok == m_statusOk)
        return;
    m_status = status;
    m_statusOk = ok;
    emit statusChanged(status, ok);
}

FlycutSyncEngine::Response FlycutSyncEngine::request(
        const QByteArray &method, const QString &path, const QByteArray &body,
        const QList<QPair<QByteArray, QByteArray>> &headers, int timeoutMs)
{
    Response response;
    if (!m_network) {
        response.error = QStringLiteral("sync is not running");
        return response;
    }

    QNetworkRequest req(QUrl(m_settings.serverUrl + path));
    req.setRawHeader("Authorization", "Bearer " + m_settings.token);
    for (const auto &header : headers)
        req.setRawHeader(header.first, header.second);
    if ( !body.isNull() )
        req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/octet-stream"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(timeoutMs);

    QNetworkReply *reply = m_network->sendCustomRequest(req, method, body);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    for (const auto &pair : reply->rawHeaderPairs())
        response.headers.insert(pair.first.toLower(), pair.second);
    if (response.status == 0)
        response.error = reply->errorString();
    reply->deleteLater();
    return response;
}

QString FlycutSyncEngine::ownLogPath() const
{
    return QDir(m_settings.stateDir).filePath(m_machine + QStringLiteral(".jsonl"));
}

QString FlycutSyncEngine::ownBlobsPath() const
{
    return QDir(m_settings.stateDir).filePath(QStringLiteral("blobs/") + m_machine);
}

QString FlycutSyncEngine::storeOwnBlob(const QByteArray &data)
{
    const QString address = sha256Hex(data);
    const QString path = QDir(ownBlobsPath()).filePath(address);
    if ( QFileInfo::exists(path) )
        return address;
    QSaveFile file(path);
    if ( !file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit() ) {
        report(QStringLiteral("cannot store pasteboard data in %1").arg(ownBlobsPath()), LogError);
        return {};
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return address;
}

QByteArray FlycutSyncEngine::ownBlob(const QString &address) const
{
    QFile file(QDir(ownBlobsPath()).filePath(address));
    if ( !file.open(QIODevice::ReadOnly) )
        return {};
    return file.readAll();
}

QByteArray FlycutSyncEngine::lineForEvent(const QJsonObject &event) const
{
    return QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
}

bool FlycutSyncEngine::appendToOwnLog(const QByteArray &lines)
{
    QFile file(ownLogPath());
    if ( !file.open(QIODevice::WriteOnly | QIODevice::Append) ) {
        setStatus(report(QStringLiteral("cannot write %1").arg(ownLogPath()), LogError), false);
        return false;
    }
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    // One write per batch of lines, so a reader never sees half of a line.
    return file.write(lines) == lines.size();
}

// Written beside the log and renamed over it, so readers see the old or the new one.
bool FlycutSyncEngine::replaceOwnLog(const QByteArray &data)
{
    QSaveFile file(ownLogPath());
    if ( !file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit() ) {
        report(QStringLiteral("cannot rewrite %1").arg(ownLogPath()), LogError);
        return false;
    }
    QFile::setPermissions(ownLogPath(), QFile::ReadOwner | QFile::WriteOwner);
    // The server's copy no longer matches: replace it on the next upload.
    m_state.setMeta(QStringLiteral("sent"), QStringLiteral("-1"));
    return true;
}

qint64 FlycutSyncEngine::nextLocalAt(qint64 seen)
{
    m_lastLocalAt = std::max({nowMs(), seen + 1, m_lastLocalAt + 1});
    return m_lastLocalAt;
}

QPair<qint64, QString> FlycutSyncEngine::clearMarker(int list) const
{
    const QStringList parts = m_state.meta(QStringLiteral("clear:%1").arg(list)).split(' ');
    if (parts.size() == 2)
        return {parts[0].toLongLong(), parts[1]};
    return {0, QString()};
}

bool FlycutSyncEngine::needsSealing(const QJsonObject &event) const
{
    const QJsonValue contents = event.value(QStringLiteral("contents"));
    return !event.value(QStringLiteral("described")).toBool()
        && contents.isString()
        && CredentialDetector::instance().isCredential(contents.toString());
}

// Replaces a credential's "contents" with "enc", and its items' data with sealed data from the own blobs.
bool FlycutSyncEngine::sealEvent(QJsonObject *event)
{
    if ( !needsSealing(*event) || !m_crypto || !m_crypto->isValid() )
        return false;
    const QString sealed = m_crypto->seal(event->value(QStringLiteral("contents")).toString());
    if ( sealed.isEmpty() )
        return false;
    event->insert(QStringLiteral("enc"), sealed);
    event->remove(QStringLiteral("contents"));

    if ( !event->contains(QStringLiteral("items")) )
        return true;
    QJsonArray sealedItems;
    for (const auto &item : event->value(QStringLiteral("items")).toArray()) {
        QJsonArray sealedItem;
        for (const auto &value : item.toArray()) {
            QJsonObject representation = value.toObject();
            if ( !representation.value(QStringLiteral("s")).toBool() ) {
                const QByteArray data = ownBlob(representation.value(QStringLiteral("b")).toString());
                const QString address = data.isNull() ? QString() : storeOwnBlob(m_crypto->sealData(data));
                if ( address.isEmpty() )
                    continue;
                representation.insert(QStringLiteral("b"), address);
                representation.insert(QStringLiteral("s"), true);
            }
            sealedItem.append(representation);
        }
        if ( !sealedItem.isEmpty() )
            sealedItems.append(sealedItem);
    }
    event->insert(QStringLiteral("items"), sealedItems);
    return true;
}

// Seals credentials this machine shared in plain text before, once per version of the credential rules,
// and makes the next upload replace the server's copy.
void FlycutSyncEngine::sealOwnLogIfNeeded()
{
    if ( !m_crypto->isValid() )
        return;
    const QString doneKey = QStringLiteral("sealed:%1").arg(CredentialDetector::instance().version());
    if ( !m_state.meta(doneKey).isEmpty() )
        return;

    QFile file(ownLogPath());
    int sealedCount = 0;
    QByteArray rewritten;
    if ( file.open(QIODevice::ReadOnly) ) {
        const QByteArray log = file.readAll();
        file.close();
        for (const QByteArray &line : log.split('\n')) {
            if ( line.isEmpty() )
                continue;
            QJsonObject event = QJsonDocument::fromJson(line).object();
            if ( event.value(QStringLiteral("op")).toString() == QLatin1String("put") && sealEvent(&event) ) {
                rewritten.append(lineForEvent(event));
                ++sealedCount;
            } else {
                rewritten.append(line + '\n');
            }
        }
    }
    if ( sealedCount > 0 && !replaceOwnLog(rewritten) )
        return;
    m_state.setMeta(doneKey, QString::number(sealedCount));
    if (sealedCount > 0)
        report(QStringLiteral("sealed %1 credential clipping(s) in %2").arg(sealedCount).arg(ownLogPath()), LogNote);
}

// Keeps only the newest event per clipping (and the newest clear per list) once the log has grown to more
// than twice that, as Flycut does.
void FlycutSyncEngine::compactOwnLogIfNeeded()
{
    QFile file(ownLogPath());
    if ( !file.open(QIODevice::ReadOnly) )
        return;
    const QList<QByteArray> lines = file.readAll().split('\n');
    file.close();
    if (lines.size() < compactionThreshold)
        return;

    QHash<QString, int> latest;  // key -> index in entries
    QVector<QPair<QByteArray, QJsonObject>> entries;
    QStringList order;
    int eventCount = 0;
    for (const QByteArray &line : lines) {
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        if ( event.isEmpty() )
            continue;
        ++eventCount;
        const QString list = QString::number(event.value(QStringLiteral("list")).toInt());
        const QString key = event.value(QStringLiteral("op")).toString() == QLatin1String("clear")
            ? QStringLiteral("clear/") + list
            : list + '/' + event.value(QStringLiteral("id")).toString();
        if ( !latest.contains(key) )
            order.append(key);
        latest.insert(key, entries.size());
        entries.append({line, event});
    }
    if (latest.size() * 2 > eventCount)
        return;

    QByteArray compacted;
    for (const auto &key : order) {
        const auto &entry = entries[latest[key]];
        const QJsonObject &event = entry.second;
        if ( event.value(QStringLiteral("op")).toString() != QLatin1String("clear") ) {
            const auto clearIndex = latest.find(QStringLiteral("clear/") + QString::number(event.value(QStringLiteral("list")).toInt()));
            if ( clearIndex != latest.end() ) {
                const QJsonObject &clear = entries[clearIndex.value()].second;
                if ( !isNewer(integerValue(event.value(QStringLiteral("at"))).value_or(0), event.value(QStringLiteral("m")).toString(),
                              integerValue(clear.value(QStringLiteral("at"))).value_or(0), clear.value(QStringLiteral("m")).toString()) )
                {
                    continue;
                }
            }
        }
        compacted.append(entry.first + '\n');
    }
    if ( replaceOwnLog(compacted) )
        report(QStringLiteral("compacted %1 from %2 to %3 events").arg(ownLogPath()).arg(eventCount).arg(latest.size()), LogNote);
}

void FlycutSyncEngine::removeUnreferencedOwnBlobs()
{
    QFile file(ownLogPath());
    const QSet<QString> keep = file.open(QIODevice::ReadOnly) ? blobAddresses(file.readAll()) : QSet<QString>();
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-unreferencedBlobSeconds);
    int removed = 0;
    const QDir dir(ownBlobsPath());
    for (const QFileInfo &info : dir.entryInfoList(QDir::Files)) {
        if ( !keep.contains(info.fileName()) && info.lastModified() < cutoff && QFile::remove(info.filePath()) )
            ++removed;
    }
    if (removed > 0)
        report(QStringLiteral("removed %1 blob(s) no line of %2 refers to").arg(removed).arg(ownLogPath()), LogNote);
}

void FlycutSyncEngine::recordPut(int list, const QString &id, const QVariantMap &data, double rank)
{
    if (m_inCycle) {
        m_deferred.append([=](){ recordPut(list, id, data, rank); });
        return;
    }
    if ( !m_state.isOpen() || id.isEmpty() )
        return;
    // Fetching evicted formats waits for the server; changes and cycles arriving meanwhile wait for this one.
    m_inCycle = true;
    putClipping(list, id, data, rank);
    m_inCycle = false;
    runDeferred();
}

void FlycutSyncEngine::putClipping(int list, const QString &id, const QVariantMap &data, double rank)
{

    QVariantMap formats = syncedFormats(data);
    const QByteArray textBytes = formats.value(mimeText).toByteArray();
    const QString text = QString::fromUtf8(textBytes);
    if (text.size() > maxTextLength)
        return;
    const bool described = text.isEmpty();
    const QString contents = described ? itemLabel(data) : text;
    if ( contents.isEmpty() )
        return;
    const bool sealed = !described && CredentialDetector::instance().isCredential(text);
    if ( sealed && !m_crypto->isValid() )
        return;  // never share a credential unsealed

    // Evicted formats: referenced again if this machine uploaded them, otherwise fetched back.
    QVector<EvictedFormat> missing;
    QHash<QString, QString> reusedAddresses;  // format -> own unsealed blob
    for (const auto &evicted : evictedFormats(data)) {
        if ( formats.contains(evicted.format) )
            continue;
        const auto blob = m_state.blob(evicted.sha256);
        if ( !sealed && blob && blob->machine == m_machine && !blob->sealed
             && QFileInfo::exists(QDir(ownBlobsPath()).filePath(blob->address)) )
        {
            reusedAddresses.insert(evicted.format, blob->address);
            formats.insert(evicted.format, QVariant());
        } else {
            missing.append(evicted);
        }
    }
    const QVariantMap fetched = fetchFormats(missing);
    for (auto it = fetched.constBegin(); it != fetched.constEnd(); ++it)
        formats.insert(it.key(), it.value());

    qint64 total = 0;
    for (auto it = formats.constBegin(); it != formats.constEnd(); ++it)
        total += it.value().toByteArray().size();
    while (total > maxItemBytes) {
        QString largest;
        qint64 largestSize = -1;
        for (auto it = formats.constBegin(); it != formats.constEnd(); ++it) {
            const qint64 size = it.value().toByteArray().size();
            if (it.key() != mimeText && size > largestSize) {
                largest = it.key();
                largestSize = size;
            }
        }
        if ( largest.isEmpty() )
            break;
        total -= largestSize;
        formats.remove(largest);
    }

    QJsonArray representations;
    if ( formats.keys() != QStringList{mimeText} ) {
        for (auto it = formats.constBegin(); it != formats.constEnd(); ++it) {
            QJsonObject representation{{QStringLiteral("t"), pasteboardType(it.key())}};
            if ( reusedAddresses.contains(it.key()) ) {
                const QString address = reusedAddresses[it.key()];
                representation.insert(QStringLiteral("b"), address);
                representation.insert(QStringLiteral("n"), QFileInfo(QDir(ownBlobsPath()).filePath(address)).size());
            } else {
                const QByteArray bytes = it.value().toByteArray();
                const QString address = storeOwnBlob(sealed ? m_crypto->sealData(bytes) : bytes);
                if ( address.isEmpty() )
                    continue;
                m_state.setBlob(sha256Hex(bytes), FlycutSyncState::Blob{m_machine, address, sealed});
                representation.insert(QStringLiteral("b"), address);
                representation.insert(QStringLiteral("n"), bytes.size());
            }
            if (sealed)
                representation.insert(QStringLiteral("s"), true);
            representations.append(representation);
        }
    }

    const auto known = m_state.clip(list, id);
    const qint64 at = nextLocalAt(std::max(known ? known->at : 0, clearMarker(list).first));
    m_state.setClip(list, id, FlycutSyncState::Clip{at, m_machine, false, keyHash(syncedFormats(data))});

    const QString firstFormat = formats.isEmpty() ? QString() : formats.firstKey();
    const qint64 copiedAt = data.value(mimeCopiedAt).toByteArray().toLongLong();
    const qint64 copiedAtSeconds = copiedAt > 0 ? copiedAt / 1000 : QDateTime::currentSecsSinceEpoch();
    QJsonObject event{
        {QStringLiteral("v"), 1},
        {QStringLiteral("op"), QStringLiteral("put")},
        {QStringLiteral("list"), list},
        {QStringLiteral("id"), id},
        {QStringLiteral("at"), at},
        {QStringLiteral("m"), m_machine},
        {QStringLiteral("rank"), rank},
        {QStringLiteral("contents"), contents},
        {QStringLiteral("type"), described ? pasteboardType(firstFormat) : QStringLiteral("NSStringPboardType")},
        {QStringLiteral("app"), m_hostName},
        {QStringLiteral("ts"), copiedAtSeconds},
    };
    if (described)
        event.insert(QStringLiteral("described"), true);
    if ( !representations.isEmpty() )
        event.insert(QStringLiteral("items"), QJsonArray{representations});
    sealEvent(&event);
    appendToOwnLog(lineForEvent(event));
    scheduleSync(100);
}

void FlycutSyncEngine::recordDel(int list, const QString &id, const QString &keyHash)
{
    if (m_inCycle) {
        m_deferred.append([=](){ recordDel(list, id, keyHash); });
        return;
    }
    if ( !m_state.isOpen() )
        return;
    const QString clipId = id.isEmpty() ? m_state.liveIdForHash(list, keyHash) : id;
    if ( clipId.isEmpty() )
        return;
    const auto known = m_state.clip(list, clipId);
    if ( !known || known->deleted )
        return;  // never shared, or already deleted
    const qint64 at = nextLocalAt(known->at);
    m_state.setClip(list, clipId, FlycutSyncState::Clip{at, m_machine, true, known->hash});
    appendToOwnLog(lineForEvent(QJsonObject{
        {QStringLiteral("v"), 1},
        {QStringLiteral("op"), QStringLiteral("del")},
        {QStringLiteral("list"), list},
        {QStringLiteral("id"), clipId},
        {QStringLiteral("at"), at},
        {QStringLiteral("m"), m_machine},
    }));
    scheduleSync(100);
}

void FlycutSyncEngine::syncNow()
{
    // A nested event loop waits for each request; don't start another cycle from it.
    if ( !m_running )
        return;
    if (m_inCycle) {
        m_syncAgain = true;
        return;
    }
    m_inCycle = true;
    const QVariantList tokens = std::exchange(m_waitingTokens, {});

    QString error;
    m_state.transaction();
    if ( runCycle(&error) ) {
        const int count = m_otherMachines.size();
        setStatus(count == 1
                  ? QStringLiteral("Synced as %1 with 1 other machine").arg(m_machine)
                  : QStringLiteral("Synced as %1 with %2 other machines").arg(m_machine).arg(count), true);
    } else {
        m_state.rollback();
        if (error != m_status)
            report(error, LogWarning);
        setStatus(error, false);
    }
    m_inCycle = false;
    runDeferred();
    emit cycleFinished(tokens);
}

// Local changes that arrived while requests were waiting, kept out of the cycle's transaction.
void FlycutSyncEngine::runDeferred()
{
    const auto deferred = std::exchange(m_deferred, {});
    for (const auto &call : deferred)
        call();

    if (m_syncAgain) {
        m_syncAgain = false;
        scheduleSync(0);
    }
}

void FlycutSyncEngine::requestSync(qulonglong token)
{
    m_waitingTokens.append(token);
    if (m_inCycle)
        m_syncAgain = true;
    else
        syncNow();
    if ( !m_running )
        emit cycleFinished(std::exchange(m_waitingTokens, {}));
}

bool FlycutSyncEngine::runCycle(QString *error)
{
    const Response logs = request("GET", QStringLiteral("/v1/logs"));
    if (logs.status == 401) {
        *error = QStringLiteral("The server refused the token");
        return false;
    }
    if (logs.status != 200) {
        *error = logs.status == 0
            ? QStringLiteral("Cannot reach the server: %1").arg(logs.error)
            : QStringLiteral("Unexpected answer from the server: HTTP %1").arg(logs.status);
        return false;
    }

    ListChanges changes[2];
    qint64 ownSize = -1;
    m_otherMachines.clear();
    const QJsonArray entries = QJsonDocument::fromJson(logs.body).array();
    for (const auto &value : entries) {
        const QJsonObject entry = value.toObject();
        const QString machine = entry.value(QStringLiteral("machine")).toString();
        const qint64 size = integerValue(entry.value(QStringLiteral("size"))).value_or(0);
        if (machine == m_machine) {
            ownSize = size;
            continue;
        }
        m_otherMachines.insert(machine);
        QVector<QJsonObject> events;
        if ( !fetchLog(machine, size, &events, error) )
            return false;
        for (const auto &event : events) {
            const int list = event.value(QStringLiteral("list")).toInt(-1);
            if (list == HistoryList || list == FavoritesList)
                applyEvent(event, &changes[list]);
        }
    }

    if ( !uploadOwnLog(ownSize, error) )
        return false;

    struct Outcome {
        QVariantList puts;
        QStringList removedIds;
        QStringList removedHashes;
    } outcomes[2];
    for (int list : {HistoryList, FavoritesList}) {
        auto &listChanges = changes[list];
        QVector<QPair<double, QVariantMap>> puts;
        for (auto it = listChanges.puts.constBegin(); it != listChanges.puts.constEnd(); ++it) {
            const QVariantMap data = pendingPutData(it.key(), it.value(), error);
            if ( data.isEmpty() ) {
                if ( !error->isEmpty() )
                    return false;
                continue;
            }
            const double rank = it.value().event.value(QStringLiteral("rank")).toDouble();
            const QString hash = keyHash(syncedFormats(data));
            listChanges.removedHashes.remove(hash);
            auto clip = m_state.clip(list, it.key());
            if (clip && clip->hash != hash) {
                clip->hash = hash;
                m_state.setClip(list, it.key(), *clip);
            }
            puts.append({rank, QVariantMap{{QStringLiteral("id"), it.key()}, {QStringLiteral("rank"), rank}, {QStringLiteral("data"), data}}});
        }
        std::sort(puts.begin(), puts.end(), [](const auto &a, const auto &b){ return a.first < b.first; });
        for (const auto &put : puts)
            outcomes[list].puts.append(put.second);
        outcomes[list].removedIds = QStringList(listChanges.removedIds.begin(), listChanges.removedIds.end());
        outcomes[list].removedHashes = QStringList(listChanges.removedHashes.begin(), listChanges.removedHashes.end());
    }

    if ( !m_state.commit() ) {
        *error = QStringLiteral("Cannot save the sync state");
        return false;
    }

    for (int list : {HistoryList, FavoritesList}) {
        const auto &outcome = outcomes[list];
        if ( !outcome.puts.isEmpty() || !outcome.removedIds.isEmpty() || !outcome.removedHashes.isEmpty() )
            emit remoteChanges(list, outcome.puts, outcome.removedIds, outcome.removedHashes);
    }
    return true;
}

// Reads the complete lines another machine appended since the last read. A log that no longer has the
// bytes the last read ended with was rewritten (compacted), so it is read from the start; applying an
// event twice changes nothing.
bool FlycutSyncEngine::fetchLog(const QString &machine, qint64 size, QVector<QJsonObject> *events, QString *error)
{
    const QString positionKey = QStringLiteral("read:") + machine;
    const QStringList position = m_state.meta(positionKey).split(' ');
    const qint64 offset = position.size() == 2 ? position[0].toLongLong() : 0;
    const QByteArray tail = position.size() == 2 ? QByteArray::fromHex(position[1].toLatin1()) : QByteArray();
    if (offset == size)
        return true;

    const QString path = QStringLiteral("/v1/logs/") + encoded(machine);
    qint64 start = offset <= size ? offset - tail.size() : 0;
    Response response = request("GET", path + QStringLiteral("?from=%1").arg(start));
    if (response.status != 200 || response.headers.value("x-log-size").toLongLong() != start + response.body.size()) {
        *error = QStringLiteral("Cannot download %1's history: %2").arg(machine,
            response.status == 0 ? response.error : QStringLiteral("HTTP %1").arg(response.status));
        // The log may be growing while it is read; retried next time.
        return response.status == 200;
    }
    QByteArray data = response.body;
    if ( start > 0 && !data.startsWith(tail) ) {
        start = 0;
        response = request("GET", path + QStringLiteral("?from=0"));
        if (response.status != 200 || response.headers.value("x-log-size").toLongLong() != response.body.size())
            return response.status == 200;
        data = response.body;
    }

    const qsizetype end = data.lastIndexOf('\n') + 1;
    const qsizetype skip = start > 0 ? tail.size() : 0;
    if (end <= skip)
        return true;
    for (const QByteArray &line : data.mid(skip, end - skip).split('\n')) {
        if ( line.isEmpty() )
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if ( doc.isObject() )
            events->append(doc.object());
        else
            report(QStringLiteral("skipping an unreadable line in %1").arg(machine), LogWarning);
    }
    const qsizetype tailStart = std::max<qsizetype>(0, end - tailLength);
    m_state.setMeta(positionKey, QStringLiteral("%1 %2").arg(start + end).arg(QString::fromLatin1(data.mid(tailStart, end - tailStart).toHex())));
    return true;
}

void FlycutSyncEngine::applyEvent(const QJsonObject &event, ListChanges *changes)
{
    if (event.value(QStringLiteral("v")).toInt() != 1)
        return;
    const int list = event.value(QStringLiteral("list")).toInt();
    const QString machine = event.value(QStringLiteral("m")).toString();
    const auto at = integerValue(event.value(QStringLiteral("at")));
    const QString op = event.value(QStringLiteral("op")).toString();
    if ( machine.isEmpty() || machine == m_machine || !at )
        return;

    const auto cleared = clearMarker(list);
    if (op == QLatin1String("clear")) {
        if ( !isNewer(*at, machine, cleared.first, cleared.second) )
            return;
        m_state.setMeta(QStringLiteral("clear:%1").arg(list), QStringLiteral("%1 %2").arg(*at).arg(machine));
        const auto removed = m_state.liveClipsOlderThan(list, *at, machine);
        m_state.markDeleted(list, removed.keys());
        for (auto it = removed.constBegin(); it != removed.constEnd(); ++it) {
            changes->puts.remove(it.key());
            changes->removedIds.insert(it.key());
            if ( !it.value().isEmpty() )
                changes->removedHashes.insert(it.value());
        }
        return;
    }

    const QString id = event.value(QStringLiteral("id")).toString();
    if ( id.isEmpty() || !isNewer(*at, machine, cleared.first, cleared.second) )
        return;
    const auto known = m_state.clip(list, id);
    if ( known && !isNewer(*at, machine, known->at, known->machine) )
        return;

    if (op == QLatin1String("put")) {
        QString text;
        if ( event.value(QStringLiteral("contents")).isString() ) {
            text = event.value(QStringLiteral("contents")).toString();
        } else if ( event.contains(QStringLiteral("enc")) && m_crypto ) {
            text = m_crypto->open(event.value(QStringLiteral("enc")).toString()).value_or(QString());
        }
        if ( text.isEmpty() || text.size() > maxTextLength )
            return;
        const bool described = event.value(QStringLiteral("described")).toBool();
        const QString hash = described ? (known ? known->hash : QString()) : sha256Hex(text.toUtf8());
        m_state.setClip(list, id, FlycutSyncState::Clip{*at, machine, false, hash});
        changes->removedIds.remove(id);
        if ( !hash.isEmpty() )
            changes->removedHashes.remove(hash);
        changes->puts.insert(id, PendingPut{event, text});
    } else if (op == QLatin1String("del")) {
        const QString hash = known ? known->hash : QString();
        m_state.setClip(list, id, FlycutSyncState::Clip{*at, machine, true, hash});
        changes->puts.remove(id);
        changes->removedIds.insert(id);
        if ( !hash.isEmpty() && m_state.liveIdForHash(list, hash).isEmpty() )
            changes->removedHashes.insert(hash);
    }
}

QVariantMap FlycutSyncEngine::pendingPutData(const QString &id, const PendingPut &put, QString *error)
{
    const QString machine = put.event.value(QStringLiteral("m")).toString();
    QVariantMap data = formatsOfEvent(put.event, machine, error);
    if ( !error->isEmpty() )
        return {};
    if ( !put.event.value(QStringLiteral("described")).toBool() || data.isEmpty() )
        data.insert(mimeText, put.text.toUtf8());
    data.insert(mimeClipId, id);
    data.insert(mimeRank, put.event.value(QStringLiteral("rank")).toDouble());
    data.insert(mimeMachine, machine.toUtf8());
    const QString app = put.event.value(QStringLiteral("app")).toString();
    if ( !app.isEmpty() )
        data.insert(mimeApp, app.toUtf8());
    const auto ts = integerValue(put.event.value(QStringLiteral("ts")));
    const qint64 copiedAt = ts && *ts > 0 ? *ts * 1000 : integerValue(put.event.value(QStringLiteral("at"))).value_or(0);
    if (copiedAt > 0)
        data.insert(mimeCopiedAt, QByteArray::number(copiedAt));
    return data;
}

QByteArray FlycutSyncEngine::fetchBlob(const QString &machine, const QString &address, QString *error)
{
    const Response response = request("GET", QStringLiteral("/v1/blobs/%1/%2").arg(encoded(machine), address), QByteArray(), {}, 120000);
    if (response.status == 404)
        return {};  // blobs go up before their lines, so it's gone
    if (response.status != 200 || sha256Hex(response.body) != address) {
        *error = QStringLiteral("Cannot download %1's pasteboard data: %2").arg(machine,
            response.status == 0 ? response.error : QStringLiteral("HTTP %1").arg(response.status));
        return {};
    }
    return response.body;
}

// The event's first item as CopyQ formats; types CopyQ has no use for are left out.
QVariantMap FlycutSyncEngine::formatsOfEvent(const QJsonObject &event, const QString &machine, QString *error)
{
    QVariantMap formats;
    const QJsonArray items = event.value(QStringLiteral("items")).toArray();
    for (const auto &value : items.isEmpty() ? QJsonArray() : items.first().toArray()) {
        const QJsonObject representation = value.toObject();
        const QString address = representation.value(QStringLiteral("b")).toString();
        if ( !isBlobAddress(address) )
            continue;
        const QString mime = mimeForPasteboardType(representation.value(QStringLiteral("t")).toString());
        if ( mime.isEmpty() || formats.contains(mime) )
            continue;
        QByteArray data = fetchBlob(machine, address, error);
        if ( !error->isEmpty() )
            return {};
        const bool sealed = representation.value(QStringLiteral("s")).toBool();
        if ( !data.isNull() && sealed )
            data = m_crypto ? m_crypto->openData(data).value_or(QByteArray()) : QByteArray();
        if ( data.isNull() )
            continue;
        m_state.setBlob(sha256Hex(data), FlycutSyncState::Blob{machine, address, sealed});
        formats.insert(mime, data);
    }
    return formats;
}

bool FlycutSyncEngine::uploadOwnLog(qint64 serverSize, QString *error)
{
    QFile file(ownLogPath());
    if ( !file.open(QIODevice::ReadOnly) )
        return true;
    QByteArray local = file.readAll();
    local.truncate(local.lastIndexOf('\n') + 1);
    const QString sentText = m_state.meta(QStringLiteral("sent"));
    const qint64 sent = sentText.isEmpty() ? -1 : sentText.toLongLong();
    if (serverSize == local.size() && local.size() == sent)
        return true;

    const QString path = QStringLiteral("/v1/logs/") + encoded(m_machine);
    int status = 0;
    if (serverSize == sent && sent >= 0 && sent < local.size()) {
        const QByteArray lines = local.mid(sent);
        if ( !uploadBlobs(lines, nullptr, error) )
            return false;
        status = request("POST", path, lines, {{"X-Log-Offset", QByteArray::number(sent)}}).status;
    }
    if (status != 204) {
        QSet<QString> known;
        if ( !blobAddresses(local).isEmpty() && !serverBlobs(&known, error) )
            return false;
        if ( !uploadBlobs(local, &known, error) )
            return false;
        const Response response = request("PUT", path, local, {}, 120000);
        if (response.status != 204) {
            *error = QStringLiteral("Cannot upload this machine's history: %1").arg(
                response.status == 0 ? response.error : QStringLiteral("HTTP %1").arg(response.status));
            return false;
        }
    }
    m_state.setMeta(QStringLiteral("sent"), QString::number(local.size()));
    return true;
}

bool FlycutSyncEngine::serverBlobs(QSet<QString> *blobs, QString *error)
{
    const Response response = request("GET", QStringLiteral("/v1/blobs/") + encoded(m_machine));
    if (response.status != 200) {
        *error = QStringLiteral("Cannot list this machine's pasteboard data on the server: HTTP %1").arg(response.status);
        return false;
    }
    for (const auto &value : QJsonDocument::fromJson(response.body).array())
        blobs->insert(value.toString());
    return true;
}

// Blobs go up before the lines that refer to them; known is what the server has, or null to ask per blob.
bool FlycutSyncEngine::uploadBlobs(const QByteArray &lines, const QSet<QString> *known, QString *error)
{
    for (const auto &address : blobAddresses(lines)) {
        if ( known && known->contains(address) )
            continue;
        const QString path = QStringLiteral("/v1/blobs/%1/%2").arg(encoded(m_machine), address);
        if ( !known && request("HEAD", path).status == 200 )
            continue;
        const QByteArray data = ownBlob(address);
        if ( data.isNull() )
            continue;  // gone here too; readers skip it
        const Response response = request("PUT", path, data, {}, 300000);
        if (response.status == 413) {
            report(QStringLiteral("the server refused %1 bytes of pasteboard data as too large; synced without it").arg(data.size()), LogWarning);
            continue;
        }
        if (response.status != 204) {
            *error = QStringLiteral("Cannot upload pasteboard data: %1").arg(
                response.status == 0 ? response.error : QStringLiteral("HTTP %1").arg(response.status));
            return false;
        }
    }
    return true;
}

QString FlycutSyncEngine::admissionUrl(QString *error)
{
    const Response response = request("POST", QStringLiteral("/v1/web/admission"), QByteArray(""));
    const QString path = QJsonDocument::fromJson(response.body).object().value(QStringLiteral("path")).toString();
    if (response.status != 200 || !path.startsWith('/')) {
        *error = response.status == 0
            ? QStringLiteral("Cannot reach the server: %1").arg(response.error)
            : QStringLiteral("The server refused a sign-in link: HTTP %1").arg(response.status);
        return {};
    }
    return m_settings.serverUrl + path;
}

QVariantMap FlycutSyncEngine::fetchFormats(const QVector<EvictedFormat> &formats)
{
    QVariantMap result;
    for (const auto &evicted : formats) {
        // Data this machine shared unsealed is named by its SHA-256 among its own blobs.
        const auto blob = m_state.blob(evicted.sha256)
            .value_or(FlycutSyncState::Blob{m_machine, evicted.sha256, false});
        QString error;
        QByteArray data;
        if ( blob.machine == m_machine )
            data = ownBlob(blob.address);
        if ( data.isNull() )
            data = fetchBlob(blob.machine, blob.address, &error);
        if ( !data.isNull() && blob.sealed )
            data = m_crypto ? m_crypto->openData(data).value_or(QByteArray()) : QByteArray();
        if ( !data.isNull() && sha256Hex(data) == evicted.sha256 )
            result.insert(evicted.format, data);
        else if ( !error.isEmpty() )
            report(error, LogWarning);
    }
    return result;
}

QVariantMap FlycutSyncEngine::itemDataFromEvent(const QJsonObject &event, QString *error)
{
    QString text;
    if ( event.value(QStringLiteral("contents")).isString() )
        text = event.value(QStringLiteral("contents")).toString();
    else if ( event.contains(QStringLiteral("enc")) && m_crypto )
        text = m_crypto->open(event.value(QStringLiteral("enc")).toString()).value_or(QString());
    if ( text.isEmpty() ) {
        *error = QStringLiteral("The clipping cannot be read");
        return {};
    }
    PendingPut put{event, text};
    return pendingPutData(event.value(QStringLiteral("id")).toString(), put, error);
}

FlycutSyncEngine::SearchResult FlycutSyncEngine::searchArchive(const QString &query, int list, int limit, int offset)
{
    SearchResult result;
    QString path = QStringLiteral("/v1/archive/search?q=") + QString::fromLatin1(QUrl::toPercentEncoding(query));
    if (list >= 0)
        path.append(QStringLiteral("&list=%1").arg(list));
    if (limit > 0)
        path.append(QStringLiteral("&limit=%1").arg(limit));
    if (offset > 0)
        path.append(QStringLiteral("&offset=%1").arg(offset));
    const Response response = request("GET", path);
    if (response.status != 200) {
        result.error = response.status == 0
            ? QStringLiteral("Cannot reach the server: %1").arg(response.error)
            : QStringLiteral("Archive search failed: HTTP %1").arg(response.status);
        return result;
    }
    const QJsonObject answer = QJsonDocument::fromJson(response.body).object();
    result.matched = answer.value(QStringLiteral("matched")).toInt();
    for (const auto &value : answer.value(QStringLiteral("clippings")).toArray()) {
        QString error;
        const QVariantMap data = itemDataFromEvent(value.toObject(), &error);
        if ( !data.isEmpty() )
            result.items.append(data);
        else if ( !error.isEmpty() )
            report(error, LogWarning);
    }
    return result;
}
