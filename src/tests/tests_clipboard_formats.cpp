// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_utils.h"
#include "tests.h"

#include "common/mimetypes.h"
#include "common/sleeptimer.h"

#include <QDateTime>
#include <QDirIterator>
#include <QFile>

namespace {

/**
 * Waits until the clipboard monitor stores copies.
 *
 * A configuration change restarts the clipboard monitor, and a monitor that starts
 * ignores the clipboard content it finds, so a copy made while it restarts is lost.
 * Right after starting, the server also reloads its configuration for a theme change
 * event unless an option was changed before. This copies a marker text until the
 * monitor stores it, then removes the marker items.
 */
QByteArray waitForClipboardMonitor(const TestInterfacePtr &test)
{
    QByteArray out;
    QByteArray errors;
    for (int attempt = 0; attempt < 10; ++attempt) {
        const QByteArray marker = "MONITOR_READY_" + QByteArray::number(attempt);
        // Fails while the monitor is still restarting, which takes long on a busy machine.
        errors = test->setClipboard(marker);
        if ( !errors.isEmpty() )
            continue;

        SleepTimer t(3000);
        do {
            if ( test->run(Args("read(0)"), &out) == 0 && out == marker ) {
                const auto removeMarkers = QStringLiteral(
                    "for (var i = size() - 1; i >= 0; --i)"
                    " if (str(read(i)).indexOf('MONITOR_READY_') == 0) remove(i)");
                return test->runClient(Args(removeMarkers), "");
            }
        } while ( t.sleep() );
    }
    return "Clipboard monitor does not store copies: " + errors;
}

} // namespace

void CoreTests::clipboardAllFormats()
{
    RUN("config('maxitems', 101)", "101\n");
    TEST( waitForClipboardMonitor(m_test) );

    const QVariantMap data{
        {mimeText, "ALL_FORMATS"},
        {"application/x-app-private", "PRIVATE"},
        {"text/x-custom", "CUSTOM"},
    };
    TEST( m_test->setClipboard(data) );
    WAIT_ON_OUTPUT("read" << "0", "ALL_FORMATS");
    RUN("read" << "application/x-app-private" << "0", "PRIVATE");
    RUN("read" << "text/x-custom" << "0", "CUSTOM");
    // Neither CopyQ's own formats nor X11 protocol targets are stored.
    RUN("read" << "?" << "0", "application/x-app-private\napplication/x-copyq-private-copied-at\ntext/plain\ntext/x-custom\n");

    // Each copy is stamped with its copy time; copying it again refreshes the time.
    const auto copiedAt = QStringLiteral("str(read('%1', 0))").arg(mimeCopiedAt);
    QByteArray firstCopiedAt;
    QCOMPARE( run(Args() << copiedAt, &firstCopiedAt), 0 );
    const qint64 firstMs = firstCopiedAt.trimmed().toLongLong();
    QVERIFY2( qAbs(QDateTime::currentMSecsSinceEpoch() - firstMs) < 60000, firstCopiedAt.constData() );
    TEST( m_test->setClipboard("OTHER") );
    WAIT_ON_OUTPUT("read" << "0", "OTHER");
    TEST( m_test->setClipboard(data) );
    WAIT_ON_OUTPUT("read" << "0", "ALL_FORMATS");
    RUN("count", "2\n");
    QByteArray secondCopiedAt;
    QCOMPARE( run(Args() << copiedAt, &secondCopiedAt), 0 );
    QVERIFY2( secondCopiedAt.trimmed().toLongLong() > firstMs, secondCopiedAt.constData() );

    // Activating an item puts every stored format back to the clipboard.
    RUN("write" << "text/plain" << "RESTORED" << "application/x-app-private" << "PRIVATE2", "");
    RUN("select" << "0", "");
    WAIT_FOR_CLIPBOARD("RESTORED");
    RUN("clipboard" << "application/x-app-private", "PRIVATE2");

    // Only formats known to plugins are stored with the option off.
    RUN("config" << "store_all_clipboard_formats" << "false", "false\n");
    TEST( waitForClipboardMonitor(m_test) );
    TEST( m_test->setClipboard(QVariantMap{{mimeText, "KNOWN_ONLY"}, {"application/x-app-private", "PRIVATE3"}}) );
    WAIT_ON_OUTPUT("read" << "0", "KNOWN_ONLY");
    RUN("read" << "?" << "0", "application/x-copyq-private-copied-at\ntext/plain\n");
}

void CoreTests::clipboardItemSizeLimit()
{
    RUN("config" << "clipboard_item_size_limit" << "100", "100\n");
    TEST( waitForClipboardMonitor(m_test) );

    // Over the limit, the largest formats are dropped first, never text/plain.
    const QVariantMap data{
        {mimeText, QByteArray(120, 'T')},
        {"application/x-big", QByteArray(60, 'B')},
        {"application/x-medium", QByteArray(40, 'M')},
        {"application/x-small", QByteArray(10, 'S')},
    };
    TEST( m_test->setClipboard(data) );
    WAIT_ON_OUTPUT("read" << "0", QByteArray(120, 'T'));
    RUN("read" << "?" << "0", "application/x-copyq-private-copied-at\ntext/plain\n");

    const QVariantMap data2{
        {mimeText, QByteArray(31, 't')},
        {"application/x-big", QByteArray(60, 'B')},
        {"application/x-small", QByteArray(10, 'S')},
    };
    TEST( m_test->setClipboard(data2) );
    WAIT_ON_OUTPUT("read" << "0", QByteArray(31, 't'));
    RUN("read" << "?" << "0", "application/x-copyq-private-copied-at\napplication/x-small\ntext/plain\n");
}

namespace {

// Records secret copies as items, so tests can wait for each copy to be handled.
const auto recordSecrets = R"(
    setCommands([{
        isScript: true,
        cmd: `global.onSecretClipboardChanged = function() {
            add("SECRET " + str(data(mimeText)));
        }`
    }])
    )";

} // namespace

void CoreTests::clipboardSkipFormats()
{
    RUN(recordSecrets, "");
    WAIT_ON_OUTPUT("commands().length", "1\n");
    RUN("config('maxitems', 101)", "101\n");
    TEST( waitForClipboardMonitor(m_test) );

    // Copies offering a format from clipboard_skip_formats are secret.
    TEST( m_test->setClipboard(QVariantMap{{mimeText, "TRANSIENT"}, {"org.nspasteboard.TransientType", "1"}}) );
    WAIT_ON_OUTPUT("read" << "0", "SECRET TRANSIENT");
    TEST( m_test->setClipboard(QVariantMap{{mimeText, "ONEPASSWORD"}, {"com.agilebits.onepassword", "1"}}) );
    WAIT_ON_OUTPUT("read" << "0", "SECRET ONEPASSWORD");
    TEST( m_test->setClipboard("STORED") );
    WAIT_ON_OUTPUT("read" << "0", "STORED");

    RUN("config('clipboard_skip_formats', ['application/x-custom-skip'])", "application/x-custom-skip\n");
    TEST( waitForClipboardMonitor(m_test) );
    TEST( m_test->setClipboard(QVariantMap{{mimeText, "NOT_LISTED"}, {"org.nspasteboard.TransientType", "1"}}) );
    WAIT_ON_OUTPUT("read" << "0", "NOT_LISTED");
    TEST( m_test->setClipboard(QVariantMap{{mimeText, "CUSTOM_SKIP"}, {"application/x-custom-skip", "1"}}) );
    WAIT_ON_OUTPUT("read" << "0", "SECRET CUSTOM_SKIP");
    RUN("count", "5\n");
}

void CoreTests::clipboardSkipSuspiciousLengths()
{
    RUN(recordSecrets, "");
    WAIT_ON_OUTPUT("commands().length", "1\n");
    RUN("config('maxitems', 101)", "101\n");
    TEST( waitForClipboardMonitor(m_test) );

    TEST( m_test->setClipboard("Abcdef1!ghij") );
    WAIT_ON_OUTPUT("read" << "0", "Abcdef1!ghij");

    RUN("config" << "clipboard_skip_suspicious_lengths" << "true", "true\n");
    TEST( waitForClipboardMonitor(m_test) );

    // Single word, listed length (12, 20, 32), upper and lower case, digit and symbol.
    TEST( m_test->setClipboard("Xyzabc2#qwer") );
    WAIT_ON_OUTPUT("read" << "0", "SECRET Xyzabc2#qwer");
    TEST( m_test->setClipboard("Aa1!Aa1!Aa1!Aa1!Aa1!") );
    WAIT_ON_OUTPUT("read" << "0", "SECRET Aa1!Aa1!Aa1!Aa1!Aa1!");

    // No symbol, a space, or another length: not secret.
    TEST( m_test->setClipboard("Xyzabc2qwert") );
    WAIT_ON_OUTPUT("read" << "0", "Xyzabc2qwert");
    TEST( m_test->setClipboard("Xyz bc2#qwer") );
    WAIT_ON_OUTPUT("read" << "0", "Xyz bc2#qwer");
    TEST( m_test->setClipboard("Xyzabc2#qwe") );
    WAIT_ON_OUTPUT("read" << "0", "Xyzabc2#qwe");

    RUN("config" << "clipboard_suspicious_lengths" << "11", "11\n");
    TEST( waitForClipboardMonitor(m_test) );
    TEST( m_test->setClipboard("Xyzabc3#qwe") );
    WAIT_ON_OUTPUT("read" << "0", "SECRET Xyzabc3#qwe");
    TEST( m_test->setClipboard("Xyzabc3#qwer") );
    WAIT_ON_OUTPUT("read" << "0", "Xyzabc3#qwer");
    RUN("count", "8\n");
}

void CoreTests::evictBinaryData()
{
    // 25 items with 2000+ bytes of binary data each; the newest written ends up on top.
    RUN("for (var i = 0; i < 25; ++i) write('text/plain', 'ITEM' + i, 'application/x-bin', 'x'.repeat(2000) + i)", "");
    RUN("count", "25\n");

    // Items added in this session are new, so nothing is evicted yet.
    RUN("config" << "binary_data_limit" << "10K", "10K\n");
    RUN("str(read('application/x-bin', 24)).length", "2001\n");

    // Make the saved data files ten days old, as if copied then, and reload the tab.
    TEST( m_test->stopServer() );
    const QDateTime old = QDateTime::currentDateTime().addDays(-10);
    QDirIterator it(qEnvironmentVariable("COPYQ_ITEM_DATA_PATH"), {"*.dat"}, QDir::Files, QDirIterator::Subdirectories);
    int files = 0;
    while ( it.hasNext() ) {
        QFile file(it.next());
        QVERIFY( file.open(QIODevice::ReadWrite) );
        QVERIFY( file.setFileTime(old, QFileDevice::FileModificationTime) );
        ++files;
    }
    QVERIFY2( files >= 25, qPrintable(QStringLiteral("data files: %1").arg(files)) );
    TEST( m_test->startServer() );

    // 50 KB is over the 10 KB limit: the binary data of the bulkiest old items goes,
    // except the 20 newest; the items stay with their text and label.
    WAIT_ON_OUTPUT("str(read('application/x-bin', 20)).length", "0\n");
    RUN("count", "25\n");
    RUN("separator" << "," << "read" << "20" << "24", "ITEM4,ITEM0");
    RUN("str(read('application/x-bin', 24)).length", "0\n");
    RUN("str(read('application/x-bin', 19)).length", "2001\n");
    RUN("str(read('application/x-bin', 0)).length", "2002\n");

    const auto script = QStringLiteral(
        "var e = JSON.parse(str(read('%1', 24)));"
        "print([e.label, e.formats.length, e.formats[0].format, e.formats[0].size,"
        " e.formats[0].sha256 == sha256sum('x'.repeat(2000) + '0')].join(','))").arg(mimeEvictedFormats);
    RUN(script, "[object: application/x-bin, 2 KB],1,application/x-bin,2001,true");
    RUN("read" << mimeEvictedFormats << "19", "");
}
