// SPDX-License-Identifier: GPL-3.0-or-later

#include "item/credentialdetector.h"

#include "common/log.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

// Matches as flycut-copyq-bridge does (Python's re on str): \w, \s and \b are Unicode-aware.
QRegularExpression compile(const QString &pattern, const QString &ruleId)
{
    QRegularExpression re(pattern, QRegularExpression::UseUnicodePropertiesOption);
    if ( !re.isValid() ) {
        log( QStringLiteral("Credential rule \"%1\": invalid regular expression: %2")
             .arg(ruleId, re.errorString()), LogWarning );
    }
    return re;
}

QVector<QRegularExpression> compileAll(const QJsonValue &patterns, const QString &ruleId)
{
    QVector<QRegularExpression> result;
    for (const auto &pattern : patterns.toArray())
        result.append( compile(pattern.toString(), ruleId) );
    return result;
}

bool matchesAny(const QVector<QRegularExpression> &regexes, const QString &text)
{
    for (const auto &re : regexes) {
        if ( re.match(text).hasMatch() )
            return true;
    }
    return false;
}

// Python's str.isspace(): bidirectional class WS, B or S, or category Zs.
bool isSpace(char32_t c)
{
    const auto direction = QChar::direction(c);
    return direction == QChar::DirWS || direction == QChar::DirB || direction == QChar::DirS
        || QChar::category(c) == QChar::Separator_Space;
}

bool isAsciiLetterOrDigit(char32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool isTokenPunctuation(char32_t c)
{
    return c == '_' || c == '+' || c == '/' || c == '=' || c == '.' || c == '-';
}

// Shannon entropy in bits per character (code point), summed in order of first appearance as Python's Counter does.
double shannonEntropy(const QList<uint> &text)
{
    if ( text.isEmpty() )
        return 0.0;

    QList<std::pair<uint, int>> counts;
    for (const uint c : text) {
        auto it = std::find_if(counts.begin(), counts.end(), [c](const auto &count){ return count.first == c; });
        if (it == counts.end())
            counts.append({c, 1});
        else
            ++it->second;
    }

    const double size = static_cast<double>(text.size());
    double sum = 0.0;
    for (const auto &count : counts) {
        const double p = count.second / size;
        sum += p * std::log2(p);
    }
    return -sum;
}

QString leftCodePoints(const QString &text, int count)
{
    int i = 0;
    for (int n = 0; n < count && i < text.size(); ++n)
        i += (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate()) ? 2 : 1;
    return text.left(i);
}

QString stripped(const QString &text)
{
    const QList<uint> codePoints = text.toUcs4();
    qsizetype begin = 0;
    qsizetype end = codePoints.size();
    while (begin < end && isSpace(codePoints[begin]))
        ++begin;
    while (end > begin && isSpace(codePoints[end - 1]))
        --end;
    QString result;
    for (qsizetype i = begin; i < end; ++i)
        result.append( QChar::fromUcs4(codePoints[i]) );
    return result;
}

QByteArray builtInRules()
{
    QFile file(QStringLiteral(":/credential-rules.json"));
    if ( !file.open(QIODevice::ReadOnly) ) {
        log( QStringLiteral("Cannot open built-in credential rules: %1").arg(file.errorString()), LogError );
        return {};
    }
    return file.readAll();
}

} // namespace

const CredentialDetector &CredentialDetector::instance()
{
    static const CredentialDetector detector(builtInRules());
    return detector;
}

CredentialDetector::CredentialDetector(const QByteArray &rulesJson)
{
    QJsonParseError error;
    const QJsonObject config = QJsonDocument::fromJson(rulesJson, &error).object();
    if (error.error != QJsonParseError::NoError) {
        log( QStringLiteral("Cannot parse credential rules: %1").arg(error.errorString()), LogError );
        return;
    }

    m_version = config.value(QStringLiteral("version")).toInt();
    m_scanLimit = config.value(QStringLiteral("scanLimit")).toInt(65536);

    for (const auto &value : config.value(QStringLiteral("rules")).toArray()) {
        const QJsonObject rule = value.toObject();
        Rule r;
        r.id = rule.value(QStringLiteral("id")).toString();
        r.regex = compile(rule.value(QStringLiteral("regex")).toString(), r.id);
        for (const auto &keyword : rule.value(QStringLiteral("keywords")).toArray())
            r.keywords.append(keyword.toString());
        r.entropy = rule.value(QStringLiteral("entropy")).toDouble(0);
        r.allowSecret = compileAll(rule.value(QStringLiteral("allowSecret")), r.id);
        m_rules.append(r);
    }

    const QJsonValue standaloneValue = config.value(QStringLiteral("standalone"));
    if ( standaloneValue.isObject() ) {
        const QJsonObject s = standaloneValue.toObject();
        m_hasStandalone = true;
        m_standalone.minLength = s.value(QStringLiteral("minLength")).toInt();
        m_standalone.maxLength = s.value(QStringLiteral("maxLength")).toInt();
        m_standalone.passwordEntropy = s.value(QStringLiteral("passwordEntropy")).toDouble();
        m_standalone.tokenMinLength = s.value(QStringLiteral("tokenMinLength")).toInt();
        m_standalone.tokenEntropy = s.value(QStringLiteral("tokenEntropy")).toDouble();
        m_standalone.tokenDigitRuns = s.value(QStringLiteral("tokenDigitRuns")).toInt();
        m_standalone.tokenLongestRun = s.value(QStringLiteral("tokenLongestRun")).toInt();
        m_standalone.allow = compileAll(s.value(QStringLiteral("allow")), QStringLiteral("standalone"));
    }

    m_valid = !m_rules.isEmpty() || m_hasStandalone;
}

QString CredentialDetector::ruleIn(const QString &text) const
{
    const QString limited = stripped( leftCodePoints(text, m_scanLimit) );
    if ( limited.isEmpty() )
        return {};

    const QString lowercase = limited.toLower();
    for (const auto &rule : m_rules) {
        if ( !rule.keywords.isEmpty() ) {
            const bool hasKeyword = std::any_of(
                rule.keywords.begin(), rule.keywords.end(),
                [&](const QString &keyword){ return lowercase.contains(keyword); });
            if (!hasKeyword)
                continue;
        }

        auto it = rule.regex.globalMatch(limited);
        while ( it.hasNext() ) {
            const auto match = it.next();
            const bool hasSecretGroup = rule.regex.captureCount() > 0 && match.capturedStart(1) >= 0;
            const QString secret = match.captured(hasSecretGroup ? 1 : 0);
            if (rule.entropy > 0 && shannonEntropy(secret.toUcs4()) < rule.entropy)
                continue;
            if ( !matchesAny(rule.allowSecret, secret) )
                return rule.id;
        }
    }

    if ( m_hasStandalone && isLoneRandomWord(limited) )
        return QStringLiteral("standalone");

    return {};
}

bool CredentialDetector::isLoneRandomWord(const QString &word) const
{
    const auto &s = m_standalone;
    const QList<uint> codePoints = word.toUcs4();
    const auto length = codePoints.size();
    if (length < s.minLength || length > s.maxLength)
        return false;
    if ( std::any_of(codePoints.begin(), codePoints.end(), isSpace) )
        return false;
    if ( matchesAny(s.allow, word) )
        return false;

    bool lower = false;
    bool upper = false;
    bool digit = false;
    bool other = false;
    bool tokenCharactersOnly = true;
    int digitRuns = 0;
    int longestRun = 0;
    int run = 0;
    bool inDigitRun = false;
    for (const uint c : codePoints) {
        const bool isDigit = c >= '0' && c <= '9';
        lower = lower || (c >= 'a' && c <= 'z');
        upper = upper || (c >= 'A' && c <= 'Z');
        digit = digit || isDigit;
        const bool letterOrDigit = isAsciiLetterOrDigit(c);
        other = other || !letterOrDigit;
        tokenCharactersOnly = tokenCharactersOnly && (letterOrDigit || isTokenPunctuation(c));

        if (isDigit && !inDigitRun)
            ++digitRuns;
        inDigitRun = isDigit;

        run = letterOrDigit ? run + 1 : 0;
        longestRun = std::max(longestRun, run);
    }

    const double entropy = shannonEntropy(codePoints);
    if (lower && upper && digit && other && entropy >= s.passwordEntropy)
        return true;

    return tokenCharactersOnly && (lower || upper) && digit && length >= s.tokenMinLength
        && digitRuns >= s.tokenDigitRuns && longestRun >= s.tokenLongestRun
        && entropy >= s.tokenEntropy;
}
