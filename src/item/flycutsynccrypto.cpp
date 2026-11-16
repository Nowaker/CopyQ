// SPDX-License-Identifier: GPL-3.0-or-later
#include "flycutsynccrypto.h"

#include "common/encryption.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>

namespace {

constexpr int nonceLength = 16;
constexpr int tagLength = 32;
constexpr int keyLength = 32;
const QByteArray sealedPrefix("v1:");

QByteArray hmacSha256(const QByteArray &key, const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}

} // namespace

FlycutSyncCrypto::FlycutSyncCrypto(const QByteArray &token)
{
    // RFC 5869 HKDF-SHA256, two output blocks.
    const QByteArray info("clipping contents v1");
    const QByteArray prk = hmacSha256(QByteArrayLiteral("flycut-sync"), token);
    QByteArray okm;
    QByteArray block;
    for (char counter = 1; counter <= 2; ++counter) {
        block = hmacSha256(prk, block + info + QByteArray(1, counter));
        okm.append(block);
    }
    m_encryptionKey = okm.left(keyLength);
    m_macKey = okm.mid(keyLength, keyLength);

#ifdef WITH_QCA_ENCRYPTION
    m_valid = Encryption::initialize() && QCA::isSupported("aes256-ctr");
#endif
}

std::optional<QByteArray> FlycutSyncCrypto::ctr(const QByteArray &nonce, const QByteArray &data) const
{
#ifdef WITH_QCA_ENCRYPTION
    if (!m_valid)
        return std::nullopt;
    // CTR is symmetric: the same operation encrypts and decrypts.
    QCA::Cipher cipher(
        QStringLiteral("aes256"), QCA::Cipher::CTR, QCA::Cipher::NoPadding, QCA::Encode,
        QCA::SymmetricKey(m_encryptionKey), QCA::InitializationVector(nonce));
    QCA::SecureArray output = cipher.update(QCA::SecureArray(data));
    if (!cipher.ok())
        return std::nullopt;
    output += cipher.final();
    if (!cipher.ok() || output.size() != data.size())
        return std::nullopt;
    return output.toByteArray();
#else
    Q_UNUSED(nonce)
    Q_UNUSED(data)
    return std::nullopt;
#endif
}

QByteArray FlycutSyncCrypto::tag(const QByteArray &nonce, const QByteArray &ciphertext) const
{
    return hmacSha256(m_macKey, QByteArrayLiteral("flycut-v1") + nonce + ciphertext);
}

QByteArray FlycutSyncCrypto::sealBytes(const QByteArray &data, const QByteArray &nonce) const
{
    if (nonce.size() != nonceLength)
        return {};
    const auto ciphertext = ctr(nonce, data);
    if (!ciphertext)
        return {};
    return nonce + *ciphertext + tag(nonce, *ciphertext);
}

QString FlycutSyncCrypto::sealWithNonce(const QString &plaintext, const QByteArray &nonce) const
{
    const QByteArray sealed = sealBytes(plaintext.toUtf8(), nonce);
    if (sealed.isEmpty())
        return {};
    return QString::fromLatin1(sealedPrefix + sealed.toBase64());
}

QString FlycutSyncCrypto::seal(const QString &plaintext) const
{
    QByteArray nonce(nonceLength, Qt::Uninitialized);
    QRandomGenerator::system()->generate(nonce.begin(), nonce.end());
    return sealWithNonce(plaintext, nonce);
}

QByteArray FlycutSyncCrypto::sealData(const QByteArray &data) const
{
    const QByteArray nonce = hmacSha256(m_macKey, QByteArrayLiteral("flycut-blob-v1") + data).left(nonceLength);
    return sealBytes(data, nonce);
}

std::optional<QByteArray> FlycutSyncCrypto::openData(const QByteArray &sealed) const
{
    if (sealed.size() < nonceLength + tagLength)
        return std::nullopt;
    const QByteArray nonce = sealed.left(nonceLength);
    const QByteArray ciphertext = sealed.mid(nonceLength, sealed.size() - nonceLength - tagLength);
    if ( !constantTimeEquals(sealed.right(tagLength), tag(nonce, ciphertext)) )
        return std::nullopt;
    return ctr(nonce, ciphertext);
}

std::optional<QString> FlycutSyncCrypto::open(const QString &sealed) const
{
    if ( !sealed.startsWith(QLatin1String(sealedPrefix)) )
        return std::nullopt;
    const auto decoded = QByteArray::fromBase64Encoding(
        sealed.mid(sealedPrefix.size()).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return std::nullopt;
    const auto opened = openData(*decoded);
    if (!opened)
        return std::nullopt;
    const QString text = QString::fromUtf8(*opened);
    // Not UTF-8 (sealed by something else): the bridge and Flycut give up too.
    if (text.toUtf8() != *opened)
        return std::nullopt;
    return text;
}
