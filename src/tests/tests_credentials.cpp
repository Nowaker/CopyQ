// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_utils.h"
#include "tests.h"
#include "tests_common.h"

// Expected rules are what flycut-copyq-bridge.py's CredentialDetector returns for the same texts.
// Credential-looking literals are split so that secret scanners don't flag this file.
void CoreTests::credentialRule()
{
    RUN("credentialRule('-----BEGIN RSA ' + 'PRIVATE KEY-----' + String.fromCharCode(10) + 'MIIEowIBAAKCAQEA')",
        "private-key\n");
    RUN("credentialRule('AKIA' + 'IOSFODNN7EXAMPLE')", "aws-access-token\n");
    RUN("credentialRule('  AKIA' + 'IOSFODNN7EXAMPLE' + String.fromCharCode(10))", "aws-access-token\n");
    RUN("credentialRule('token: gh' + 'p_' + 'aB3dE5fG7hJ9kL1mN3pQ5rS7tU9vW1xY3zA5')", "github-token\n");
    RUN("credentialRule('glp' + 'at-' + 'Xy9Zq8Wv7Ut6Sr5Qp4On')", "gitlab-token\n");
    RUN("credentialRule('https://deploy:hun' + 'ter2xyz@example.com/repo.git')", "url-password\n");
    RUN("credentialRule('https://example.com/cb?tok' + 'en=Ab3dE5fG7hJ9kL1mN3pQ')", "url-token-parameter\n");
    RUN("credentialRule('ey' + 'JhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dBjftJeZ4CVPmB92K27uhbUJU1p1r_wW1gFWFOEjXk')",
        "jwt\n");
    RUN("credentialRule('abcDef-' + 'ghi2kl-mnopqr')", "apple-generated-password\n");

    // Entropy of the secret and allowSecret.
    RUN("credentialRule('api_' + 'key = \"Zx8Kq2Lm9Pv4Rt7Wy1Bn\"')", "generic-api-key\n");
    RUN("credentialRule('secret: \"Źdźbło9Kq2Lm9Pv4Rt\"')", "generic-api-key\n");
    RUN("credentialRule('https://example.com/cb?token=aaaaaaaaaaaaaaaaaaaa')", "");
    RUN("credentialRule('password = \"correct_horse_battery\"')", "");

    // A lone word that looks random.
    RUN("credentialRule('Tr0ub4dor&3xQ')", "standalone\n");
    RUN("credentialRule('Pąssw0rd!xyz1')", "standalone\n");
    RUN("credentialRule('a8f3k2m9q1z7x4c6v5b0n8m2w')", "standalone\n");
    RUN("credentialRule('Password1')", "");
    RUN("credentialRule('550e8400-e29b-41d4-a716-446655440000')", "");
    RUN("credentialRule('3f1c2a9b8d7e6f50')", "");
    RUN("credentialRule('ses_e1cde789fffe5sBxkCeSl6PysP')", "");
    RUN("credentialRule('someone@example.com')", "");
    RUN("credentialRule('~/projekty/forks/copyq2026')", "");

    RUN("credentialRule('hello world')", "");
    RUN("credentialRule('Zażółć gęślą jaźń')", "");
    RUN("credentialRule('https://example.com/some/path?x=1')", "");
    RUN("credentialRule('')", "");
}

// Items without text are labelled by what they hold (itemLabel()), here in the window title.
void CoreTests::windowTitleDescribesData()
{
    const QString png = QStringLiteral(
        "iVBORw0KGgoAAAANSUhEUgAAACgAAAAeCAIAAADRv8uKAAAAKklEQVR4nO3NMQ0AAAgDsAmb/yALGXA06d9MeyJisVgs"
        "FovFYrFYLP4bL9HP3Ew1mJ9PAAAAAElFTkSuQmCC");
    RUN("setData('image/png', fromBase64('" + png + "')); updateTitle()", "");
    WAIT_ON_OUTPUT("currentWindowTitle", appWindowTitle(QStringLiteral("[image: PNG, 40×30, 99 bytes]")));
}
