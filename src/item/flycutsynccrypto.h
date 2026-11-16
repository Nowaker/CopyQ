// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

/**
 * The sealed format Flycut uses for credentials in sync logs (FlycutEngine/FlycutSyncCrypto.h),
 * also read by flycut-sync-server and flycut-copyq-bridge.
 *
 * Keys come from the sync server token: HKDF-SHA256 with salt "flycut-sync" and info
 * "clipping contents v1" gives 64 bytes, an AES-256 key and an HMAC-SHA256 key.
 *
 * Sealed data is nonce (16 bytes) || AES-256-CTR ciphertext || HMAC-SHA256 tag (32 bytes)
 * over "flycut-v1" || nonce || ciphertext. A sealed text is "v1:" + base64 of that, with a
 * random nonce; a sealed blob is the raw bytes, with the nonce derived from the data
 * (HMAC of "flycut-blob-v1" || data), so equal data seals to the same blob.
 */
class FlycutSyncCrypto final
{
public:
    explicit FlycutSyncCrypto(const QByteArray &token);

    /// False if AES-256-CTR is not available (built without QCA, or its OpenSSL plugin is missing).
    bool isValid() const { return m_valid; }

    /// Seals text with a random nonce; empty on failure.
    QString seal(const QString &plaintext) const;

    /// Seals text with the given 16-byte nonce; for tests against other implementations.
    QString sealWithNonce(const QString &plaintext, const QByteArray &nonce) const;

    /// Seals blob data deterministically; empty on failure.
    QByteArray sealData(const QByteArray &data) const;

    std::optional<QByteArray> openData(const QByteArray &sealed) const;

    std::optional<QString> open(const QString &sealed) const;

private:
    QByteArray sealBytes(const QByteArray &data, const QByteArray &nonce) const;
    QByteArray tag(const QByteArray &nonce, const QByteArray &ciphertext) const;
    std::optional<QByteArray> ctr(const QByteArray &nonce, const QByteArray &data) const;

    QByteArray m_encryptionKey;
    QByteArray m_macKey;
    bool m_valid = false;
};
