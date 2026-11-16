// SPDX-License-Identifier: GPL-3.0-or-later
#include "configtabsync.h"

#include "gui/flycutsync.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

ConfigTabSync::ConfigTabSync(QWidget *parent)
    : QWidget(parent)
    , checkBoxSync(new QCheckBox(tr("&Sync history and favorites with Flycut"), this))
    , lineEditServer(new QLineEdit(this))
    , lineEditToken(new QLineEdit(this))
    , lineEditFavoritesTab(new QLineEdit(this))
    , m_labelStatus(new QLabel(this))
    , m_buttonOpenHistory(new QPushButton(tr("Open &History in Browser"), this))
{
    checkBoxSync->setObjectName(QStringLiteral("checkBoxFlycutSync"));
    checkBoxSync->setToolTip(tr("Share the clipboard tab and the favorites tab with Flycut on other machines"
                                " through flycut-sync-server"));

    lineEditServer->setObjectName(QStringLiteral("lineEditFlycutSyncServer"));
    lineEditServer->setPlaceholderText(QStringLiteral("https://flycut.example.ts.net"));
    lineEditServer->setToolTip(tr("Address of flycut-sync-server"));

    lineEditToken->setObjectName(QStringLiteral("lineEditFlycutSyncToken"));
    lineEditToken->setEchoMode(QLineEdit::Password);
    lineEditToken->setPlaceholderText(tr("Leave empty to keep the stored token"));
    lineEditToken->setToolTip(tr("The server's token (the contents of its token file), kept in the system keychain"));

    lineEditFavoritesTab->setObjectName(QStringLiteral("lineEditFlycutSyncFavoritesTab"));
    lineEditFavoritesTab->setToolTip(tr("Tab synced with Flycut's favorites"));

    m_labelStatus->setObjectName(QStringLiteral("labelFlycutSyncStatus"));
    m_labelStatus->setWordWrap(true);
    m_labelStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_buttonOpenHistory->setEnabled(false);

    auto form = new QFormLayout;
    form->addRow(tr("Server &address:"), lineEditServer);
    form->addRow(tr("&Token:"), lineEditToken);
    form->addRow(tr("&Favorites tab:"), lineEditFavoritesTab);
    form->addRow(tr("Status:"), m_labelStatus);

    auto layout = new QVBoxLayout(this);
    layout->addWidget(checkBoxSync);
    layout->addLayout(form);
    layout->addWidget(m_buttonOpenHistory, 0, Qt::AlignLeft);
    layout->addStretch();
}

void ConfigTabSync::setFlycutSync(FlycutSync *flycutSync)
{
    if (!flycutSync)
        return;
    m_flycutSync = flycutSync;
    connect(flycutSync, &FlycutSync::statusChanged, this, &ConfigTabSync::showStatus);
    connect(m_buttonOpenHistory, &QPushButton::clicked, flycutSync, &FlycutSync::openHistoryInBrowser);
    showStatus(flycutSync->status(), flycutSync->statusOk());
}

void ConfigTabSync::saveToken()
{
    const QByteArray token = lineEditToken->text().trimmed().toUtf8();
    if ( token.isEmpty() )
        return;
    if ( FlycutSync::writeToken(token) )
        lineEditToken->clear();
    else
        showStatus(tr("Cannot store the token in the system keychain"), false);
}

void ConfigTabSync::showStatus(const QString &status, bool ok)
{
    m_labelStatus->setText(status);
    m_labelStatus->setStyleSheet(ok ? QString() : QStringLiteral("color: #c00"));
    m_buttonOpenHistory->setEnabled(ok && m_flycutSync && m_flycutSync->isRunning());
}
