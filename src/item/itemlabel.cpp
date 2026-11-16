// SPDX-License-Identifier: GPL-3.0-or-later
#include "itemlabel.h"

#include "common/mimetypes.h"

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <cmath>
#include <cstdio>

namespace {

struct ImageInfo {
    quint32 width = 0;
    quint32 height = 0;
    // Frame count, or estimated quality for JPEG (-1 if unknown).
    int extra = 1;
};

quint32 readBigEndian(const QByteArray &data, qsizetype at, int bytes)
{
    quint32 value = 0;
    for (int i = 0; i < bytes; ++i)
        value = (value << 8) | static_cast<quint8>(data[at + i]);
    return value;
}

quint32 readLittleEndian16(const QByteArray &data, qsizetype at)
{
    return static_cast<quint8>(data[at]) | (static_cast<quint8>(data[at + 1]) << 8);
}

quint8 byteAt(const QByteArray &data, qsizetype at)
{
    return static_cast<quint8>(data[at]);
}

bool pngInfo(const QByteArray &data, ImageInfo *info)
{
    if ( !data.startsWith("\x89PNG\r\n\x1a\n") || data.size() < 24 )
        return false;
    info->width = readBigEndian(data, 16, 4);
    info->height = readBigEndian(data, 20, 4);
    info->extra = 1;
    return true;
}

qsizetype colorTableSize(quint8 flags)
{
    return (flags & 0x80) ? (3 << ((flags & 7) + 1)) : 0;
}

bool gifInfo(const QByteArray &data, ImageInfo *info)
{
    if ( !data.startsWith("GIF8") || data.size() < 13 )
        return false;
    info->width = readLittleEndian16(data, 6);
    info->height = readLittleEndian16(data, 8);

    // Counts image descriptors, walking over extensions and LZW data sub-blocks.
    int frames = 0;
    qsizetype at = 13 + colorTableSize(byteAt(data, 10));
    while (at < data.size()) {
        const quint8 block = byteAt(data, at);
        if (block == 0x2C) {
            ++frames;
            if (at + 10 > data.size())
                break;
            at += 10 + colorTableSize(byteAt(data, at + 9)) + 1;
        } else if (block == 0x21) {
            at += 2;
        } else {
            // The trailer (0x3B) or data this walk doesn't understand.
            break;
        }
        while (at < data.size() && byteAt(data, at) != 0)
            at += byteAt(data, at) + 1;
        at += 1;
    }
    info->extra = std::max(frames, 1);
    return true;
}

const int standardLuminance[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};

/**
 * Size and estimated quality, as Flycut works them out: libjpeg scales the standard
 * luminance table by quality, so the ratio of the first quantization table to it
 * inverts to the quality the encoder was given.
 */
bool jpegInfo(const QByteArray &data, ImageInfo *info)
{
    if ( !data.startsWith("\xff\xd8") )
        return false;

    int standardSum = 0;
    for (int value : standardLuminance)
        standardSum += value;

    qsizetype at = 2;
    int quality = -1;
    while (at + 4 <= data.size() && byteAt(data, at) == 0xFF) {
        const quint8 marker = byteAt(data, at + 1);
        const qsizetype length = readBigEndian(data, at + 2, 2);
        if (marker == 0xDB && quality < 0 && at + 69 <= data.size() && (byteAt(data, at + 4) >> 4) == 0) {
            int sum = 0;
            for (qsizetype i = at + 5; i < at + 69; ++i)
                sum += byteAt(data, i);
            const double scale = sum * 100.0 / standardSum;
            const double estimate = scale <= 100 ? (200 - scale) / 2 : 5000 / scale;
            // Rounds half to even, as Python's round() in the reference implementation.
            quality = static_cast<int>(std::nearbyint(std::max(1.0, std::min(100.0, estimate))));
        }
        if ((marker == 0xC0 || marker == 0xC1 || marker == 0xC2) && at + 9 <= data.size()) {
            info->height = readBigEndian(data, at + 5, 2);
            info->width = readBigEndian(data, at + 7, 2);
        }
        if (marker == 0xDA)
            break;
        at += 2 + length;
    }
    info->extra = quality;
    return true;
}

QString imageName(const QString &format)
{
    static const QHash<QString, QString> names{
        {QStringLiteral("image/png"), QStringLiteral("PNG")},
        {QStringLiteral("image/jpeg"), QStringLiteral("JPEG")},
        {QStringLiteral("image/gif"), QStringLiteral("GIF")},
        {QStringLiteral("image/tiff"), QStringLiteral("TIFF")},
        {QStringLiteral("image/bmp"), QStringLiteral("BMP")},
        {QStringLiteral("image/webp"), QStringLiteral("WEBP")},
        {QStringLiteral("image/svg+xml"), QStringLiteral("SVG")},
        {QStringLiteral("image/heic"), QStringLiteral("HEIC")},
    };
    const auto name = names.value(format);
    return name.isEmpty() ? format.section(QLatin1Char('/'), 1).toUpper() : name;
}

QString imageLabel(const QString &format, const QByteArray &data)
{
    QStringList parts{imageName(format)};
    ImageInfo info;
    const bool hasInfo = pngInfo(data, &info) || gifInfo(data, &info) || jpegInfo(data, &info);
    int frames = 1;
    int quality = -1;
    if (hasInfo && format == QLatin1String("image/jpeg"))
        quality = info.extra;
    else if (hasInfo)
        frames = info.extra;

    if (hasInfo && info.width && info.height)
        parts.append(QStringLiteral("%1×%2").arg(info.width).arg(info.height));
    parts.append(byteCountLabel(data.size()));
    if (quality > 0)
        parts.append(QStringLiteral("~%1% quality").arg(quality));
    if (frames > 1)
        parts.append(QStringLiteral("%1 frames").arg(frames));
    return QStringLiteral("[image: %1]").arg(parts.join(QLatin1String(", ")));
}

bool isPdfSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isWordCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// Counts "/Type /Page" dictionary entries, not "/Pages" or "/PageLabel".
int pdfPageCount(const QByteArray &data)
{
    int pages = 0;
    qsizetype at = 0;
    while ( (at = data.indexOf("/Type", at)) != -1 ) {
        qsizetype end = at + 5;
        while (end < data.size() && isPdfSpace(data[end]))
            ++end;
        if ( data.mid(end, 5) == "/Page" && (end + 5 >= data.size() || !isWordCharacter(data[end + 5])) ) {
            ++pages;
            at = end + 5;
        } else {
            at += 1;
        }
    }
    return pages;
}

QString pdfLabel(const QByteArray &data)
{
    const int pages = pdfPageCount(data);
    if (pages == 0)
        return QStringLiteral("[PDF: %1]").arg(byteCountLabel(data.size()));
    return QStringLiteral("[PDF: %1 page%2, %3]")
        .arg(pages)
        .arg(pages == 1 ? QString() : QStringLiteral("s"), byteCountLabel(data.size()));
}

QString uriListLabel(const QByteArray &data)
{
    static const QRegularExpression lineBreak(
        QStringLiteral("\\r\\n|[\\n\\r\\x{0b}\\x{0c}\\x{1c}-\\x{1e}\\x{85}\\x{2028}\\x{2029}]"));
    QStringList labels;
    for (const auto &uri : QString::fromUtf8(data).split(lineBreak)) {
        if ( uri.isEmpty() || uri.startsWith(QLatin1Char('#')) )
            continue;
        if ( uri.startsWith(QLatin1String("file:")) ) {
            const QString path = QUrl(uri).path(QUrl::FullyDecoded);
            labels.append(QStringLiteral("[file: %1]").arg(path.section(QLatin1Char('/'), -1)));
        } else {
            labels.append(QStringLiteral("[link: %1]").arg(uri));
        }
    }
    return labels.join(QLatin1Char(' '));
}

QString evictedLabel(const QVariantMap &data)
{
    const auto evicted = data.value(mimeEvictedFormats).toByteArray();
    if ( evicted.isEmpty() )
        return {};
    return QJsonDocument::fromJson(evicted).object().value(QLatin1String("label")).toString();
}

} // namespace

QString itemLabel(const QVariantMap &data)
{
    const QString evicted = evictedLabel(data);
    if ( !evicted.isEmpty() )
        return evicted;

    QStringList formats;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if ( !it.key().startsWith(QLatin1String(COPYQ_MIME_PREFIX)) )
            formats.append(it.key());
    }

    for (const auto &format : formats) {
        if ( format.startsWith(QLatin1String("image/")) )
            return imageLabel(format, data[format].toByteArray());
    }

    const QLatin1String pdf("application/pdf");
    if ( data.contains(pdf) )
        return pdfLabel(data[pdf].toByteArray());

    if ( data.contains(mimeUriList) ) {
        const QString label = uriListLabel(data[mimeUriList].toByteArray());
        if ( !label.isEmpty() )
            return label;
    }

    if ( !formats.isEmpty() ) {
        return QStringLiteral("[object: %1, %2]")
            .arg(formats.first(), byteCountLabel(data[formats.first()].toByteArray().size()));
    }

    return {};
}

QString byteCountLabel(qint64 bytes)
{
    if (bytes < 1000)
        return bytes == 1 ? QStringLiteral("1 byte") : QStringLiteral("%1 bytes").arg(bytes);

    struct Unit { const char *name; double scale; int digits; };
    static const Unit units[] = {{"KB", 1e3, 0}, {"MB", 1e6, 1}, {"GB", 1e9, 2}};
    for (const auto &unit : units) {
        if (bytes < unit.scale * 1000 || unit.scale == 1e9) {
            // snprintf rounds exact halves to even, as Python's format() does in Flycut's reference.
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*f", unit.digits, static_cast<double>(bytes) / unit.scale);
            QString number = QString::fromLatin1(buffer);
            if ( number.contains(QLatin1Char('.')) ) {
                while ( number.endsWith(QLatin1Char('0')) )
                    number.chop(1);
                if ( number.endsWith(QLatin1Char('.')) )
                    number.chop(1);
            }
            return QStringLiteral("%1 %2").arg(number, QLatin1String(unit.name));
        }
    }
    return {};
}
