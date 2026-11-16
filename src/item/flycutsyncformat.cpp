// SPDX-License-Identifier: GPL-3.0-or-later
#include "flycutsyncformat.h"

#include "common/mimetypes.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace FlycutSyncFormat {

const QLatin1String mimeClipId(COPYQ_MIME_PRIVATE_PREFIX "flycut-id");
const QLatin1String mimeRank(COPYQ_MIME_PRIVATE_PREFIX "flycut-rank");
const QLatin1String mimeSignature(COPYQ_MIME_PRIVATE_PREFIX "flycut-signature");
const QLatin1String mimeMachine(COPYQ_MIME_PRIVATE_PREFIX "flycut-machine");
const QLatin1String mimeApp(COPYQ_MIME_PRIVATE_PREFIX "flycut-app");
const QLatin1String mimeCopiedAt(COPYQ_MIME_PRIVATE_PREFIX "copied-at");

namespace {

constexpr double recentSeconds = 3600;

const QHash<QString, QString> &mimeToPasteboardType()
{
    static const QHash<QString, QString> types{
        {QStringLiteral("text/plain"), QStringLiteral("public.utf8-plain-text")},
        {QStringLiteral("text/html"), QStringLiteral("public.html")},
        {QStringLiteral("text/rtf"), QStringLiteral("public.rtf")},
        {QStringLiteral("application/rtf"), QStringLiteral("public.rtf")},
        {QStringLiteral("image/png"), QStringLiteral("public.png")},
        {QStringLiteral("image/jpeg"), QStringLiteral("public.jpeg")},
        {QStringLiteral("image/gif"), QStringLiteral("com.compuserve.gif")},
        {QStringLiteral("image/tiff"), QStringLiteral("public.tiff")},
        {QStringLiteral("image/bmp"), QStringLiteral("com.microsoft.bmp")},
        {QStringLiteral("image/webp"), QStringLiteral("org.webmproject.webp")},
        {QStringLiteral("image/svg+xml"), QStringLiteral("public.svg-image")},
        {QStringLiteral("image/heic"), QStringLiteral("public.heic")},
        {QStringLiteral("application/pdf"), QStringLiteral("com.adobe.pdf")},
    };
    return types;
}

const QHash<QString, QString> &pasteboardTypeToMime()
{
    static const QHash<QString, QString> mimes = [](){
        QHash<QString, QString> result;
        const auto &types = mimeToPasteboardType();
        for (auto it = types.constBegin(); it != types.constEnd(); ++it) {
            if ( it.key() != QLatin1String("application/rtf") )
                result.insert(it.value(), it.key());
        }
        return result;
    }();
    return mimes;
}

} // namespace

bool isNewer(qint64 at, const QString &machine, qint64 otherAt, const QString &otherMachine)
{
    if (at != otherAt)
        return at > otherAt;
    return machine.compare(otherMachine) > 0;
}

QString pasteboardType(const QString &mime)
{
    return mimeToPasteboardType().value(mime, mime);
}

QString mimeForPasteboardType(const QString &type)
{
    const auto mime = pasteboardTypeToMime().value(type);
    if ( !mime.isEmpty() )
        return mime;
    return type.contains('/') ? type : QString();
}

QVariantMap syncedFormats(const QVariantMap &data)
{
    QVariantMap formats;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if ( !it.key().startsWith(QLatin1String(COPYQ_MIME_PREFIX)) )
            formats.insert(it.key(), it.value());
    }
    return formats;
}

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString itemKey(const QVariantMap &formats)
{
    const QByteArray text = formats.value(mimeText).toByteArray();
    if ( !text.isEmpty() )
        return QString::fromUtf8(text);

    // QVariantMap keeps formats sorted by name.
    for (auto it = formats.constBegin(); it != formats.constEnd(); ++it) {
        if ( !it.key().startsWith(QLatin1String(COPYQ_MIME_PREFIX)) )
            return QChar(1) + sha256Hex(it.value().toByteArray());
    }
    return {};
}

QString keyHash(const QVariantMap &formats)
{
    const QString key = itemKey(formats);
    return key.isEmpty() ? QString() : sha256Hex(key.toUtf8());
}

bool isBlobAddress(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re.match(text).hasMatch();
}

QSet<QString> blobAddresses(const QByteArray &log)
{
    QSet<QString> addresses;
    for (const QByteArray &line : log.split('\n')) {
        if ( !line.contains("\"items\"") )
            continue;
        const QJsonArray items = QJsonDocument::fromJson(line).object().value(QStringLiteral("items")).toArray();
        for (const auto &item : items) {
            for (const auto &representation : item.toArray()) {
                const QString address = representation.toObject().value(QStringLiteral("b")).toString();
                if ( isBlobAddress(address) )
                    addresses.insert(address);
            }
        }
    }
    return addresses;
}

QVector<EvictedFormat> evictedFormats(const QVariantMap &data)
{
    QVector<EvictedFormat> result;
    const QByteArray json = data.value(mimeEvictedFormats).toByteArray();
    if ( json.isEmpty() )
        return result;
    const QJsonArray formats = QJsonDocument::fromJson(json).object().value(QStringLiteral("formats")).toArray();
    for (const auto &value : formats) {
        const QJsonObject format = value.toObject();
        EvictedFormat evicted;
        evicted.format = format.value(QStringLiteral("format")).toString();
        evicted.size = static_cast<qint64>(format.value(QStringLiteral("size")).toDouble());
        evicted.sha256 = format.value(QStringLiteral("sha256")).toString();
        if ( !evicted.format.isEmpty() && isBlobAddress(evicted.sha256) )
            result.append(evicted);
    }
    return result;
}

QByteArray contentSignature(const QVariantMap &data)
{
    QMap<QString, QString> hashes;
    const QVariantMap formats = syncedFormats(data);
    for (auto it = formats.constBegin(); it != formats.constEnd(); ++it)
        hashes.insert(it.key(), sha256Hex(it.value().toByteArray()));
    for (const auto &evicted : evictedFormats(data))
        hashes.insert(evicted.format, evicted.sha256);

    QByteArray text;
    for (auto it = hashes.constBegin(); it != hashes.constEnd(); ++it)
        text.append(it.key().toUtf8() + '\n' + it.value().toLatin1() + '\n');
    return QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex();
}

double rankBetween(std::optional<double> newerRank, std::optional<double> olderRank, double now)
{
    if (newerRank && olderRank)
        return (*newerRank + *olderRank) / 2;
    if (newerRank)
        return *newerRank - 1;
    // Stepping past an older top by a millisecond keeps rapid moves from running ahead of the clock.
    return olderRank ? std::max(*olderRank + 0.001, now) : now;
}

int rowForRank(const QVector<std::optional<double>> &ranks, double rank, double now)
{
    const bool recent = rank >= now - recentSeconds;
    for (int row = 0; row < ranks.size(); ++row) {
        const auto &rowRank = ranks[row];
        if (rowRank ? rank > *rowRank : recent)
            return row;
    }
    return ranks.size();
}

} // namespace FlycutSyncFormat
