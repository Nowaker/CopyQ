// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QRegularExpression>
#include <QString>
#include <QVector>

class QByteArray;

/**
 * Tells whether a text is a credential (a password, an API token, a private key),
 * using the rules shared with Flycut and flycut-copyq-bridge (credential-rules.json).
 *
 * Rules follow gitleaks: a regex, optional lowercase keywords one of which must appear
 * in the text before the regex is tried, an optional minimum Shannon entropy of the
 * secret (capture group 1, or the whole match) and optional allowSecret regexes that
 * clear a secret matching any of them. A text matching no rule is still a credential
 * if it is a single word that looks random (the "standalone" heuristic).
 */
class CredentialDetector final
{
public:
    /// Detector with the rules built into the application.
    static const CredentialDetector &instance();

    explicit CredentialDetector(const QByteArray &rulesJson);

    bool isValid() const { return m_valid; }

    /// Version of the rules; sealed data is re-sealed once per version.
    int version() const { return m_version; }

    /// ID of the first rule the text matches, "standalone" for a lone random word, or empty.
    QString ruleIn(const QString &text) const;

    bool isCredential(const QString &text) const { return !ruleIn(text).isEmpty(); }

private:
    struct Rule {
        QString id;
        QRegularExpression regex;
        QStringList keywords;
        double entropy = 0;
        QVector<QRegularExpression> allowSecret;
    };

    struct Standalone {
        int minLength = 0;
        int maxLength = 0;
        double passwordEntropy = 0;
        int tokenMinLength = 0;
        double tokenEntropy = 0;
        int tokenDigitRuns = 0;
        int tokenLongestRun = 0;
        QVector<QRegularExpression> allow;
    };

    bool isLoneRandomWord(const QString &word) const;

    bool m_valid = false;
    int m_version = 0;
    int m_scanLimit = 65536;
    QVector<Rule> m_rules;
    bool m_hasStandalone = false;
    Standalone m_standalone;
};
