// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class FlycutSync;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

/// Preferences page for syncing with Flycut through flycut-sync-server.
class ConfigTabSync final : public QWidget
{
    Q_OBJECT
public:
    explicit ConfigTabSync(QWidget *parent = nullptr);

    /// Shows the sync's status and enables opening the web history.
    void setFlycutSync(FlycutSync *flycutSync);

    /// Stores a newly entered token in the system keychain.
    void saveToken();

    QCheckBox *checkBoxSync;
    QLineEdit *lineEditServer;
    QLineEdit *lineEditToken;
    QLineEdit *lineEditFavoritesTab;

private:
    void showStatus(const QString &status, bool ok);

    FlycutSync *m_flycutSync = nullptr;
    QLabel *m_labelStatus;
    QPushButton *m_buttonOpenHistory;
};
