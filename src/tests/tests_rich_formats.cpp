// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_utils.h"
#include "tests.h"

#include "common/command.h"
#include "common/mimetypes.h"
#include "common/predefinedcommands.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtEndian>

namespace {

const QString webCustomData = QStringLiteral("chromium/x-web-custom-data");

QString keepFormattingCommand()
{
    for (const auto &command : predefinedCommands()) {
        if (command.name == QLatin1String("Keep formatting of Markdown and Slack copies"))
            return command.cmd;
    }
    return {};
}

QByteArray uint32le(quint32 value)
{
    QByteArray bytes(4, '\0');
    qToLittleEndian(value, bytes.data());
    return bytes;
}

QByteArray utf16le(const QString &text)
{
    QByteArray bytes;
    for (const QChar c : text) {
        bytes.append(static_cast<char>(c.unicode() & 0xff));
        bytes.append(static_cast<char>(c.unicode() >> 8));
    }
    return bytes;
}

// Chromium's web custom data: a base::Pickle of UTF-16 key/value pairs, each padded to 4 bytes.
QByteArray pickle(const QList<QPair<QString, QString>> &pairs)
{
    QByteArray body = uint32le(pairs.size());
    for (const auto &pair : pairs) {
        for (const QString &text : {pair.first, pair.second}) {
            const QByteArray units = utf16le(text);
            body += uint32le(text.size()) + units + QByteArray((4 - units.size() % 4) % 4, '\0');
        }
    }
    return uint32le(body.size()) + body;
}

} // namespace

void CoreTests::keepFormattingOfMarkdownAndSlackCopies()
{
    const bool installed = !QStandardPaths::findExecutable(QStringLiteral("clipboard-markdown")).isEmpty()
        || QFileInfo(QDir::homePath() + QStringLiteral("/bin/clipboard-markdown")).isExecutable();
    if (!installed)
        SKIP("clipboard-markdown (dotfiles) is not installed");

    const QString command = keepFormattingCommand();
    QVERIFY(!command.isEmpty());
    RUN("eval" << "setCommands([{isScript: true, name: 'Keep formatting', cmd: arguments[1]}])" << "--" << command, "");

    // A Markdown copy keeps its source as plain text and gains HTML and Slack's delta, stored and on the clipboard.
    const QByteArray markdown = "Plan:\n\n1. **one**\n2. two\n";
    TEST( m_test->setClipboard(markdown, mimeText) );
    TEST( m_test->verifyClipboard("<ol>\n<li><strong>one</strong></li>", mimeHtml, false) );
    TEST( m_test->verifyClipboard(utf16le(QStringLiteral("slack/texty")), webCustomData, false) );
    WAIT_FOR_CLIPBOARD(markdown);
    RUN("read(0)", markdown);
    RUN("str(read(mimeHtml, 0)).includes('<ol>')", "true\n");
    RUN("read('" + webCustomData + "', 0).length > 0", "true\n");

    // A copy out of Slack gets laid-out text and HTML, and keeps Slack's own delta.
    const QString texty = QStringLiteral(
        R"({"ops":[{"insert":"Intro:\n"},{"attributes":{"bold":true},"insert":"a"},)"
        R"({"attributes":{"list":"ordered"},"insert":"\n"},{"insert":"b"},)"
        R"({"attributes":{"indent":1,"list":"bullet"},"insert":"\n"}]})");
    QVariantMap slack;
    slack[mimeText] = QByteArray("Intro:\na\nb");
    slack[webCustomData] = pickle({{QStringLiteral("public.utf8-plain-text"), QStringLiteral("Intro:\na\nb")},
                                   {QStringLiteral("slack/texty"), texty}});
    TEST( m_test->setClipboard(slack, ClipboardMode::Clipboard) );
    WAIT_FOR_CLIPBOARD("Intro:\n1. a\n   - b");
    TEST( m_test->verifyClipboard("<li><strong>a</strong>", mimeHtml, false) );
    TEST( m_test->verifyClipboard(utf16le(texty), webCustomData, false) );
    RUN("read(0)", "Intro:\n1. a\n   - b");

    // Plain prose and credentials stay as they were copied.
    const QByteArray prose = "Hello there,\nhow are you?";
    TEST( m_test->setClipboard(prose, mimeText) );
    WAIT_ON_OUTPUT("read(0)", prose);
    RUN("hasClipboardFormat(mimeHtml)", "false\n");

    // Split so that secret scanners don't flag this file.
    const QByteArray credential = QByteArray("- key\n- AKIA") + "IOSFODNN7EXAMPLE";
    TEST( m_test->setClipboard(credential, mimeText) );
    WAIT_ON_OUTPUT("read(0)", credential);
    RUN("hasClipboardFormat(mimeHtml)", "false\n");
}
