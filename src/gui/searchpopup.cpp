// SPDX-License-Identifier: GPL-3.0-or-later

#include "searchpopup.h"

#include "common/common.h"
#include "common/contenttype.h"
#include "common/display.h"
#include "common/mimetypes.h"
#include "common/shortcuts.h"
#include "common/textdata.h"
#include "gui/clipboardbrowser.h"
#include "gui/screen.h"
#include "gui/windowgeometryguard.h"
#include "item/credentialdetector.h"
#include "item/flycutsyncformat.h"
#include "item/itemlabel.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSet>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

namespace {

using FlycutSyncFormat::mimeApp;
using FlycutSyncFormat::mimeClipId;
using FlycutSyncFormat::mimeCopiedAt;
using FlycutSyncFormat::mimeMachine;
using FlycutSyncFormat::mimeRank;

// Only the beginning of a long text is searched and previewed.
constexpr int maxSearchedCharacters = 100'000;
constexpr int maxPreviewCharacters = 100'000;

enum class TextRole { Normal, Credential, Label };

bool hasText(const QVariantMap &data)
{
    return data.contains(mimeText) || data.contains(mimeTextUtf8) || data.contains(mimeUriList);
}

bool isHiddenItem(const QVariantMap &data)
{
    return data.contains(mimeHidden) || data.contains(mimeSecret);
}

QString itemTitle(const QVariantMap &data)
{
    if ( isHiddenItem(data) || !hasText(data) ) {
        const QString label = isHiddenItem(data) ? QString() : itemLabel(data);
        return label.isEmpty() ? textLabelForData(data) : label;
    }

    const QString text = getTextData(data).left(maxSearchedCharacters);
    for (const auto &line : QStringView(text).split(u'\n')) {
        const QString title = line.toString().simplified();
        if ( !title.isEmpty() )
            return title;
    }
    return {};
}

QString relativeTime(qint64 msecsSinceEpoch)
{
    const qint64 seconds = (QDateTime::currentMSecsSinceEpoch() - msecsSinceEpoch) / 1000;
    if (seconds < 60)
        return SearchPopup::tr("just now");
    if (seconds < 3600)
        return SearchPopup::tr("%1 min ago").arg(seconds / 60);
    if (seconds < 24 * 3600)
        return SearchPopup::tr("%1 h ago").arg(seconds / 3600);
    if (seconds < 7 * 24 * 3600)
        return SearchPopup::tr("%1 d ago").arg(seconds / (24 * 3600));
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(msecsSinceEpoch).date(), QLocale::ShortFormat);
}

QString itemSubtitle(const QVariantMap &data, bool onServer)
{
    QStringList parts;
    if (onServer)
        parts.append(SearchPopup::tr("on server"));

    const QString app = getTextData(data, mimeApp);
    const QString machine = getTextData(data, mimeMachine);
    if ( !app.isEmpty() )
        parts.append(app);
    else if ( !machine.isEmpty() )
        parts.append(machine);
    else if ( const QString window = getTextData(data, mimeWindowTitle).simplified(); !window.isEmpty() )
        parts.append(window);

    bool ok = false;
    qint64 copiedAt = data.value(mimeCopiedAt).toByteArray().toLongLong(&ok);
    if (!ok) {
        const double rank = data.value(mimeRank).toByteArray().toDouble(&ok);
        copiedAt = static_cast<qint64>(rank * 1000);
    }
    if (ok && copiedAt > 0)
        parts.append( relativeTime(copiedAt) );

    return parts.join(QStringLiteral(" · "));
}

TextRole textRoleFor(const QVariantMap &data)
{
    if ( CredentialDetector::instance().isCredentialItem(data) )
        return TextRole::Credential;
    if ( textLabelDescribesData(data) )
        return TextRole::Label;
    return TextRole::Normal;
}

QString searchedText(const QModelIndex &index)
{
    QString text;
    if ( !index.data(contentType::isHidden).toBool() ) {
        text = index.data(contentType::text).toString().left(maxSearchedCharacters);
        if ( text.isEmpty() )
            text = itemLabel( index.data(contentType::data).toMap() );
    }
    const QString notes = index.data(contentType::notes).toString();
    return foldedForSearch( notes.isEmpty() ? text : notes + '\n' + text );
}

bool matchesAll(const QString &folded, const QStringList &words)
{
    return std::all_of(words.begin(), words.end(), [&](const QString &word){ return folded.contains(word); });
}

} // namespace

class SearchPopupModel final : public QAbstractListModel
{
public:
    enum class Kind { Local, ArchiveSearch, ArchiveHeader, Remote };
    enum class ArchiveState { Idle, Searching, Done };

    struct Row {
        Row() = default;
        explicit Row(Kind kind, const QPersistentModelIndex &index = {}, const QVariantMap &data = {})
            : kind(kind), index(index), data(data) {}

        Kind kind = Kind::Local;
        QPersistentModelIndex index;
        QVariantMap data;
        mutable QString title;
        mutable QString subtitle;
        mutable TextRole textRole = TextRole::Normal;
        mutable bool described = false;
    };

    explicit SearchPopupModel(QObject *parent)
        : QAbstractListModel(parent)
    {
    }

    void setSource(QAbstractItemModel *source)
    {
        if (m_source)
            disconnect(m_source, nullptr, this, nullptr);
        m_source = source;
        m_folded.clear();
        if (m_source) {
            const auto changed = [this]{ m_folded.clear(); refilter(); };
            connect(m_source, &QAbstractItemModel::rowsInserted, this, changed);
            connect(m_source, &QAbstractItemModel::rowsRemoved, this, changed);
            connect(m_source, &QAbstractItemModel::rowsMoved, this, changed);
            connect(m_source, &QAbstractItemModel::dataChanged, this, changed);
            connect(m_source, &QAbstractItemModel::modelReset, this, changed);
            connect(m_source, &QObject::destroyed, this, [this]{ m_source = nullptr; m_folded.clear(); refilter(); });
        }
    }

    void setFilter(const QStringList &words, bool canSearchArchive)
    {
        m_words = words;
        m_canSearchArchive = canSearchArchive;
        m_archiveState = ArchiveState::Idle;
        m_archiveResult = {};
        m_remoteItems.clear();
        refilter();
    }

    void setArchiveSearching()
    {
        m_archiveState = ArchiveState::Searching;
        m_archiveResult = {};
        m_remoteItems.clear();
        refilter();
    }

    void setArchiveResult(const SearchPopup::ArchiveResult &result)
    {
        m_archiveState = ArchiveState::Done;
        m_archiveResult = result;
        m_remoteItems.clear();

        // Items held locally are already listed above (or filtered out by the same words).
        QSet<QByteArray> localIds;
        QSet<QString> localTexts;
        for (int row = 0; m_source && row < m_source->rowCount(); ++row) {
            const QVariantMap data = m_source->index(row, 0).data(contentType::data).toMap();
            const QByteArray id = data.value(mimeClipId).toByteArray();
            if ( !id.isEmpty() )
                localIds.insert(id);
            localTexts.insert( hasText(data) ? getTextData(data) : itemLabel(data) );
        }
        for (const auto &data : result.items) {
            const QByteArray id = data.value(mimeClipId).toByteArray();
            const QString text = hasText(data) ? getTextData(data) : itemLabel(data);
            if ( (id.isEmpty() || !localIds.contains(id)) && !localTexts.contains(text) )
                m_remoteItems.append(data);
        }
        refilter();
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if ( !index.isValid() || role != Qt::DisplayRole )
            return {};
        return row(index.row()).title;
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if ( !index.isValid() || row(index.row()).kind == Kind::ArchiveHeader )
            return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    /// Row with title, subtitle and text role filled in.
    const Row &row(int i) const
    {
        const Row &r = m_rows[i];
        if (r.described)
            return r;
        r.described = true;
        switch (r.kind) {
        case Kind::Local: {
            const QVariantMap data = r.index.data(contentType::data).toMap();
            r.title = itemTitle(data);
            r.subtitle = itemSubtitle(data, false);
            r.textRole = textRoleFor(data);
            break;
        }
        case Kind::Remote:
            r.title = itemTitle(r.data);
            r.subtitle = itemSubtitle(r.data, true);
            r.textRole = textRoleFor(r.data);
            break;
        case Kind::ArchiveSearch:
            r.title = SearchPopup::tr("Search full archive on server…");
            r.subtitle = archiveStatus();
            break;
        case Kind::ArchiveHeader:
            r.title = SearchPopup::tr("On server");
            break;
        }
        return r;
    }

    /// Item data of the row, or empty for rows that aren't items.
    QVariantMap itemData(int i) const
    {
        const Row &r = m_rows[i];
        if (r.kind == Kind::Local)
            return r.index.data(contentType::data).toMap();
        return r.data;
    }

    QString archiveStatus() const
    {
        if (m_archiveState == ArchiveState::Searching)
            return SearchPopup::tr("Searching the server…");
        if (m_archiveState == ArchiveState::Idle)
            return {};
        if ( !m_archiveResult.error.isEmpty() )
            return m_archiveResult.error;
        return SearchPopup::tr("%1 found on server").arg(m_archiveResult.matched);
    }

    int localCount() const { return m_localCount; }
    int sourceCount() const { return m_source ? m_source->rowCount() : 0; }
    int firstRemoteRow() const { return m_remoteItems.isEmpty() ? -1 : m_localCount + 2; }

private:
    void refilter()
    {
        beginResetModel();
        m_rows.clear();
        m_localCount = 0;

        if (m_source) {
            const int count = m_source->rowCount();
            if (m_folded.size() != count) {
                m_folded.clear();
                m_folded.reserve(count);
                for (int row = 0; row < count; ++row)
                    m_folded.append( searchedText(m_source->index(row, 0)) );
            }
            for (int row = 0; row < count; ++row) {
                if ( matchesAll(m_folded[row], m_words) )
                    m_rows.append( Row(Kind::Local, m_source->index(row, 0)) );
            }
            m_localCount = static_cast<int>(m_rows.size());
        }

        if (m_canSearchArchive) {
            m_rows.append( Row(Kind::ArchiveSearch) );
            if ( !m_remoteItems.isEmpty() ) {
                m_rows.append( Row(Kind::ArchiveHeader) );
                for (const auto &data : m_remoteItems)
                    m_rows.append( Row(Kind::Remote, {}, data) );
            }
        }

        endResetModel();
    }

    QAbstractItemModel *m_source = nullptr;
    QVector<QString> m_folded;
    QStringList m_words;
    bool m_canSearchArchive = false;
    ArchiveState m_archiveState = ArchiveState::Idle;
    SearchPopup::ArchiveResult m_archiveResult;
    QVector<QVariantMap> m_remoteItems;
    QVector<Row> m_rows;
    int m_localCount = 0;
};

namespace {

class SearchPopupDelegate final : public QStyledItemDelegate
{
public:
    SearchPopupDelegate(const SearchPopup::Style *style, QObject *parent)
        : QStyledItemDelegate(parent)
        , m_style(style)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto model = static_cast<const SearchPopupModel*>(index.model());
        const int lineHeight = option.fontMetrics.height();
        const int lines = model->row(index.row()).kind == SearchPopupModel::Kind::ArchiveHeader ? 1 : 2;
        return QSize(option.rect.width(), lines * lineHeight + padding(option));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto model = static_cast<const SearchPopupModel*>(index.model());
        const auto &row = model->row(index.row());
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const int pad = padding(option);
        const int halfPad = pad / 2;
        const QRect rect = option.rect.adjusted(pad, halfPad, -pad, -halfPad);
        const QFontMetrics &metrics = option.fontMetrics;
        const int lineHeight = metrics.height();

        painter->save();
        painter->fillRect(option.rect, selected ? m_style->selectedBackground : m_style->background);

        if (row.kind == SearchPopupModel::Kind::ArchiveHeader) {
            painter->setPen(m_style->muted);
            const int y = option.rect.center().y();
            const QString title = row.title + ' ';
            painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, title);
            painter->drawLine(rect.left() + metrics.horizontalAdvance(title), y, rect.right(), y);
            painter->restore();
            return;
        }

        QColor titleColor = m_style->text;
        if (row.textRole == TextRole::Credential)
            titleColor = m_style->credential;
        else if (row.textRole == TextRole::Label)
            titleColor = m_style->label;
        if (selected)
            titleColor = m_style->selectedText;

        QFont titleFont = option.font;
        if (row.kind == SearchPopupModel::Kind::ArchiveSearch)
            titleFont.setItalic(true);
        painter->setFont(titleFont);
        painter->setPen(titleColor);
        const QRect titleRect(rect.left(), rect.top(), rect.width(), lineHeight);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(titleFont).elidedText(row.title, Qt::ElideRight, rect.width()));

        painter->setFont(option.font);
        QColor subtitleColor = selected ? m_style->selectedText : m_style->muted;
        if (selected)
            subtitleColor.setAlphaF(0.7f);
        painter->setPen(subtitleColor);
        const QRect subtitleRect(rect.left(), rect.top() + lineHeight, rect.width(), lineHeight);
        painter->drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(row.subtitle, Qt::ElideRight, rect.width()));

        painter->restore();
    }

private:
    static int padding(const QStyleOptionViewItem &option)
    {
        return option.fontMetrics.height() / 2;
    }

    const SearchPopup::Style *m_style;
};

QString imageFormat(const QVariantMap &data)
{
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if ( it.key().startsWith(QLatin1String("image/")) )
            return it.key();
    }
    return {};
}

} // namespace

SearchPopup::SearchPopup(QWidget *parent)
    : QWidget(parent, Qt::Popup)
    , m_filter(new QLineEdit(this))
    , m_list(new QListView(this))
    , m_preview(new QStackedWidget(this))
    , m_previewText(new QPlainTextEdit(m_preview))
    , m_previewImage(new QLabel(m_preview))
    , m_status(new QLabel(this))
    , m_model(new SearchPopupModel(this))
{
    setObjectName(QStringLiteral("SearchPopup"));
    m_filter->setObjectName(QStringLiteral("searchPopupFilter"));
    m_list->setObjectName(QStringLiteral("searchPopupList"));

    // Same workaround as for the tray menu: on Wayland, Qt 6.9+ rejects a popup
    // without a focused transient parent, so use a frameless toplevel that closes
    // when it loses focus.
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    if (QGuiApplication::platformName() == QLatin1String("wayland")) {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    }
#endif

    m_list->setModel(m_model);
    m_list->setItemDelegate(new SearchPopupDelegate(&m_style, this));
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_previewText->setReadOnly(true);
    m_previewText->setFocusPolicy(Qt::NoFocus);
    m_previewText->setFrameShape(QFrame::NoFrame);
    m_previewImage->setAlignment(Qt::AlignCenter);
    m_preview->addWidget(m_previewText);
    m_preview->addWidget(m_previewImage);

    auto content = new QHBoxLayout;
    content->addWidget(m_list, 3);
    content->addWidget(m_preview, 2);

    auto layout = new QVBoxLayout(this);
    const int margin = fontMetrics().height() / 3;
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(margin);
    layout->addWidget(m_filter);
    layout->addLayout(content, 1);
    layout->addWidget(m_status);

    m_filter->installEventFilter(this);
    setFocusProxy(m_filter);

    connect(m_filter, &QLineEdit::textChanged, this, &SearchPopup::updateFilter);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &SearchPopup::updatePreview);
    connect(m_list, &QListView::activated, this, [this](const QModelIndex &index){ activate(index, true); });
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex &index){
        m_list->setCurrentIndex(index);
        m_filter->setFocus();
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]{
        if ( !m_list->currentIndex().isValid() )
            setCurrentRow(0);
        updateStatus();
    });
}

SearchPopup::~SearchPopup() = default;

void SearchPopup::setStyle(const Style &style)
{
    m_style = style;

    setFont(style.font);
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, style.background);
    palette.setColor(QPalette::Base, style.background);
    palette.setColor(QPalette::AlternateBase, style.background);
    palette.setColor(QPalette::Text, style.text);
    palette.setColor(QPalette::WindowText, style.text);
    palette.setColor(QPalette::Highlight, style.selectedBackground);
    palette.setColor(QPalette::HighlightedText, style.selectedText);
    palette.setColor(QPalette::PlaceholderText, style.muted);
    setPalette(palette);
    setAutoFillBackground(true);

    QPalette statusPalette = palette;
    statusPalette.setColor(QPalette::WindowText, style.muted);
    m_status->setPalette(statusPalette);

    m_list->viewport()->update();
}

void SearchPopup::setTabs(const QString &clipboardTab, const QString &favoritesTab)
{
    m_tabs = QStringList{clipboardTab, favoritesTab};
}

QString SearchPopup::tabLabel(int tabIndex) const
{
    QString name = m_tabs.value(tabIndex);
    return removeKeyHint(&name);
}

bool SearchPopup::toggle(QPoint pos)
{
    if ( isVisible() ) {
        close();
        return false;
    }

    m_tabIndex = -1;
    m_filter->clear();
    showTab(0);

    const QRect screen = screenAvailableGeometry(*this);
    const QFontMetrics metrics(m_style.font);
    QSize size(metrics.averageCharWidth() * 110, metrics.height() * 28);
    if ( screen.isValid() )
        size = size.boundedTo(screen.size() * 0.9);
    resize(size);
    move( toScreen(pos, this) );

    show();
    raiseWindow(this);
    activateWindow();
    m_filter->setFocus();
    return true;
}

bool SearchPopup::eventFilter(QObject *watched, QEvent *event)
{
    if ( watched == m_filter ) {
        if ( event->type() == QEvent::KeyPress && handleKey(static_cast<QKeyEvent*>(event)) )
            return true;

        // Keep keys the popup handles from triggering application shortcuts.
        if ( event->type() == QEvent::ShortcutOverride ) {
            const auto key = static_cast<QKeyEvent*>(event)->key();
            if ( key == Qt::Key_Escape || key == Qt::Key_Backspace || key == Qt::Key_Return
                 || key == Qt::Key_Enter || key == Qt::Key_Tab || key == Qt::Key_Backtab )
            {
                event->accept();
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

bool SearchPopup::event(QEvent *event)
{
    if ( event->type() == QEvent::WindowDeactivate && !windowFlags().testFlag(Qt::Popup) )
        close();
    return QWidget::event(event);
}

// Keys sent to the popup itself (it's the active popup widget) go to the search field;
// those the field ignores come back here through the parent and stop.
void SearchPopup::keyPressEvent(QKeyEvent *event)
{
    if (m_forwardingKey) {
        QWidget::keyPressEvent(event);
        return;
    }
    m_forwardingKey = true;
    QCoreApplication::sendEvent(m_filter, event);
    m_forwardingKey = false;
}

bool SearchPopup::handleKey(QKeyEvent *event)
{
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    switch ( event->key() ) {
    case Qt::Key_Escape:
        if ( m_filter->text().isEmpty() )
            close();
        else
            m_filter->clear();
        return true;

    case Qt::Key_Return:
    case Qt::Key_Enter:
        activate(m_list->currentIndex(), !control);
        return true;

    case Qt::Key_Backspace:
        if (!control)
            return false;
        removeCurrent();
        return true;

    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        showTab(m_tabIndex == 0 ? 1 : 0);
        return true;

    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        QCoreApplication::sendEvent(m_list, event);
        return true;

    default:
        return false;
    }
}

void SearchPopup::showTab(int tabIndex)
{
    if (tabIndex == m_tabIndex)
        return;

    const QString tabName = m_tabs.value(tabIndex);
    ClipboardBrowser *browser = tabName.isEmpty() || !m_browserForTab ? nullptr : m_browserForTab(tabName);
    // Stay in the current tab if the other one doesn't exist.
    if (!browser && m_tabIndex != -1)
        return;

    m_tabIndex = tabIndex;
    m_browser = browser;
    m_model->setSource(browser ? browser->model() : nullptr);

    // Reloading configuration or tabs replaces browsers; follow the tab to its new one.
    if (browser) {
        connect(browser, &QObject::destroyed, this, [this]{
            QTimer::singleShot(0, this, [this]{
                if ( !m_browser && isVisible() )
                    reloadTab();
            });
        });
    }

    const QString otherTab = tabLabel(tabIndex == 0 ? 1 : 0);
    m_filter->setPlaceholderText(
        otherTab.isEmpty()
        ? tr("Search %1").arg(tabLabel(tabIndex))
        : tr("Search %1 (Tab: %2)").arg(tabLabel(tabIndex), otherTab) );

    updateFilter();
}

void SearchPopup::reloadTab()
{
    const int tabIndex = m_tabIndex;
    m_tabIndex = -1;
    showTab(tabIndex);
}

void SearchPopup::updateFilter()
{
    ++m_archiveRequest;
    const QStringList words = foldedForSearch(m_filter->text()).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    m_model->setFilter(words, m_archiveSearch && !words.isEmpty());
    setCurrentRow(0);
    updatePreview();
}

void SearchPopup::setCurrentRow(int row)
{
    const int count = m_model->rowCount();
    if (count == 0)
        return;
    const QModelIndex index = m_model->index(qBound(0, row, count - 1), 0);
    m_list->setCurrentIndex(index);
    m_list->scrollTo(index);
}

void SearchPopup::updateStatus()
{
    QString summary = tr("%1 of %2 items in %3")
        .arg(m_model->localCount()).arg(m_model->sourceCount()).arg(tabLabel(m_tabIndex));
    const QString archiveStatus = m_model->archiveStatus();
    if ( !archiveStatus.isEmpty() )
        summary.append(QStringLiteral(", ") + archiveStatus);

    // The title isn't shown for a popup, but window lists and screen readers use it.
    setWindowTitle(summary);
    m_status->setText(
        summary + QStringLiteral("  ·  ") + tr("Enter: paste, Ctrl+Enter: copy, Ctrl+Backspace: remove") );
}

void SearchPopup::updatePreview()
{
    const QModelIndex index = m_list->currentIndex();
    const QVariantMap data = index.isValid() ? m_model->itemData(index.row()) : QVariantMap();

    const QString image = isHiddenItem(data) ? QString() : imageFormat(data);
    if ( !image.isEmpty() && !hasText(data) ) {
        QPixmap pixmap;
        if ( pixmap.loadFromData(data.value(image).toByteArray(), image.mid(6).toLatin1()) ) {
            const QSize maxSize = m_preview->size();
            if (pixmap.width() > maxSize.width() || pixmap.height() > maxSize.height())
                pixmap = pixmap.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_previewImage->setPixmap(pixmap);
            m_preview->setCurrentWidget(m_previewImage);
            return;
        }
    }

    QString text;
    if ( data.isEmpty() )
        text.clear();
    else if ( isHiddenItem(data) || !hasText(data) )
        text = itemTitle(data);
    else
        text = getTextData(data).left(maxPreviewCharacters);
    m_previewText->setPlainText(text);
    m_preview->setCurrentWidget(m_previewText);
}

void SearchPopup::activate(const QModelIndex &index, bool paste)
{
    if ( !index.isValid() )
        return;

    const auto kind = m_model->row(index.row()).kind;
    if (kind == SearchPopupModel::Kind::ArchiveSearch) {
        searchArchive();
        return;
    }
    if (kind == SearchPopupModel::Kind::ArchiveHeader)
        return;

    const QVariantMap data = m_model->itemData(index.row());
    const bool fromArchive = kind == SearchPopupModel::Kind::Remote;
    const QString tabName = m_tabs.value(fromArchive ? 0 : m_tabIndex);
    close();
    emit itemActivated(data, tabName, paste, fromArchive);
}

void SearchPopup::removeCurrent()
{
    const QModelIndex index = m_list->currentIndex();
    if ( !index.isValid() || !m_browser )
        return;

    const auto &row = m_model->row(index.row());
    if (row.kind != SearchPopupModel::Kind::Local || !row.index.isValid())
        return;

    const int currentRow = index.row();
    QString error;
    m_browser->removeIndexes({QModelIndex(row.index)}, &error);
    if ( !error.isEmpty() ) {
        m_status->setText(error);
        return;
    }
    setCurrentRow(currentRow);
}

void SearchPopup::searchArchive()
{
    if (!m_archiveSearch)
        return;

    const int request = ++m_archiveRequest;
    m_model->setArchiveSearching();
    setCurrentRow(m_model->localCount());

    const QString query = m_filter->text().simplified();
    m_archiveSearch(query, m_tabIndex, this, [this, request](const ArchiveResult &result){
        if (request != m_archiveRequest)
            return;
        m_model->setArchiveResult(result);
        const int firstRemoteRow = m_model->firstRemoteRow();
        setCurrentRow(firstRemoteRow == -1 ? m_model->localCount() : firstRemoteRow);
        updatePreview();
    });
}
