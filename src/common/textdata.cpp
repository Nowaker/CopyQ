// SPDX-License-Identifier: GPL-3.0-or-later

#include "textdata.h"

#include "common/mimetypes.h"

#include <QLocale>
#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <iterator>

namespace {

const QLatin1String mimePluginPrefix(COPYQ_MIME_PREFIX "item");

QString escapeHtmlSpaces(const QString &str)
{
    return QString(str)
            .replace(' ', QLatin1String("&nbsp;"))
            .replace('\n', QLatin1String("<br />"));
}

bool isPluginFormat(const QString &mime)
{
    return mime.startsWith(mimePrivatePrefix) || (
        mime.startsWith(mimePluginPrefix)
        && mime.size() > mimePluginPrefix.size()
    );
}

} // namespace

uint hash(const QVariantMap &data)
{
    uint seed = 0;
#if QT_VERSION >= QT_VERSION_CHECK(6,10,0)
    QtPrivate::QHashCombine hash(seed);
#else
    QtPrivate::QHashCombine hash;
#endif

    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        const auto &mime = it.key();

        // Skip some special data.
        if (mime == mimeWindowTitle || mime == mimeOwner || mime == mimeClipboardMode)
            continue;

        if ( isPluginFormat(mime) )
            continue;

        seed = hash(seed, mime);

        const auto &value = it.value();
        if ( value.typeId() == QMetaType::QByteArray )
            seed = hash(seed, value.toByteArray());
        else
            seed = hash(seed, value.toString());
    }

    return seed;
}

QString quoteString(const QString &str)
{
    return QLocale().quoteString(str);
}

QString escapeHtml(const QString &str)
{
    return escapeHtmlSpaces(str.toHtmlEscaped());
}

QString getTextData(const QByteArray &bytes)
{
    // QString::fromUtf8(bytes) ends string at first '\0'.
    return QString::fromUtf8( bytes.constData(), bytes.size() );
}

QString getTextData(const QVariantMap &data, const QString &mime)
{
    if (mime.isEmpty())
        return getTextData(data);

    const auto it = data.find(mime);
    if ( it != data.constEnd() )
        return getTextData( it->toByteArray() );

    return QString();
}

QString getTextData(const QVariantMap &data)
{
    for (const auto &mime : {mimeTextUtf8, mimeText, mimeUriList}) {
        const auto it = data.find(mime);
        if ( it != data.constEnd() )
            return getTextData( it->toByteArray() );
    }

    return QString();
}

void setTextData(QVariantMap *data, const QString &text, const QString &mime)
{
    data->insert(mime, text.toUtf8());
}

void setTextData(QVariantMap *data, const QString &text)
{
    setTextData(data, text, mimeText);
    data->remove(mimeTextUtf8);
}

QVariantMap createDataMap(const QString &format, const QVariant &value)
{
    QVariantMap dataMap;
    dataMap.insert(format, value);
    return dataMap;
}

QVariantMap createDataMap(const QString &format, const QByteArray &value)
{
    return createDataMap( format, QVariant(value) );
}

QVariantMap createDataMap(const QString &format, const QString &value)
{
    return createDataMap( format, value.toUtf8() );
}

QString foldedForSearch(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QString result;
    result.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;

        switch (c.unicode()) {
        case 0x0141: case 0x0142: result.append(u'l'); break; // Ł ł
        case 0x00D8: case 0x00F8: result.append(u'o'); break; // Ø ø
        case 0x0110: case 0x0111: case 0x00D0: case 0x00F0: result.append(u'd'); break; // Đ đ Ð ð
        case 0x0126: case 0x0127: result.append(u'h'); break; // Ħ ħ
        case 0x0166: case 0x0167: result.append(u't'); break; // Ŧ ŧ
        case 0x0131: result.append(u'i'); break; // ı
        case 0x00DF: case 0x1E9E: result.append(QLatin1String("ss")); break; // ß ẞ
        case 0x00C6: case 0x00E6: result.append(QLatin1String("ae")); break; // Æ æ
        case 0x0152: case 0x0153: result.append(QLatin1String("oe")); break; // Œ œ
        case 0x00DE: case 0x00FE: result.append(QLatin1String("th")); break; // Þ þ
        default: result.append(c);
        }
    }
    return result.toCaseFolded();
}

QString accentsRemoved(const QString &text)
{
    if (text.isEmpty())
        return {};

    QString result = text.normalized(QString::NormalizationForm_D);
    const auto newEnd = std::remove_if(
        std::begin(result), std::end(result),
        [](QChar c){ return c.category() == QChar::Mark_NonSpacing; });
    const auto newSize = std::distance(std::begin(result), newEnd);
    result.resize(newSize);
    return result;
}
