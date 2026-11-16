// SPDX-License-Identifier: GPL-3.0-or-later
#include "tests_itemlabel.h"

#include "common/mimetypes.h"
#include "item/itemlabel.h"

#include <QTest>

// Expected labels come from describe() and byte_count() of the Python reference
// (flycut-copyq-bridge.py) run on the same bytes, so labels match Flycut's.

namespace {

QByteArray padded(QByteArray data, int size)
{
    data.append(QByteArray(size - data.size(), '\0'));
    return data;
}

QByteArray bigEndian(quint32 value, int bytes)
{
    QByteArray result;
    for (int i = bytes - 1; i >= 0; --i)
        result.append(static_cast<char>((value >> (8 * i)) & 0xFF));
    return result;
}

QByteArray littleEndian16(quint32 value)
{
    return QByteArray(1, static_cast<char>(value & 0xFF)) + QByteArray(1, static_cast<char>(value >> 8));
}

QByteArray png(quint32 width, quint32 height, int size)
{
    return padded(QByteArray("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16) + bigEndian(width, 4) + bigEndian(height, 4), size);
}

QByteArray gif(quint32 width, quint32 height, int frames, int size)
{
    // Global colour table with 4 entries, then a graphic control extension and
    // an image descriptor with one LZW sub-block per frame.
    QByteArray data = "GIF89a" + littleEndian16(width) + littleEndian16(height)
        + QByteArray("\x81\0\0", 3) + QByteArray(12, '\0');
    for (int i = 0; i < frames; ++i) {
        data.append(QByteArray("\x21\xf9\x04\x00\x0a\x00\x00\x00", 8));
        data.append(QByteArray("\x2c\0\0\0\0", 5) + littleEndian16(width) + littleEndian16(height));
        data.append(QByteArray("\x00\x02\x02\x4c\x01\x00", 6));
    }
    return padded(data + "\x3b", size);
}

QByteArray jpeg(quint32 width, quint32 height, int quality, int size)
{
    static const int standardLuminance[64] = {
        16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
        14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
        49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
    };
    // Quantization table as libjpeg scales it for the quality.
    const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    QByteArray table;
    for (int value : standardLuminance)
        table.append(static_cast<char>(std::max(1, std::min(255, (value * scale + 50) / 100))));

    QByteArray data("\xff\xd8\xff\xe0\x00\x10JFIF\0\x01\x01\0\0\x01\0\x01\0\0", 20);
    data.append(QByteArray("\xff\xdb\x00\x43\x00", 5) + table);
    data.append(QByteArray("\xff\xc0\x00\x11\x08", 5) + bigEndian(height, 2) + bigEndian(width, 2)
                + QByteArray("\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01", 10));
    data.append(QByteArray("\xff\xda\x00\x0c\x03\x01\x00\x02\x11\x03\x11\x00\x3f\x00", 14));
    return padded(data, size);
}

QVariantMap item(const QString &format, const QByteArray &data)
{
    return {{format, data}};
}

} // namespace

ItemLabelTests::ItemLabelTests(const TestInterfacePtr &, QObject *parent)
    : QObject(parent)
{
    setProperty("CopyQ_test_id", QStringLiteral("itemlabel"));
}

void ItemLabelTests::pngLabel()
{
    QCOMPARE( itemLabel(item("image/png", png(1920, 1080, 1234567))),
              QString::fromUtf8("[image: PNG, 1920×1080, 1.2 MB]") );
    QCOMPARE( itemLabel(item("image/png", padded("\x89PNG\r\n\x1a\n", 24))),
              QStringLiteral("[image: PNG, 24 bytes]") );
}

void ItemLabelTests::gifLabel()
{
    QCOMPARE( itemLabel(item("image/gif", gif(480, 270, 20, 1100000))),
              QString::fromUtf8("[image: GIF, 480×270, 1.1 MB, 20 frames]") );
    QCOMPARE( itemLabel(item("image/gif", gif(16, 16, 1, 300))),
              QString::fromUtf8("[image: GIF, 16×16, 300 bytes]") );
}

void ItemLabelTests::jpegLabel()
{
    QCOMPARE( itemLabel(item("image/jpeg", jpeg(64, 48, 94, 4000))),
              QString::fromUtf8("[image: JPEG, 64×48, 4 KB, ~94% quality]") );
    QCOMPARE( itemLabel(item("image/jpeg", jpeg(640, 480, 50, 48000))),
              QString::fromUtf8("[image: JPEG, 640×480, 48 KB, ~50% quality]") );
    QCOMPARE( itemLabel(item("image/jpeg", jpeg(8, 8, 10, 999))),
              QString::fromUtf8("[image: JPEG, 8×8, 999 bytes, ~15% quality]") );
}

void ItemLabelTests::otherImageLabel()
{
    const QVariantMap bmp{
        {"image/bmp", padded("BM", 1502)},
        {"application/x-copyq-owner", "x"},
    };
    QCOMPARE( itemLabel(bmp), QStringLiteral("[image: BMP, 2 KB]") );
    QCOMPARE( itemLabel(item("image/x-icon", QByteArray(1, '\0'))), QStringLiteral("[image: X-ICON, 1 byte]") );
}

void ItemLabelTests::pdfLabel()
{
    const QByteArray pdf = padded(
        "%PDF-1.4\n1 0 obj << /Type /Pages /Kids [2 0 R 3 0 R 4 0 R] >>\n"
        "2 0 obj << /Type /Page >>\n3 0 obj <</Type/Page/Parent 1 0 R>>\n"
        "4 0 obj << /Type\n/Page\n>>\n5 0 obj << /Type /PageLabel >>\n", 120000);
    const QVariantMap data{{"application/pdf", pdf}, {"text/html", "<b>x</b>"}};
    QCOMPARE( itemLabel(data), QStringLiteral("[PDF: 3 pages, 120 KB]") );
    QCOMPARE( itemLabel(item("application/pdf", padded("%PDF-1.4\n", 2500))), QStringLiteral("[PDF: 2 KB]") );
}

void ItemLabelTests::uriListLabel()
{
    const QByteArray uris = "file:///home/u/notes%20v2.txt\r\n# comment\r\nhttps://example.com/a?b=1\r\n";
    QCOMPARE( itemLabel(item(mimeUriList, uris)),
              QStringLiteral("[file: notes v2.txt] [link: https://example.com/a?b=1]") );
}

void ItemLabelTests::objectLabel()
{
    const QVariantMap data{
        {"application/x-foo", "0123456789ab"},
        {"application/x-copyq-owner", "me"},
    };
    QCOMPARE( itemLabel(data), QStringLiteral("[object: application/x-foo, 12 bytes]") );
    QCOMPARE( itemLabel(item("application/x-copyq-owner", "me")), QString() );
    QCOMPARE( itemLabel(QVariantMap()), QString() );
}

void ItemLabelTests::evictedLabel()
{
    const QVariantMap data{
        {"application/x-foo", "0123456789ab"},
        {mimeEvictedFormats, R"({"label": "[image: PNG, 1920×1080, 1.2 MB]", "formats": []})"},
    };
    QCOMPARE( itemLabel(data), QString::fromUtf8("[image: PNG, 1920×1080, 1.2 MB]") );
}

void ItemLabelTests::byteCounts()
{
    const QList<QPair<qint64, QString>> expected{
        {0, "0 bytes"}, {1, "1 byte"}, {2, "2 bytes"}, {999, "999 bytes"},
        {1000, "1 KB"}, {1499, "1 KB"}, {1500, "2 KB"}, {2500, "2 KB"}, {3500, "4 KB"},
        {999499, "999 KB"}, {999500, "1000 KB"}, {999999, "1000 KB"},
        {1000000, "1 MB"}, {1050000, "1.1 MB"}, {1250000, "1.2 MB"}, {1234567, "1.2 MB"},
        {999949999, "999.9 MB"}, {999950000, "1000 MB"},
        {1234567890, "1.23 GB"}, {12000000000000, "12000 GB"},
    };
    for (const auto &[bytes, label] : expected)
        QCOMPARE( byteCountLabel(bytes), label );
}
