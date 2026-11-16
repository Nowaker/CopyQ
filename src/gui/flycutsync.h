// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "item/flycutsyncengine.h"

#include <QHash>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include <functional>

class AppConfig;
class ClipboardBrowser;
class MainWindow;
class QThread;

/**
 * Keeps the clipboard tab (Flycut's history, list 0) and the favorites tab (list 1) in sync with
 * flycut-sync-server, as one more machine next to the Macs running Flycut.
 *
 * Items are told apart by a clipping id kept in their data (FlycutSyncFormat::mimeClipId), together
 * with their rank (Flycut's order) and a fingerprint of their content. New, copied-again, moved and
 * edited items become put events; items the user deletes become del events. Items dropped by the tab's
 * item limit write nothing, as Flycut doesn't share removals made only to keep history short.
 *
 * The server work happens in FlycutSyncEngine, on its own thread.
 */
class FlycutSync final : public QObject
{
    Q_OBJECT

public:
    explicit FlycutSync(MainWindow *mainWindow);
    ~FlycutSync();

    /// Starts, restarts or stops syncing as the options say.
    void loadSettings(const AppConfig &appConfig);

    /// Watches a tab's items for changes to share (any tab; only the synced ones are shared).
    void watchBrowser(ClipboardBrowser *browser);

    bool isRunning() const { return m_engine != nullptr; }
    QString status() const { return m_status; }
    bool statusOk() const { return m_statusOk; }
    QString favoritesTab() const { return m_favoritesTab; }

    /// Runs a sync cycle now and returns the status after it.
    QString syncNow();

    /// One-time sign-in link for the server's web history (empty and error set on failure).
    QString historyUrl(QString *error);

    /// Opens the server's web history in the default browser, signed in.
    void openHistoryInBrowser();

    using SearchCallback = std::function<void(const FlycutSyncEngine::SearchResult &result)>;

    /**
     * Searches the whole server history (GET /v1/archive/search). The callback gets the clippings
     * as CopyQ item data, in the thread of the context object, unless the context is gone by then.
     */
    void searchArchive(const QString &query, int list, int limit, int offset, QObject *context, SearchCallback callback);

    /// Same, waiting for the result.
    FlycutSyncEngine::SearchResult searchArchive(const QString &query, int list, int limit, int offset);

    /// Item data with formats evicted to save space fetched back from the server where possible.
    QVariantMap restoreEvictedFormats(const QVariantMap &data);

    static QByteArray readToken();
    static bool writeToken(const QByteArray &token);

signals:
    void statusChanged(const QString &status, bool ok);
    void error(const QString &error);

private:
    int listForTab(const QString &tabName) const;
    QString tabForList(int list) const;
    void stopEngine();
    bool isFromStoppedEngine() const;

    void onRowsInserted(ClipboardBrowser *browser, int first, int last);
    void onRowsMoved(ClipboardBrowser *browser, int start, int end, int destinationRow);
    void onDataChanged(ClipboardBrowser *browser, int first, int last);
    void onItemsRemovedByUser(ClipboardBrowser *browser, const QList<QPersistentModelIndex> &indexes);

    enum RankUpdate { NewRank, KeepRank };

    /// Gives the row an id (if needed), rank (from its neighbours') and fingerprint, and shares it.
    void shareRow(ClipboardBrowser *browser, int list, int row, RankUpdate rankUpdate);

    void applyRemoteChanges(int list, const QVariantList &puts, const QStringList &removedIds, const QStringList &removedHashes);
    void assignLegacyIds(const QVariantMap &hashToId);
    void setStatus(const QString &status, bool ok);

    template <typename Function>
    void callEngine(Function function);

    template <typename Function>
    void callEngineAndWait(Function function);

    MainWindow *m_mainWindow;
    QThread *m_thread = nullptr;
    FlycutSyncEngine *m_engine = nullptr;
    FlycutSyncSettings m_settings;
    QString m_clipboardTab;
    QString m_favoritesTab;
    QString m_status;
    bool m_statusOk = false;
    bool m_applying = false;
};
