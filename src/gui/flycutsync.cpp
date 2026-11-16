// SPDX-License-Identifier: GPL-3.0-or-later
#include "flycutsync.h"

#include "common/appconfig.h"
#include "common/config.h"
#include "common/contenttype.h"
#include "common/keychainaccess.h"
#include "common/log.h"
#include "common/mimetypes.h"
#include "gui/clipboardbrowser.h"
#include "gui/mainwindow.h"
#include "item/flycutsyncformat.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <optional>

using namespace FlycutSyncFormat;

namespace {

QString keychainServiceName()
{
    return QStringLiteral("com.github.hluk.copyq");
}

QString keychainTokenKey()
{
    const QString sessionName = qApp->property("CopyQ_session_name").toString();
    if (sessionName.isEmpty())
        return QStringLiteral("FlycutSyncToken");
    return QStringLiteral("FlycutSyncTokenForSession-%1").arg(sessionName);
}

double nowSeconds()
{
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

QVariantMap itemData(const QAbstractItemModel *model, int row)
{
    return model->index(row, 0).data(contentType::data).toMap();
}

std::optional<double> rankOf(const QVariantMap &data)
{
    const QVariant rank = data.value(mimeRank);
    if ( !rank.isValid() )
        return std::nullopt;
    bool ok = false;
    const double value = rank.toDouble(&ok);
    return ok ? std::optional<double>(value) : std::nullopt;
}

std::optional<double> rankOfRow(const QAbstractItemModel *model, int row)
{
    if (row < 0 || row >= model->rowCount())
        return std::nullopt;
    return rankOf(itemData(model, row));
}

QString newClipId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
}

} // namespace

FlycutSync::FlycutSync(MainWindow *mainWindow)
    : QObject(mainWindow)
    , m_mainWindow(mainWindow)
{
}

FlycutSync::~FlycutSync()
{
    stopEngine();
}

template <typename Function>
void FlycutSync::callEngine(Function function)
{
    if (m_engine)
        QMetaObject::invokeMethod(m_engine, function, Qt::QueuedConnection);
}

template <typename Function>
void FlycutSync::callEngineAndWait(Function function)
{
    if (m_engine)
        QMetaObject::invokeMethod(m_engine, function, Qt::BlockingQueuedConnection);
}

QByteArray FlycutSync::readToken()
{
    return KeychainAccess::readPassword(keychainServiceName(), keychainTokenKey()).trimmed();
}

bool FlycutSync::writeToken(const QByteArray &token)
{
    if ( token.isEmpty() ) {
        KeychainAccess::deletePassword(keychainServiceName(), keychainTokenKey());
        return true;
    }
    return KeychainAccess::writePassword(keychainServiceName(), keychainTokenKey(), token);
}

void FlycutSync::loadSettings(const AppConfig &appConfig)
{
    m_clipboardTab = appConfig.option<Config::clipboard_tab>();
    m_favoritesTab = appConfig.option<Config::flycut_sync_favorites_tab>();

    FlycutSyncSettings settings;
    settings.serverUrl = appConfig.option<Config::flycut_sync_server>().trimmed();
    settings.stateDir = QDir(itemDataPath()).filePath(QStringLiteral("flycut-sync"));
    settings.bridgeStateDir = appConfig.option<Config::flycut_sync_bridge_state>();

    const bool enabled = appConfig.option<Config::flycut_sync>();
    if ( enabled && !settings.serverUrl.isEmpty() ) {
        const QString tokenFile = appConfig.option<Config::flycut_sync_token_file>();
        if ( tokenFile.isEmpty() ) {
            settings.token = readToken();
        } else {
            QFile file(tokenFile);
            if ( file.open(QIODevice::ReadOnly) )
                settings.token = file.readAll().trimmed();
        }
    }

    const bool sameSettings = m_engine
        && settings.serverUrl == m_settings.serverUrl && settings.token == m_settings.token
        && settings.stateDir == m_settings.stateDir && settings.bridgeStateDir == m_settings.bridgeStateDir;
    if (sameSettings)
        return;

    stopEngine();
    m_settings = settings;

    if ( !enabled ) {
        setStatus(tr("Sync is off"), true);
        return;
    }
    if ( settings.serverUrl.isEmpty() ) {
        setStatus(tr("No server address"), false);
        return;
    }
    if ( settings.token.isEmpty() ) {
        setStatus(tr("No token: enter it in Preferences, History"), false);
        return;
    }

    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("FlycutSync"));
    m_engine = new FlycutSyncEngine();
    m_engine->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_engine, &QObject::deleteLater);
    connect(m_engine, &FlycutSyncEngine::remoteChanges, this, &FlycutSync::applyRemoteChanges);
    connect(m_engine, &FlycutSyncEngine::liveHashesLoaded, this, &FlycutSync::assignLegacyIds);
    connect(m_engine, &FlycutSyncEngine::statusChanged, this, &FlycutSync::setStatus);
    m_thread->start();

    setStatus(tr("Connecting to %1").arg(settings.serverUrl), true);
    FlycutSyncEngine *engine = m_engine;
    callEngine([engine, settings](){ engine->start(settings); });
}

void FlycutSync::stopEngine()
{
    if (!m_engine)
        return;
    // Results the engine already queued for this thread are ignored (see isFromStoppedEngine()).
    disconnect(m_engine, nullptr, this, nullptr);
    m_engine = nullptr;
    // Quitting ends the requests a cycle may be waiting for; the engine is deleted in its own thread
    // as the thread finishes, since its database connection and network belong to it.
    m_thread->quit();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
}

bool FlycutSync::isFromStoppedEngine() const
{
    const QObject *engine = sender();
    return engine && engine != m_engine;
}

void FlycutSync::setStatus(const QString &status, bool ok)
{
    if ( isFromStoppedEngine() )
        return;
    m_status = status;
    m_statusOk = ok;
    emit statusChanged(status, ok);
}

int FlycutSync::listForTab(const QString &tabName) const
{
    if ( !m_engine || tabName.isEmpty() )
        return -1;
    if (tabName == m_clipboardTab)
        return HistoryList;
    if (tabName == m_favoritesTab)
        return FavoritesList;
    return -1;
}

QString FlycutSync::tabForList(int list) const
{
    return list == FavoritesList ? m_favoritesTab : m_clipboardTab;
}

void FlycutSync::watchBrowser(ClipboardBrowser *browser)
{
    QAbstractItemModel *model = browser->model();
    connect(model, &QAbstractItemModel::rowsInserted, this,
            [this, browser](const QModelIndex &, int first, int last) { onRowsInserted(browser, first, last); });
    connect(model, &QAbstractItemModel::rowsMoved, this,
            [this, browser](const QModelIndex &, int start, int end, const QModelIndex &, int row) {
                onRowsMoved(browser, start, end, row);
            });
    connect(model, &QAbstractItemModel::dataChanged, this,
            [this, browser](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
                onDataChanged(browser, topLeft.row(), bottomRight.row());
            });
    connect(browser, &ClipboardBrowser::itemsRemovedByUser, this,
            [this, browser](const QList<QPersistentModelIndex> &indexes) { onItemsRemovedByUser(browser, indexes); });
}

void FlycutSync::shareRow(ClipboardBrowser *browser, int list, int row, RankUpdate rankUpdate)
{
    QAbstractItemModel *model = browser->model();
    const QModelIndex index = model->index(row, 0);
    QVariantMap data = index.data(contentType::data).toMap();
    if ( syncedFormats(data).isEmpty() && evictedFormats(data).isEmpty() )
        return;

    QString id = data.value(mimeClipId).toString();
    if ( id.isEmpty() )
        id = newClipId();
    const auto oldRank = rankOf(data);
    const double rank = rankUpdate == KeepRank && oldRank
        ? *oldRank : rankBetween(rankOfRow(model, row - 1), rankOfRow(model, row + 1), nowSeconds());
    const QVariantMap syncData{
        {mimeClipId, id},
        {mimeRank, rank},
        {mimeSignature, contentSignature(data)},
    };
    m_applying = true;
    model->setData(index, syncData, contentType::updateData);
    m_applying = false;

    for (auto it = syncData.constBegin(); it != syncData.constEnd(); ++it)
        data.insert(it.key(), it.value());
    callEngine([engine = m_engine, list, id, data, rank](){ engine->recordPut(list, id, data, rank); });
}

void FlycutSync::onRowsInserted(ClipboardBrowser *browser, int first, int last)
{
    const int list = listForTab(browser->tabName());
    if (m_applying || list == -1)
        return;
    // Bottom-up, so each row's rank fits between its neighbours'.
    for (int row = last; row >= first; --row)
        shareRow(browser, list, row, NewRank);
}

void FlycutSync::onRowsMoved(ClipboardBrowser *browser, int start, int end, int destinationRow)
{
    const int list = listForTab(browser->tabName());
    if (m_applying || list == -1)
        return;
    const int count = end - start + 1;
    const int first = destinationRow > end ? destinationRow - count : destinationRow;
    for (int row = first + count - 1; row >= first; --row)
        shareRow(browser, list, row, NewRank);
}

void FlycutSync::onDataChanged(ClipboardBrowser *browser, int first, int last)
{
    const int list = listForTab(browser->tabName());
    if (m_applying || list == -1)
        return;
    QAbstractItemModel *model = browser->model();
    for (int row = first; row <= last; ++row) {
        const QVariantMap data = itemData(model, row);
        const QString id = data.value(mimeClipId).toString();
        if ( id.isEmpty() )
            continue;
        const QByteArray signature = contentSignature(data);
        const QByteArray oldSignature = data.value(mimeSignature).toByteArray();
        if (signature == oldSignature)
            continue;
        if ( oldSignature.isEmpty() ) {
            m_applying = true;
            model->setData(model->index(row, 0), QVariantMap{{mimeSignature, signature}}, contentType::updateData);
            m_applying = false;
            continue;
        }
        shareRow(browser, list, row, KeepRank);
    }
}

void FlycutSync::onItemsRemovedByUser(ClipboardBrowser *browser, const QList<QPersistentModelIndex> &indexes)
{
    const int list = listForTab(browser->tabName());
    if (m_applying || list == -1)
        return;
    for (const auto &index : indexes) {
        const QVariantMap data = index.data(contentType::data).toMap();
        const QString id = data.value(mimeClipId).toString();
        const QString hash = id.isEmpty() ? keyHash(syncedFormats(data)) : QString();
        if ( id.isEmpty() && hash.isEmpty() )
            continue;
        callEngine([engine = m_engine, list, id, hash](){ engine->recordDel(list, id, hash); });
    }
}

void FlycutSync::applyRemoteChanges(
        int list, const QVariantList &puts, const QStringList &removedIds, const QStringList &removedHashes)
{
    if ( isFromStoppedEngine() )
        return;
    const QString tabName = tabForList(list);
    ClipboardBrowser *browser = tabName.isEmpty() ? nullptr : m_mainWindow->tab(tabName);
    if ( !browser || !browser->isLoaded() ) {
        log(QStringLiteral("Flycut sync: cannot open tab \"%1\" to apply changes from other machines").arg(tabName), LogWarning);
        return;
    }
    QAbstractItemModel *model = browser->model();
    m_applying = true;

    const QSet<QString> idsToRemove(removedIds.begin(), removedIds.end());
    const QSet<QString> hashesToRemove(removedHashes.begin(), removedHashes.end());
    QList<QPersistentModelIndex> toRemove;
    for (int row = 0; row < model->rowCount(); ++row) {
        const QVariantMap data = itemData(model, row);
        const QString id = data.value(mimeClipId).toString();
        if ( id.isEmpty() ? hashesToRemove.contains(keyHash(syncedFormats(data))) : idsToRemove.contains(id) )
            toRemove.append(QPersistentModelIndex(model->index(row, 0)));
    }
    for (const auto &index : toRemove) {
        if ( index.isValid() )
            model->removeRow(index.row());
    }

    // Mirrors of the rows' ids, item key hashes and ranks, kept up to date while placing.
    QVector<QString> ids;
    QVector<QString> hashes;
    QVector<std::optional<double>> ranks;
    const auto readRows = [&]() {
        ids.clear();
        hashes.clear();
        ranks.clear();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QVariantMap data = itemData(model, row);
            ids.append(data.value(mimeClipId).toString());
            hashes.append(keyHash(syncedFormats(data)));
            ranks.append(rankOf(data));
        }
    };
    readRows();

    const double now = nowSeconds();
    for (const auto &value : puts) {
        const QVariantMap put = value.toMap();
        const QString id = put.value(QStringLiteral("id")).toString();
        const double rank = put.value(QStringLiteral("rank")).toDouble();
        QVariantMap data = put.value(QStringLiteral("data")).toMap();
        const QString hash = keyHash(syncedFormats(data));

        // The same clipping, or a copy of the same content (which takes the newer clipping's id).
        int row = ids.indexOf(id);
        if (row == -1)
            row = hashes.indexOf(hash);

        if (row != -1) {
            const QVariantMap oldData = itemData(model, row);
            for (auto it = oldData.constBegin(); it != oldData.constEnd(); ++it) {
                if ( it.key().startsWith(QLatin1String(COPYQ_MIME_PREFIX)) && !data.contains(it.key()) )
                    data.insert(it.key(), it.value());
            }
            data.insert(mimeSignature, contentSignature(data));
            model->setData(model->index(row, 0), data, contentType::data);
            ids.removeAt(row);
            hashes.removeAt(row);
            ranks.removeAt(row);
            const int target = rowForRank(ranks, rank, now);
            if (target != row)
                model->moveRow(QModelIndex(), row, QModelIndex(), target > row ? target + 1 : target);
            ids.insert(target, id);
            hashes.insert(target, hash);
            ranks.insert(target, rank);
            continue;
        }

        data.insert(mimeSignature, contentSignature(data));
        const int target = rowForRank(ranks, rank, now);
        const int countBefore = model->rowCount();
        if ( !browser->add(data, target) )
            continue;
        if (model->rowCount() == countBefore + 1) {
            ids.insert(target, id);
            hashes.insert(target, hash);
            ranks.insert(target, rank);
        } else {
            // The tab's item limit dropped items to make space.
            readRows();
        }
    }

    m_applying = false;
}

void FlycutSync::assignLegacyIds(const QVariantMap &hashToId)
{
    if ( isFromStoppedEngine() )
        return;
    if ( hashToId.isEmpty() || m_clipboardTab.isEmpty() )
        return;
    ClipboardBrowser *browser = m_mainWindow->tab(m_clipboardTab);
    if ( !browser || !browser->isLoaded() )
        return;
    QAbstractItemModel *model = browser->model();
    m_applying = true;
    for (int row = 0; row < model->rowCount(); ++row) {
        const QVariantMap data = itemData(model, row);
        if ( data.contains(mimeClipId) )
            continue;
        const QString id = hashToId.value(keyHash(syncedFormats(data))).toString();
        if ( !id.isEmpty() ) {
            model->setData(model->index(row, 0),
                           QVariantMap{{mimeClipId, id}, {mimeSignature, contentSignature(data)}},
                           contentType::updateData);
        }
    }
    m_applying = false;
}

QString FlycutSync::syncNow()
{
    if (!m_engine)
        return m_status;

    static qulonglong lastToken = 0;
    const qulonglong token = ++lastToken;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    // Queued after the cycle's status and remote changes, so they are applied when this returns.
    connect(m_engine, &FlycutSyncEngine::cycleFinished, &loop, [&](const QVariantList &tokens){
        if ( tokens.contains(token) )
            loop.quit();
    });
    callEngine([engine = m_engine, token](){ engine->requestSync(token); });
    timeout.start(120000);
    loop.exec();
    return m_status;
}

QString FlycutSync::historyUrl(QString *error)
{
    if (!m_engine) {
        *error = m_status;
        return {};
    }
    QString url;
    QString engineError;
    callEngineAndWait([&, engine = m_engine](){ url = engine->admissionUrl(&engineError); });
    *error = engineError;
    return url;
}

void FlycutSync::openHistoryInBrowser()
{
    if (!m_engine) {
        emit error(tr("Sync with Flycut is not running: %1").arg(m_status));
        return;
    }
    QPointer<FlycutSync> self(this);
    callEngine([self, engine = m_engine](){
        QString error;
        const QString url = engine->admissionUrl(&error);
        QMetaObject::invokeMethod(qApp, [self, url, error](){
            if (!self)
                return;
            if ( url.isEmpty() )
                emit self->error(tr("Cannot open the history in a browser: %1").arg(error));
            else
                QDesktopServices::openUrl(QUrl(url));
        });
    });
}

void FlycutSync::searchArchive(
        const QString &query, int list, int limit, int offset, QObject *context, SearchCallback callback)
{
    if (!m_engine) {
        FlycutSyncEngine::SearchResult result;
        result.error = m_status;
        callback(result);
        return;
    }
    QPointer<QObject> receiver(context);
    callEngine([=, engine = m_engine](){
        const auto result = engine->searchArchive(query, list, limit, offset);
        if (receiver)
            QMetaObject::invokeMethod(receiver, [receiver, callback, result](){ if (receiver) callback(result); });
    });
}

FlycutSyncEngine::SearchResult FlycutSync::searchArchive(const QString &query, int list, int limit, int offset)
{
    FlycutSyncEngine::SearchResult result;
    if (!m_engine) {
        result.error = m_status;
        return result;
    }
    callEngineAndWait([&, engine = m_engine](){ result = engine->searchArchive(query, list, limit, offset); });
    return result;
}

QVariantMap FlycutSync::restoreEvictedFormats(const QVariantMap &data)
{
    const auto evicted = evictedFormats(data);
    if ( evicted.isEmpty() || !m_engine )
        return data;
    QVariantMap fetched;
    callEngineAndWait([&, engine = m_engine](){ fetched = engine->fetchFormats(evicted); });
    QVariantMap restored = data;
    for (auto it = fetched.constBegin(); it != fetched.constEnd(); ++it) {
        if ( !restored.contains(it.key()) )
            restored.insert(it.key(), it.value());
    }
    return restored;
}
