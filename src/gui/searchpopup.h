// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QVariantMap>
#include <QWidget>

#include <functional>

class ClipboardBrowser;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPlainTextEdit;
class QStackedWidget;
class SearchPopupModel;

/**
 * Popup for searching items and pasting one, as Klipper's popup does.
 *
 * It opens near the mouse pointer and stays open until an item is chosen or Esc
 * is pressed twice (the first Esc clears the search). Typing filters items of the
 * clipboard tab (Tab switches to the favorites tab): every word must appear in
 * the item, ignoring case and diacritics. Enter pastes the current item into the
 * window that was active before, Ctrl+Enter only copies it, Ctrl+Backspace
 * removes it from the tab.
 *
 * When an archive search is set (synchronization with a server), the last row
 * searches the whole archive on the server; items found there that aren't held
 * locally are listed below it.
 */
class SearchPopup final : public QWidget
{
    Q_OBJECT

public:
    struct Style {
        QFont font;
        QColor text;
        QColor background;
        QColor selectedText;
        QColor selectedBackground;
        QColor credential;
        QColor label;
        QColor muted;
    };

    struct ArchiveResult {
        int matched = 0;
        QVector<QVariantMap> items;
        QString error;
    };
    using ArchiveCallback = std::function<void(const ArchiveResult &)>;
    /// Searches the server archive (list 0: history, 1: favorites); calls back in the context object's thread.
    using ArchiveSearch = std::function<void(const QString &query, int list, QObject *context, const ArchiveCallback &callback)>;
    /// Returns the browser with the tab's items, loading the tab if needed, or null if there is no such tab.
    using BrowserForTab = std::function<ClipboardBrowser*(const QString &tabName)>;

    explicit SearchPopup(QWidget *parent = nullptr);
    ~SearchPopup();

    void setStyle(const Style &style);
    void setBrowserForTab(const BrowserForTab &browserForTab) { m_browserForTab = browserForTab; }
    void setTabs(const QString &clipboardTab, const QString &favoritesTab);
    /// Archive search, or null if there is no server to search.
    void setArchiveSearch(const ArchiveSearch &archiveSearch) { m_archiveSearch = archiveSearch; }

    /// Opens the popup at the given point on screen, or closes it if it's open.
    bool toggle(QPoint pos);

signals:
    /**
     * An item was chosen; paste it into the previously active window if paste is true.
     * An item from the server archive isn't in any tab yet.
     */
    void itemActivated(const QVariantMap &data, const QString &tabName, bool paste, bool fromArchive);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void showTab(int tabIndex);
    void reloadTab();
    void updateFilter();
    void updatePreview();
    void updateStatus();
    void activate(const QModelIndex &index, bool paste);
    void removeCurrent();
    void searchArchive();
    bool handleKey(QKeyEvent *event);
    void setCurrentRow(int row);
    QString tabLabel(int tabIndex) const;

    QLineEdit *m_filter;
    QListView *m_list;
    QStackedWidget *m_preview;
    QPlainTextEdit *m_previewText;
    QLabel *m_previewImage;
    QLabel *m_status;
    SearchPopupModel *m_model;

    Style m_style;
    BrowserForTab m_browserForTab;
    ArchiveSearch m_archiveSearch;
    QStringList m_tabs;
    int m_tabIndex = 0;
    QPointer<ClipboardBrowser> m_browser;
    int m_archiveRequest = 0;
    bool m_forwardingKey = false;
};
