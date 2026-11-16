// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_utils.h"
#include "tests.h"
#include "tests_common.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <memory>

namespace {

constexpr auto searchPopupId = "focus:SearchPopup";

/**
 * Stands in for flycut-sync-server: answers what a sync cycle asks (no other machines,
 * uploads accepted) and GET /v1/archive/search with the given put events.
 *
 * It runs in its own thread, so it answers while the test waits for a client command.
 */
class FakeSyncServer final : public QThread
{
public:
    FakeSyncServer(const QByteArray &token, const QJsonArray &clippings)
        : m_token(token)
        , m_clippings(clippings)
    {
        start();
        m_ready.acquire();
    }

    ~FakeSyncServer()
    {
        quit();
        wait();
    }

    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_port); }

    QStringList searchRequests() const
    {
        QMutexLocker lock(&m_mutex);
        return m_searchRequests;
    }

    int unauthorizedRequests() const
    {
        QMutexLocker lock(&m_mutex);
        return m_unauthorized;
    }

protected:
    void run() override
    {
        QTcpServer server;
        server.listen(QHostAddress::LocalHost);
        m_port = server.serverPort();
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]{
            while (QTcpSocket *socket = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]{
                    buffer->append(socket->readAll());
                    const auto headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd == -1)
                        return;
                    const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
                    QHash<QByteArray, QByteArray> headers;
                    for (const auto &line : lines.mid(1)) {
                        const auto colon = line.indexOf(':');
                        if (colon != -1)
                            headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
                    }
                    if (buffer->size() < headerEnd + 4 + headers.value("content-length").toLongLong())
                        return;
                    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
                    socket->write( respond(requestLine.value(0), requestLine.value(1), headers.value("authorization")) );
                    socket->disconnectFromHost();
                });
            }
        });
        m_ready.release();
        exec();
    }

private:
    QByteArray respond(const QByteArray &method, const QByteArray &path, const QByteArray &authorization)
    {
        int status = 404;
        QByteArray body;
        {
            QMutexLocker lock(&m_mutex);
            if (authorization != "Bearer " + m_token) {
                ++m_unauthorized;
                status = 401;
            } else if (method == "GET" && path.startsWith("/v1/archive/search?")) {
                m_searchRequests.append(QString::fromUtf8(path));
                status = 200;
                body = QJsonDocument(QJsonObject{
                    {QStringLiteral("matched"), m_clippings.size()},
                    {QStringLiteral("clippings"), m_clippings},
                }).toJson(QJsonDocument::Compact);
            } else if (method == "GET" && (path == "/v1/logs" || path.count('/') == 3)) {
                // No machine logs, no blobs.
                status = 200;
                body = "[]";
            } else if (method == "PUT" || method == "POST") {
                status = 204;
            }
        }

        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n"
            "Connection: close\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (status == 200)
            response.append("Content-Type: application/json\r\n");
        return response + "\r\n" + body;
    }

    QByteArray m_token;
    QJsonArray m_clippings;
    quint16 m_port = 0;
    QSemaphore m_ready;
    mutable QMutex m_mutex;
    QStringList m_searchRequests;
    int m_unauthorized = 0;
};

QJsonObject putEvent(const QString &id, const QString &contents, const QString &app)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return QJsonObject{
        {QStringLiteral("v"), 1}, {QStringLiteral("op"), QStringLiteral("put")}, {QStringLiteral("list"), 0},
        {QStringLiteral("id"), id}, {QStringLiteral("at"), now * 1000}, {QStringLiteral("m"), QStringLiteral("fake-mac")},
        {QStringLiteral("rank"), static_cast<double>(now - 3600)}, {QStringLiteral("contents"), contents},
        {QStringLiteral("type"), QStringLiteral("NSStringPboardType")}, {QStringLiteral("app"), app},
        {QStringLiteral("ts"), now - 3600},
    };
}

} // namespace

/*
 * The server reloads its configuration a second after the theme changes, which happens
 * as its windows first show. Changing an option reloads it at once and cancels the pending
 * reload, so it doesn't replace the tab's items while the popup is open.
 */
#define RELOAD_CONFIGURATION(FAVORITES_TAB) \
    RUN("config" << "flycut_sync_favorites_tab" << (FAVORITES_TAB), (FAVORITES_TAB) + "\n")

void CoreTests::searchPopupFiltersAndPastes()
{
    // Pasting goes through the paste() override, which records what it pasted.
    const auto pasted = testTab(1);
    const auto script = QStringLiteral(R"(
        setCommands([
            {
                isScript: true,
                cmd: 'global.paste = function() { tab("%1"); add("pasted: " + str(clipboard())) }'
            },
        ])
        )").arg(pasted);
    RUN(script, "");
    RUN("add" << "other" << "Søren Đorđević" << "Łódź Kaliska", "");
    KEYS(clipboardBrowserId);
    RELOAD_CONFIGURATION(testTab(2));

    // Letters with strokes match their base letters; Ctrl+Enter only copies.
    RUN("searchPopup", "true\n");
    KEYS(searchPopupId << ":soren dorde" << "CTRL+ENTER" << clipboardBrowserId);
    WAIT_FOR_CLIPBOARD("Søren Đorđević");

    // Every word matches, ignoring case and diacritics; Enter copies and pastes.
    RUN("searchPopup", "true\n");
    KEYS(searchPopupId << ":lodz KAL" << "ENTER" << clipboardBrowserId);
    WAIT_ON_OUTPUT("tab" << pasted << "read" << "0", "pasted: Łódź Kaliska");
    RUN("tab" << pasted << "size", "1\n");
}

void CoreTests::searchPopupKeys()
{
    RUN("add" << "gamma" << "beta" << "alpha", "");
    KEYS(clipboardBrowserId);
    const auto favorites = testTab(2);
    RELOAD_CONFIGURATION(favorites);

    // Ctrl+Backspace removes the current item, the first Esc clears the search, the second closes.
    RUN("searchPopup", "true\n");
    KEYS(searchPopupId << ":beta" << "CTRL+BACKSPACE");
    RUN("separator" << "," << "read" << "0" << "1" << "2", "alpha,gamma,");
    KEYS(searchPopupId << "ESC" << searchPopupId << "ESC" << clipboardBrowserId);

    // Tab switches to the favorites tab.
    RUN("tab" << favorites << "add" << "favorite item", "");
    RUN("add" << "history item", "");
    RUN("searchPopup", "true\n");
    KEYS(searchPopupId << "TAB" << ":item" << "CTRL+ENTER" << clipboardBrowserId);
    WAIT_FOR_CLIPBOARD("favorite item");
}

void CoreTests::searchPopupSearchesArchive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray token = "search-popup-test-token";
    const QString tokenFile = dir.filePath(QStringLiteral("token"));
    QFile file(tokenFile);
    QVERIFY( file.open(QIODevice::WriteOnly) && file.write(token) == token.size() );
    file.close();

    QDir(QDir(qEnvironmentVariable("COPYQ_ITEM_DATA_PATH")).filePath(QStringLiteral("flycut-sync"))).removeRecursively();

    FakeSyncServer server(token, {
        putEvent(QStringLiteral("SRV1"), QStringLiteral("server needle only"), QStringLiteral("Safari")),
        putEvent(QStringLiteral("SRV2"), QStringLiteral("local needle one"), QStringLiteral("Safari")),
    });

    RUN("add" << "local needle one" << "unrelated", "");
    KEYS(clipboardBrowserId);

    RUN("config" << "flycut_sync_token_file" << tokenFile
        << "flycut_sync_bridge_state" << "/nonexistent"
        << "flycut_sync_server" << server.url()
        << "flycut_sync" << "true",
        QStringLiteral("flycut_sync=true\nflycut_sync_bridge_state=/nonexistent\nflycut_sync_server=%1\nflycut_sync_token_file=%2\n")
            .arg(server.url(), tokenFile));

    // The row below the one local match searches the server; the clipping held locally isn't listed again.
    RUN("searchPopup", "true\n");
    KEYS(searchPopupId << ":needle" << "DOWN" << "ENTER");
    KEYS(QStringLiteral("focus:SearchPopup'.*2 found on server") << "CTRL+ENTER" << clipboardBrowserId);
    WAIT_FOR_CLIPBOARD("server needle only");
    RUN("read" << "0", "server needle only");

    QCOMPARE( server.searchRequests(), QStringList{QStringLiteral("/v1/archive/search?q=needle&list=0")} );
    QCOMPARE( server.unauthorizedRequests(), 0 );

    RUN("config" << "flycut_sync" << "false", "false\n");
}
