// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests_itemeviction.h"

#include "common/mimetypes.h"
#include "item/itemeviction.h"
#include "item/itemlabel.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

namespace {

BinaryDataUsage usage(qint64 bytes, double ageDays, bool keep = false)
{
    BinaryDataUsage item;
    item.bytes = bytes;
    item.ageDays = ageDays;
    item.keep = keep;
    return item;
}

// The 20 newest rows, which are never evicted.
QList<BinaryDataUsage> newestRows(qint64 bytes = 0)
{
    return QList<BinaryDataUsage>(20, usage(bytes, 100));
}

} // namespace

ItemEvictionTests::ItemEvictionTests(const TestInterfacePtr &, QObject *parent)
    : QObject(parent)
{
    setProperty("CopyQ_test_id", QStringLiteral("itemeviction"));
}

void ItemEvictionTests::underLimitEvictsNothing()
{
    auto items = newestRows();
    items << usage(500, 90) << usage(500, 90);
    QCOMPARE( rowsToEvictBinaryData(items, 1000), QList<int>() );
}

void ItemEvictionTests::evictsHighestScoreFirstUntilUnderTarget()
{
    auto items = newestRows();
    // Scores: bytes * (1 + age_days / 30).
    items << usage(400, 0 + 30)   // row 20: 800
          << usage(300, 90)       // row 21: 1200
          << usage(600, 2)        // row 22: 640
          << usage(100, 300);     // row 23: 1100
    // Total 1400 over the 1000 limit; evict until at most 900.
    QCOMPARE( rowsToEvictBinaryData(items, 1000), QList<int>({21, 23, 20}) );

    // A tie goes to the older row first.
    auto ties = newestRows();
    ties << usage(500, 10) << usage(500, 10) << usage(500, 10);
    QCOMPARE( rowsToEvictBinaryData(ties, 1000), QList<int>({22, 21}) );
}

void ItemEvictionTests::keepsNewestRecentAndPinned()
{
    auto items = newestRows(1000);
    items << usage(1000, 0.5)          // row 20: saved within 24 hours
          << usage(1000, 50, true)     // row 21: pinned
          << usage(1000, 5);           // row 22
    QCOMPARE( rowsToEvictBinaryData(items, 1000), QList<int>({22}) );
}

void ItemEvictionTests::noLimit()
{
    auto items = newestRows();
    items << usage(1000000, 100);
    QCOMPARE( rowsToEvictBinaryData(items, -1), QList<int>() );
}

void ItemEvictionTests::binaryFormats()
{
    QVERIFY( isBinaryDataFormat("image/png") );
    QVERIFY( isBinaryDataFormat("application/pdf") );
    QVERIFY( isBinaryDataFormat("application/x-app-private") );
    QVERIFY( !isBinaryDataFormat("text/plain") );
    QVERIFY( !isBinaryDataFormat("text/html") );
    QVERIFY( !isBinaryDataFormat("text/uri-list") );
    QVERIFY( !isBinaryDataFormat(mimeEvictedFormats) );
    QVERIFY( !isBinaryDataFormat("application/x-copyq-item-notes") );
}

void ItemEvictionTests::withoutBinaryDataRecordsEvictedFormats()
{
    const QByteArray png = QByteArray("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\0\x28\0\0\0\x1e", 24) + QByteArray(1976, '\0');
    const QVariantMap data{
        {"image/png", png},
        {"application/x-app-private", "PRIVATE"},
        {"text/html", "<img>"},
        {"application/x-copyq-item-notes", "NOTES"},
    };

    const QVariantMap evicted = withoutBinaryData(data);
    QCOMPARE( evicted.keys(), QStringList({"application/x-copyq-evicted-formats", "application/x-copyq-item-notes", "text/html"}) );
    QCOMPARE( evicted["text/html"].toByteArray(), QByteArray("<img>") );

    const QJsonObject json = QJsonDocument::fromJson(evicted[mimeEvictedFormats].toByteArray()).object();
    QCOMPARE( json["label"].toString(), QString::fromUtf8("[image: PNG, 40×30, 2 KB]") );
    QCOMPARE( itemLabel(evicted), QString::fromUtf8("[image: PNG, 40×30, 2 KB]") );

    const QJsonArray formats = json["formats"].toArray();
    QCOMPARE( formats.size(), 2 );
    QCOMPARE( formats[0]["format"].toString(), QStringLiteral("application/x-app-private") );
    QCOMPARE( formats[0]["size"].toInt(), 7 );
    QCOMPARE( formats[1]["format"].toString(), QStringLiteral("image/png") );
    QCOMPARE( formats[1]["size"].toInt(), 2000 );
    QCOMPARE( formats[1]["sha256"].toString(),
              QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex()) );

    // Evicting again (formats fetched back meanwhile) keeps the first label and adds to the list.
    QVariantMap refetched = evicted;
    refetched.insert("application/x-other", "OTHER");
    const QJsonObject json2 = QJsonDocument::fromJson(withoutBinaryData(refetched)[mimeEvictedFormats].toByteArray()).object();
    QCOMPARE( json2["label"].toString(), QString::fromUtf8("[image: PNG, 40×30, 2 KB]") );
    QCOMPARE( json2["formats"].toArray().size(), 3 );
}

void ItemEvictionTests::usageOfUnsavedData()
{
    const QVariantMap data{
        {"image/png", QByteArray(300, 'p')},
        {"application/x-app-private", QByteArray(50, 'a')},
        {"text/plain", QByteArray(1000, 't')},
    };
    const QDateTime now = QDateTime::currentDateTime();

    const BinaryDataUsage unsaved = binaryDataUsage(data, now);
    QCOMPARE( unsaved.bytes, 350 );
    QCOMPARE( unsaved.ageDays, 0.0 );

    const BinaryDataUsage added = binaryDataUsage(data, now, now.addDays(-3));
    QCOMPARE( added.bytes, 350 );
    QVERIFY( qAbs(added.ageDays - 3.0) < 0.01 );

    // The copy time wins over when the item was added.
    QVariantMap copied = data;
    copied.insert(mimeCopiedAt, QByteArray::number(now.addDays(-5).toMSecsSinceEpoch()));
    const BinaryDataUsage copiedUsage = binaryDataUsage(copied, now, now.addDays(-3));
    QCOMPARE( copiedUsage.bytes, 350 );
    QVERIFY( qAbs(copiedUsage.ageDays - 5.0) < 0.01 );
}
