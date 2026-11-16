// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests_flycutsync.h"

#include "test_utils.h"

#include "common/mimetypes.h"
#include "item/flycutsynccrypto.h"
#include "item/flycutsyncformat.h"
#include "item/flycutsyncstate.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

#include <memory>

using namespace FlycutSyncFormat;

namespace {

// Vectors computed by flycut-copyq-bridge.py (SyncCrypto) and flycut-sync-server (seal.go) for the
// same token, nonce and data; both give the same bytes.
const QByteArray vectorToken("test-token-0123456789abcdef");
const QString vectorPlaintext = QString::fromUtf8("hunter2 Zażółć");
const QString vectorSealedText = QStringLiteral(
    "v1:AAECAwQFBgcICQoLDA0OD1vl+qJrVAF8g7Jj67SgEsKe0NMRmWuBWCeB1xoZBrtGSIHuwnaRZ6Mnypqqd/Z4lg72");
const QByteArray vectorBlob("blob data \x00\xff", 12);
const QByteArray vectorSealedBlob = QByteArray::fromHex(
    "d296ba80219a1b5cf8d948723326f9094502c185674a27e7232f625bf1966537"
    "ebfffdc9ab1792df60fa4814616b3d9338513fa522588445f802544c");

// Looks like a credential to the shared rules (a lone random word).
const QString credentialText = QStringLiteral("Tr0ub4dor&3xQ");

const QByteArray pngData = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");

double nowSeconds()
{
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

QByteArray jsonLine(const QJsonObject &event)
{
    return QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject putEvent(int list, const QString &id, qint64 at, const QString &machine, double rank, const QString &contents)
{
    return QJsonObject{
        {QStringLiteral("v"), 1}, {QStringLiteral("op"), QStringLiteral("put")}, {QStringLiteral("list"), list},
        {QStringLiteral("id"), id}, {QStringLiteral("at"), at}, {QStringLiteral("m"), machine},
        {QStringLiteral("rank"), rank}, {QStringLiteral("contents"), contents},
        {QStringLiteral("type"), QStringLiteral("NSStringPboardType")}, {QStringLiteral("app"), QStringLiteral("Test")},
        {QStringLiteral("ts"), 1700000000},
    };
}

QJsonObject delEvent(int list, const QString &id, qint64 at, const QString &machine)
{
    return QJsonObject{
        {QStringLiteral("v"), 1}, {QStringLiteral("op"), QStringLiteral("del")}, {QStringLiteral("list"), list},
        {QStringLiteral("id"), id}, {QStringLiteral("at"), at}, {QStringLiteral("m"), machine},
    };
}

QJsonObject clearEvent(int list, qint64 at, const QString &machine)
{
    return QJsonObject{
        {QStringLiteral("v"), 1}, {QStringLiteral("op"), QStringLiteral("clear")}, {QStringLiteral("list"), list},
        {QStringLiteral("at"), at}, {QStringLiteral("m"), machine},
    };
}

QVector<QJsonObject> parseLog(const QByteArray &log)
{
    QVector<QJsonObject> events;
    for (const QByteArray &line : log.split('\n')) {
        if ( !line.isEmpty() )
            events.append(QJsonDocument::fromJson(line).object());
    }
    return events;
}

QJsonObject lastEventFor(const QVector<QJsonObject> &events, const QString &key, const QString &value)
{
    QJsonObject found;
    for (const auto &event : events) {
        if (event.value(key).toString() == value)
            found = event;
    }
    return found;
}

} // namespace

/// A flycut-sync-server process with its own data directory and token, on a free loopback port.
class SyncServer final {
public:
    struct Response {
        int status = 0;
        QByteArray body;
        QByteArray logSize;
    };

    QString start()
    {
        const QString binary = qEnvironmentVariable("FLYCUT_SYNC_SERVER");
        if ( binary.isEmpty() )
            return QStringLiteral("FLYCUT_SYNC_SERVER is not set");
        if ( !m_dir.isValid() )
            return QStringLiteral("cannot create a temporary directory");

        token = QUuid::createUuid().toByteArray(QUuid::Id128);
        tokenFile = m_dir.filePath(QStringLiteral("token"));
        QFile file(tokenFile);
        if ( !file.open(QIODevice::WriteOnly) || file.write(token) != token.size() )
            return QStringLiteral("cannot write the token file");
        file.close();

        QTcpServer probe;
        if ( !probe.listen(QHostAddress::LocalHost) )
            return QStringLiteral("no free port");
        const quint16 port = probe.serverPort();
        probe.close();
        url = QStringLiteral("http://127.0.0.1:%1").arg(port);

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&m_process, &QProcess::readyReadStandardError, &loop, [&](){
            m_stderr.append(m_process.readAllStandardError());
            if ( m_stderr.contains("listening on") )
                loop.quit();
        });
        QObject::connect(&m_process, &QProcess::finished, &loop, &QEventLoop::quit);
        m_process.start(binary, {
            QStringLiteral("-listen"), QStringLiteral("127.0.0.1:%1").arg(port),
            QStringLiteral("-data"), m_dir.filePath(QStringLiteral("logs")),
            QStringLiteral("-token-file"), tokenFile,
        });
        timeout.start(10000);
        loop.exec();
        if ( !m_stderr.contains("listening on") )
            return QStringLiteral("flycut-sync-server didn't start: ") + QString::fromUtf8(m_stderr);
        return {};
    }

    ~SyncServer()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished(5000);
        }
    }

    Response request(const QByteArray &method, const QString &path, const QByteArray &body = QByteArray(),
                     const QByteArray &offset = QByteArray())
    {
        QNetworkRequest req(QUrl(url + path));
        req.setRawHeader("Authorization", "Bearer " + token);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/octet-stream"));
        if ( !offset.isEmpty() )
            req.setRawHeader("X-Log-Offset", offset);
        QNetworkReply *reply = m_network.sendCustomRequest(req, method, body);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        Response response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = reply->readAll();
        response.logSize = reply->rawHeader("X-Log-Size");
        reply->deleteLater();
        return response;
    }

    bool append(const QString &machine, const QVector<QJsonObject> &events)
    {
        QByteArray lines;
        for (const auto &event : events)
            lines.append(jsonLine(event));
        return request("POST", QStringLiteral("/v1/logs/") + machine, lines).status == 204;
    }

    QString putBlob(const QString &machine, const QByteArray &data)
    {
        const QString address = sha256Hex(data);
        const int status = request("PUT", QStringLiteral("/v1/blobs/%1/%2").arg(machine, address), data).status;
        return status == 204 ? address : QString();
    }

    QByteArray log(const QString &machine)
    {
        return request("GET", QStringLiteral("/v1/logs/") + machine).body;
    }

    /// Machines with a log on the server.
    QStringList machines()
    {
        QStringList result;
        for (const auto &value : QJsonDocument::fromJson(request("GET", QStringLiteral("/v1/logs")).body).array())
            result.append(value.toObject().value(QStringLiteral("machine")).toString());
        return result;
    }

    /// The log of the CopyQ under test, the only machine not named by the test.
    QString copyqMachine()
    {
        for (const auto &machine : machines()) {
            if ( machine.contains(QLatin1String("-copyq-")) )
                return machine;
        }
        return {};
    }

    QString url;
    QByteArray token;
    QString tokenFile;

private:
    QTemporaryDir m_dir;
    QProcess m_process;
    QByteArray m_stderr;
    QNetworkAccessManager m_network;
};

namespace {

/// Script printing the formats of a row, without the private ones the sync adds.
QString sharedFormats(int row)
{
    return QStringLiteral(
        "print(str(read('?', %1)).split(String.fromCharCode(10))"
        ".filter(function(f) { return f && f.indexOf('application/x-copyq-private-') != 0; }).sort().join(','))").arg(row);
}

QString syncStateDir()
{
    return QDir(qEnvironmentVariable("COPYQ_ITEM_DATA_PATH")).filePath(QStringLiteral("flycut-sync"));
}

} // namespace

#define EVAL(SCRIPT, OUTPUT) \
    TEST( m_test->getClientOutput(Args() << "eval" << (SCRIPT), (OUTPUT)) )

// The server outlives the test function, so that cleanup() turns sync off before the server goes.
#define START_SYNC_SERVER(SERVER) \
    m_server = std::make_shared<SyncServer>(); \
    SyncServer &SERVER = *m_server; \
    do { \
        const QString error = SERVER.start(); \
        if ( error.contains(QLatin1String("FLYCUT_SYNC_SERVER")) ) \
            QSKIP("Set FLYCUT_SYNC_SERVER to a flycut-sync-server binary to run sync tests"); \
        QVERIFY2( error.isEmpty(), qPrintable(error) ); \
    } while (false)

#define ENABLE_SYNC(SERVER, BRIDGE_DIR) \
    RUN("config" << "flycut_sync_token_file" << (SERVER).tokenFile \
        << "flycut_sync_bridge_state" << (BRIDGE_DIR) \
        << "flycut_sync_server" << (SERVER).url \
        << "flycut_sync" << "true", \
        QStringLiteral("flycut_sync=true\nflycut_sync_bridge_state=%1\nflycut_sync_server=%2\nflycut_sync_token_file=%3\n") \
            .arg((BRIDGE_DIR), (SERVER).url, (SERVER).tokenFile))

#define SYNC_NOW() \
    do { \
        QByteArray syncStatus; \
        EVAL("syncNow()", &syncStatus); \
        QVERIFY2( syncStatus.startsWith("Synced as "), syncStatus.constData() ); \
    } while (false)

FlycutSyncTests::FlycutSyncTests(const TestInterfacePtr &test, QObject *parent)
    : QObject(parent)
    , m_test(test)
{
    setProperty("CopyQ_test_id", QStringLiteral("flycutsync"));
}

void FlycutSyncTests::initTestCase()
{
    TEST(m_test->initTestCase());
}

void FlycutSyncTests::cleanupTestCase()
{
    TEST(m_test->cleanupTestCase());
}

void FlycutSyncTests::init()
{
    TEST(m_test->init());
    QDir(syncStateDir()).removeRecursively();
}

void FlycutSyncTests::cleanup()
{
    // Turned off before the server goes, so the state is closed and the next test starts afresh.
    RUN("config" << "flycut_sync" << "false", "false\n");
    m_server.reset();
    TEST(m_test->cleanup());
}

void FlycutSyncTests::cryptoMatchesBridgeAndServer()
{
    const FlycutSyncCrypto crypto(vectorToken);
    QVERIFY(crypto.isValid());

    QCOMPARE( crypto.sealWithNonce(vectorPlaintext, QByteArray::fromHex("000102030405060708090a0b0c0d0e0f")), vectorSealedText );
    QCOMPARE( crypto.open(vectorSealedText).value_or(QString()), vectorPlaintext );
    QCOMPARE( crypto.sealData(vectorBlob), vectorSealedBlob );
    QCOMPARE( crypto.openData(vectorSealedBlob).value_or(QByteArray()), vectorBlob );

    const QString sealed = crypto.seal(vectorPlaintext);
    QVERIFY(sealed.startsWith(QLatin1String("v1:")));
    QVERIFY(sealed != vectorSealedText);
    QCOMPARE( crypto.open(sealed).value_or(QString()), vectorPlaintext );
}

void FlycutSyncTests::cryptoRejectsAlteredData()
{
    const FlycutSyncCrypto crypto(vectorToken);
    QVERIFY( !FlycutSyncCrypto("another-token-0123456789").open(vectorSealedText) );

    QByteArray altered = vectorSealedBlob;
    altered[20] = static_cast<char>(altered[20] ^ 1);
    QVERIFY( !crypto.openData(altered) );
    QVERIFY( !crypto.openData(vectorSealedBlob.left(40)) );
    QVERIFY( !crypto.open(QStringLiteral("v1:not base64!")) );
    QVERIFY( !crypto.open(QStringLiteral("v2:") + vectorSealedText.mid(3)) );
}

void FlycutSyncTests::eventOrdering()
{
    QVERIFY( isNewer(2, QStringLiteral("a"), 1, QStringLiteral("z")) );
    QVERIFY( !isNewer(1, QStringLiteral("z"), 2, QStringLiteral("a")) );
    QVERIFY( isNewer(5, QStringLiteral("m4max-1"), 5, QStringLiteral("m2pro-1")) );
    QVERIFY( !isNewer(5, QStringLiteral("m2pro-1"), 5, QStringLiteral("m4max-1")) );
    QVERIFY( !isNewer(5, QStringLiteral("same"), 5, QStringLiteral("same")) );

    QCOMPARE( pasteboardType(QStringLiteral("image/png")), QStringLiteral("public.png") );
    QCOMPARE( pasteboardType(QStringLiteral("application/x-slack")), QStringLiteral("application/x-slack") );
    QCOMPARE( mimeForPasteboardType(QStringLiteral("public.rtf")), QStringLiteral("text/rtf") );
    QCOMPARE( mimeForPasteboardType(QStringLiteral("public.utf8-plain-text")), QStringLiteral("text/plain") );
    QCOMPARE( mimeForPasteboardType(QStringLiteral("com.apple.flat-rtfd")), QString() );

    const QVariantMap text{{QStringLiteral("text/plain"), QByteArray("abc")}, {QStringLiteral("text/html"), QByteArray("<b>abc</b>")}};
    QCOMPARE( itemKey(text), QStringLiteral("abc") );
    const QVariantMap image{{QStringLiteral("image/png"), pngData}, {QStringLiteral(COPYQ_MIME_PREFIX "item-notes"), QByteArray("x")}};
    QCOMPARE( itemKey(image), QChar(1) + sha256Hex(pngData) );
    QCOMPARE( keyHash(text), sha256Hex("abc") );
    QVERIFY( !syncedFormats(image).contains(QStringLiteral(COPYQ_MIME_PREFIX "item-notes")) );
}

void FlycutSyncTests::rankPlacement()
{
    const double now = 1'700'000'000;
    QCOMPARE( rankBetween(std::nullopt, std::nullopt, now), now );
    QCOMPARE( rankBetween(std::nullopt, now + 5, now), now + 5.001 );
    QCOMPARE( rankBetween(now, std::nullopt, now), now - 1 );
    QCOMPARE( rankBetween(now, now - 10, now), now - 5 );

    const QVector<std::optional<double>> ranks{now - 10, std::nullopt, now - 100000, std::nullopt};
    QCOMPARE( rowForRank(ranks, now - 1, now), 0 );
    // Recent: above items from before sync.
    QCOMPARE( rowForRank(ranks, now - 20, now), 1 );
    // Old: below them, among older ranked items.
    QCOMPARE( rowForRank(ranks, now - 50000, now), 2 );
    QCOMPARE( rowForRank(ranks, now - 200000, now), 4 );
}

void FlycutSyncTests::migrationTakesOverBridgeState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bridgeDir = dir.filePath(QStringLiteral("bridge"));
    const QString stateDir = dir.filePath(QStringLiteral("state"));
    const QString machine = QStringLiteral("nwkr-test-copyq-0a1b2c3d");
    QVERIFY(QDir().mkpath(bridgeDir + QStringLiteral("/blobs/") + machine));

    const QByteArray blob("<b>html</b>");
    const QString address = sha256Hex(blob);
    QFile blobFile(bridgeDir + QStringLiteral("/blobs/") + machine + '/' + address);
    QVERIFY(blobFile.open(QIODevice::WriteOnly));
    blobFile.write(blob);
    blobFile.close();

    QJsonObject put = putEvent(HistoryList, QStringLiteral("M1"), 1000, machine, 1.5, QStringLiteral("migrated"));
    put.insert(QStringLiteral("items"), QJsonArray{QJsonArray{QJsonObject{
        {QStringLiteral("t"), QStringLiteral("public.html")}, {QStringLiteral("b"), address}, {QStringLiteral("n"), blob.size()}}}});
    const QByteArray log = jsonLine(put);
    QFile logFile(bridgeDir + '/' + machine + QStringLiteral(".jsonl"));
    QVERIFY(logFile.open(QIODevice::WriteOnly));
    logFile.write(log);
    logFile.close();

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("bridge-fixture"));
        db.setDatabaseName(bridgeDir + QStringLiteral("/state.sqlite"));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE clips (id TEXT PRIMARY KEY, at INTEGER NOT NULL, machine TEXT NOT NULL,"
                                          " deleted INTEGER NOT NULL, hash TEXT, contents TEXT, ckey TEXT)")));
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO clips VALUES ('M1', 1000, '%1', 0, '%2', 'migrated', NULL)")
                           .arg(machine, sha256Hex("migrated"))));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO clips VALUES ('D1', 900, 'm4max-1', 1, '%1', 'gone', NULL)")
                           .arg(sha256Hex("gone"))));
        const QStringList meta{
            QStringLiteral("('machine', '%1')").arg(machine),
            QStringLiteral("('read:m4max-1', '1234 0a0b')"),
            QStringLiteral("('sent', '%1')").arg(log.size()),
            QStringLiteral("('clear', '500 m4max-1')"),
            QStringLiteral("('sealed:3', '0')"),
            QStringLiteral("('exported', '0')"),
        };
        QVERIFY(query.exec(QStringLiteral("INSERT INTO meta VALUES ") + meta.join(',')));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("bridge-fixture"));

    FlycutSyncState state;
    QString error;
    QVERIFY2(state.open(stateDir, &error), qPrintable(error));
    QVERIFY2(state.migrateFromBridge(bridgeDir, stateDir, &error), qPrintable(error));

    QCOMPARE( state.meta(QStringLiteral("machine")), machine );
    QCOMPARE( state.meta(QStringLiteral("read:m4max-1")), QStringLiteral("1234 0a0b") );
    QCOMPARE( state.meta(QStringLiteral("sent")), QString::number(log.size()) );
    QCOMPARE( state.meta(QStringLiteral("clear:0")), QStringLiteral("500 m4max-1") );
    QCOMPARE( state.meta(QStringLiteral("sealed:3")), QStringLiteral("0") );
    QCOMPARE( state.meta(QStringLiteral("exported")), QString() );

    const auto clip = state.clip(HistoryList, QStringLiteral("M1"));
    QVERIFY(clip.has_value());
    QCOMPARE( clip->at, 1000 );
    QCOMPARE( clip->machine, machine );
    QVERIFY( !clip->deleted );
    QCOMPARE( state.liveIdForHash(HistoryList, sha256Hex("migrated")), QStringLiteral("M1") );
    QVERIFY( state.clip(HistoryList, QStringLiteral("D1"))->deleted );
    QVERIFY( state.liveIdForHash(HistoryList, sha256Hex("gone")).isEmpty() );

    QFile migratedLog(stateDir + '/' + machine + QStringLiteral(".jsonl"));
    QVERIFY(migratedLog.open(QIODevice::ReadOnly));
    QCOMPARE( migratedLog.readAll(), log );
    QFile migratedBlob(stateDir + QStringLiteral("/blobs/") + machine + '/' + address);
    QVERIFY(migratedBlob.open(QIODevice::ReadOnly));
    QCOMPARE( migratedBlob.readAll(), blob );
    QCOMPARE( state.blob(address)->machine, machine );

    // Taken over once: a second call keeps the state as it is.
    state.setMeta(QStringLiteral("sent"), QStringLiteral("7"));
    QVERIFY(state.migrateFromBridge(bridgeDir, stateDir, &error));
    QCOMPARE( state.meta(QStringLiteral("sent")), QStringLiteral("7") );
}

void FlycutSyncTests::sharesLocalChanges()
{
    START_SYNC_SERVER(server);
    ENABLE_SYNC(server, QStringLiteral("/nonexistent"));
    SYNC_NOW();

    RUN("add" << "local text", "");
    RUN("add" << credentialText, "");
    RUN("write" << "text/plain" << "rich" << "text/html" << "<b>rich</b>", "");
    RUN_WITH_INPUT("write" << "image/png" << "-", pngData, "");
    SYNC_NOW();

    const QString machine = server.copyqMachine();
    QVERIFY(!machine.isEmpty());
    const auto events = parseLog(server.log(machine));

    QByteArray id;
    EVAL("str(getItem(3)['" + QString(mimeClipId) + "'])", &id);
    id = id.trimmed();
    const QJsonObject text = lastEventFor(events, QStringLiteral("contents"), QStringLiteral("local text"));
    QCOMPARE( text.value(QStringLiteral("op")).toString(), QStringLiteral("put") );
    QCOMPARE( text.value(QStringLiteral("list")).toInt(), 0 );
    QCOMPARE( text.value(QStringLiteral("id")).toString().toUtf8(), id );
    QVERIFY( !text.contains(QStringLiteral("items")) );

    // A credential is sealed with the token's keys, nowhere in plain text.
    QVERIFY( !server.log(machine).contains(credentialText.toUtf8()) );
    const FlycutSyncCrypto crypto(server.token);
    bool foundSealed = false;
    for (const auto &event : events) {
        if ( crypto.open(event.value(QStringLiteral("enc")).toString()).value_or(QString()) == credentialText )
            foundSealed = !event.contains(QStringLiteral("contents"));
    }
    QVERIFY(foundSealed);

    // Every format travels, as a blob uploaded before its line.
    const QJsonObject rich = lastEventFor(events, QStringLiteral("contents"), QStringLiteral("rich"));
    QJsonObject html;
    for (const auto &value : rich.value(QStringLiteral("items")).toArray().first().toArray()) {
        if (value.toObject().value(QStringLiteral("t")).toString() == QLatin1String("public.html"))
            html = value.toObject();
    }
    QCOMPARE( html.value(QStringLiteral("b")).toString(), sha256Hex("<b>rich</b>") );
    QCOMPARE( server.request("GET", QStringLiteral("/v1/blobs/%1/%2").arg(machine, html.value(QStringLiteral("b")).toString())).body,
              QByteArray("<b>rich</b>") );

    // An item without text is labelled as Flycut labels it.
    QJsonObject image;
    for (const auto &event : events) {
        if ( event.value(QStringLiteral("described")).toBool() )
            image = event;
    }
    QCOMPARE( image.value(QStringLiteral("contents")).toString(), QString::fromUtf8("[image: PNG, 1×1, 70 bytes]") );
    QCOMPARE( image.value(QStringLiteral("type")).toString(), QStringLiteral("public.png") );

    // Copying an item again shares it again, under the same id and a later time.
    TEST( m_test->setClipboard(QByteArray("local text")) );
    WAIT_ON_OUTPUT("read" << "0", "local text");
    SYNC_NOW();
    const auto eventsAfter = parseLog(server.log(machine));
    const QJsonObject again = lastEventFor(eventsAfter, QStringLiteral("contents"), QStringLiteral("local text"));
    QCOMPARE( again.value(QStringLiteral("id")).toString(), text.value(QStringLiteral("id")).toString() );
    QVERIFY( again.value(QStringLiteral("at")).toDouble() > text.value(QStringLiteral("at")).toDouble() );
    QVERIFY( again.value(QStringLiteral("rank")).toDouble() > rich.value(QStringLiteral("rank")).toDouble() );
}

void FlycutSyncTests::appliesOtherMachinesChanges()
{
    START_SYNC_SERVER(server);
    const QString a = QStringLiteral("fake-mac-a");
    const QString b = QStringLiteral("fake-mac-b");
    const double now = nowSeconds();

    QVERIFY(server.append(a, {
        putEvent(0, QStringLiteral("A1"), 1000, a, now - 20, QStringLiteral("remote one")),
        putEvent(0, QStringLiteral("B1"), 1001, a, now - 10, QStringLiteral("remote two")),
        // For one clipping the greatest (at, machine) wins, in whatever order it is read.
        putEvent(0, QStringLiteral("C1"), 3000, a, now - 30, QStringLiteral("newer text")),
        putEvent(0, QStringLiteral("C1"), 2000, a, now - 30, QStringLiteral("older text")),
        putEvent(0, QStringLiteral("D1"), 4000, a, now - 40, QStringLiteral("from a")),
        putEvent(1, QStringLiteral("F1"), 1000, a, now - 30, QStringLiteral("favorite old")),
        putEvent(1, QStringLiteral("F2"), 1001, a, now - 1, QStringLiteral("favorite new")),
    }));
    QVERIFY(server.append(b, {putEvent(0, QStringLiteral("D1"), 4000, b, now - 40, QStringLiteral("from b"))}));

    ENABLE_SYNC(server, QStringLiteral("/nonexistent"));
    SYNC_NOW();
    RUN("read" << "0" << "1" << "2" << "3", "remote two\nremote one\nnewer text\nfrom b");
    RUN("size", "4\n");
    RUN("tab" << "Favorites" << "read" << "0" << "1", "favorite new\nfavorite old");

    // Sealed text and sealed pasteboard data from another machine are opened.
    const FlycutSyncCrypto crypto(server.token);
    const QString sealedHtml = server.putBlob(a, crypto.sealData("<i>secret</i>"));
    QVERIFY(!sealedHtml.isEmpty());
    QJsonObject sealedPut = putEvent(0, QStringLiteral("S1"), 4500, a, now - 5, QString());
    sealedPut.remove(QStringLiteral("contents"));
    sealedPut.insert(QStringLiteral("enc"), crypto.seal(credentialText));
    sealedPut.insert(QStringLiteral("items"), QJsonArray{QJsonArray{QJsonObject{
        {QStringLiteral("t"), QStringLiteral("public.html")}, {QStringLiteral("b"), sealedHtml},
        {QStringLiteral("n"), 13}, {QStringLiteral("s"), true}}}});
    // An item without text gets its formats and no label as text.
    const QString png = server.putBlob(a, pngData);
    QJsonObject imagePut = putEvent(0, QStringLiteral("I1"), 4600, a, now - 50, QStringLiteral("[image: PNG, 1×1, 70 bytes]"));
    imagePut.insert(QStringLiteral("described"), true);
    imagePut.insert(QStringLiteral("items"), QJsonArray{QJsonArray{QJsonObject{
        {QStringLiteral("t"), QStringLiteral("public.png")}, {QStringLiteral("b"), png}, {QStringLiteral("n"), pngData.size()}}}});
    QVERIFY(server.append(a, {sealedPut, imagePut, delEvent(0, QStringLiteral("A1"), 5000, a)}));
    SYNC_NOW();
    RUN("read" << "0", credentialText);
    RUN("read" << "text/html" << "0", "<i>secret</i>");
    RUN("size", "5\n");
    RUN("eval" << sharedFormats(4), "image/png");
    RUN("eval" << "str(toBase64(read('image/png', 4)))", pngData.toBase64() + "\n");
    // Where and when it was copied, for the item's subtitle.
    RUN("eval" << "var d = getItem(0); print([d['" + QString(mimeMachine) + "'], d['" + QString(mimeApp) + "'], d['"
                  + QString(mimeCopiedAt) + "']].map(str).join(','))", "fake-mac-a,Test,1700000000000");

    // A clear removes what was last changed before it, from any machine; later changes stay.
    QVERIFY(server.append(a, {clearEvent(0, 6000, a), putEvent(0, QStringLiteral("E1"), 7000, a, now - 2, QStringLiteral("after clear"))}));
    SYNC_NOW();
    RUN("size", "1\n");
    RUN("read" << "0", "after clear");
}

void FlycutSyncTests::sharesUserRemovalsOnly()
{
    START_SYNC_SERVER(server);
    ENABLE_SYNC(server, QStringLiteral("/nonexistent"));
    RUN("add" << "to delete" << "to keep", "");
    SYNC_NOW();
    QByteArray id;
    EVAL("str(getItem(1)['" + QString(mimeClipId) + "'])", &id);
    id = id.trimmed();
    QVERIFY(!id.isEmpty());

    RUN("remove" << "1", "");
    SYNC_NOW();
    const QString machine = server.copyqMachine();
    const QJsonObject del = lastEventFor(parseLog(server.log(machine)), QStringLiteral("op"), QStringLiteral("del"));
    QCOMPARE( del.value(QStringLiteral("id")).toString().toUtf8(), id );
    QCOMPARE( del.value(QStringLiteral("list")).toInt(), 0 );

    // Dropped by the item limit: nothing is shared.
    RUN("config" << "maxitems" << "2", "2\n");
    RUN("add" << "l1" << "l2", "");
    SYNC_NOW();
    RUN("size", "2\n");
    int dels = 0;
    for (const auto &event : parseLog(server.log(machine)))
        dels += event.value(QStringLiteral("op")).toString() == QLatin1String("del") ? 1 : 0;
    QCOMPARE(dels, 1);
}

void FlycutSyncTests::continuesBridgeHistory()
{
    START_SYNC_SERVER(server);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bridgeDir = dir.filePath(QStringLiteral("bridge"));
    const QString machine = QStringLiteral("nwkr-test-copyq-0a1b2c3d");
    const QString mac = QStringLiteral("fake-mac-a");
    const double now = nowSeconds();
    QVERIFY(QDir().mkpath(bridgeDir + QStringLiteral("/blobs/") + machine));

    // What the bridge left: its log on the server and in its directory, and a Mac log it had read.
    const QByteArray bridgeLog = jsonLine(putEvent(0, QStringLiteral("M1"), 1000, machine, now - 100, QStringLiteral("migrated")));
    QFile logFile(bridgeDir + '/' + machine + QStringLiteral(".jsonl"));
    QVERIFY(logFile.open(QIODevice::WriteOnly));
    logFile.write(bridgeLog);
    logFile.close();
    QVERIFY(server.append(machine, parseLog(bridgeLog)));
    const QByteArray macLog = jsonLine(putEvent(0, QStringLiteral("R1"), 900, mac, now - 200, QStringLiteral("already applied")));
    QVERIFY(server.append(mac, parseLog(macLog)));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("bridge-fixture"));
        db.setDatabaseName(bridgeDir + QStringLiteral("/state.sqlite"));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE clips (id TEXT PRIMARY KEY, at INTEGER NOT NULL, machine TEXT NOT NULL,"
                                          " deleted INTEGER NOT NULL, hash TEXT, contents TEXT, ckey TEXT)")));
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO clips VALUES ('M1', 1000, '%1', 0, '%2', 'migrated', NULL),"
                                          " ('R1', 900, '%3', 0, '%4', 'already applied', NULL)")
                           .arg(machine, sha256Hex("migrated"), mac, sha256Hex("already applied"))));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO meta VALUES ('machine', '%1'), ('sent', '%2'), ('read:%3', '%4 %5')")
                           .arg(machine).arg(bridgeLog.size()).arg(mac).arg(macLog.size())
                           .arg(QString::fromLatin1(macLog.right(32).toHex()))));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("bridge-fixture"));

    // The item the bridge shared is in CopyQ already, without a clipping id.
    RUN("add" << "migrated", "");
    ENABLE_SYNC(server, bridgeDir);
    SYNC_NOW();

    // Same machine, nothing re-uploaded or re-inserted.
    QCOMPARE( server.machines(), QStringList({mac, machine}) );
    QCOMPARE( server.log(machine), bridgeLog );
    RUN("size", "1\n");
    RUN("eval" << "str(getItem(0)['" + QString(mimeClipId) + "'])", "M1\n");

    // Deleting it now reaches the other machines.
    RUN("remove" << "0", "");
    SYNC_NOW();
    const auto events = parseLog(server.log(machine));
    QCOMPARE( events.size(), 2 );
    QCOMPARE( events.last().value(QStringLiteral("op")).toString(), QStringLiteral("del") );
    QCOMPARE( events.last().value(QStringLiteral("id")).toString(), QStringLiteral("M1") );
}

void FlycutSyncTests::searchesArchiveAndOpensHistory()
{
    START_SYNC_SERVER(server);
    const QString a = QStringLiteral("fake-mac-a");
    QVERIFY(server.append(a, {
        putEvent(0, QStringLiteral("N1"), 1000, a, nowSeconds() - 5, QStringLiteral("archive needle")),
        putEvent(0, QStringLiteral("N2"), 1001, a, nowSeconds() - 6, QStringLiteral("something else")),
    }));
    ENABLE_SYNC(server, QStringLiteral("/nonexistent"));
    SYNC_NOW();

    RUN("eval" << "r = searchSyncArchive('needle'); print(r.matched + ':' + r.items.length + ':' + str(r.items[0]['text/plain'])"
                  " + ':' + str(r.items[0]['" + QString(mimeClipId) + "']))",
        "1:1:archive needle:N1");

    QByteArray link;
    EVAL("syncHistoryLink()", &link);
    QVERIFY2( link.startsWith((server.url + QStringLiteral("/web/admit?code=")).toUtf8()), link.constData() );
}

void FlycutSyncTests::refetchesEvictedFormats()
{
    START_SYNC_SERVER(server);
    const QString a = QStringLiteral("fake-mac-a");
    const QByteArray html("<b>bulky</b>");
    const QString address = server.putBlob(a, html);
    QJsonObject put = putEvent(0, QStringLiteral("H1"), 1000, a, nowSeconds() - 5, QStringLiteral("bulky"));
    put.insert(QStringLiteral("items"), QJsonArray{QJsonArray{QJsonObject{
        {QStringLiteral("t"), QStringLiteral("public.html")}, {QStringLiteral("b"), address}, {QStringLiteral("n"), html.size()}}}});
    QVERIFY(server.append(a, {put}));
    ENABLE_SYNC(server, QStringLiteral("/nonexistent"));
    SYNC_NOW();
    RUN("read" << "text/html" << "0", html);

    // The format evicted to save space (as the storage budget leaves it) comes back when the item is used.
    const QByteArray evicted = QJsonDocument(QJsonObject{
        {QStringLiteral("label"), QStringLiteral("bulky")},
        {QStringLiteral("formats"), QJsonArray{QJsonObject{
            {QStringLiteral("format"), QStringLiteral("text/html")}, {QStringLiteral("size"), html.size()},
            {QStringLiteral("sha256"), address}}}},
    }).toJson(QJsonDocument::Compact);
    RUN("write" << "0" << "text/plain" << "evicted copy" << mimeEvictedFormats << evicted, "");
    RUN("eval" << sharedFormats(0), QString(mimeEvictedFormats) + ",text/plain");
    RUN("select" << "0", "");
    WAIT_FOR_CLIPBOARD2(html, "text/html");
}
