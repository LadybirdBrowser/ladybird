/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/GenericShorthands.h>
#include <AK/kmalloc.h>
#include <LibWebView/Autocomplete.h>
#include <UI/Qt/Autocomplete.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Theme.h>

#include <QAbstractListModel>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QIcon>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QStyle>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>

namespace Ladybird {

// The suggestions are listed in the address field's surface, below the field, with this much room around them. The
// surface also draws the line between the field and the list, which is this tall.
static constexpr int LIST_PADDING = 6;
static constexpr int FIELD_DIVIDER_HEIGHT = 1;
static constexpr int ROW_HEIGHT = 34;
static constexpr int ROW_CORNER_RADIUS = 6;
static constexpr int CELL_HORIZONTAL_PADDING = 12;
static constexpr int CELL_ICON_SIZE = 16;
static constexpr int CELL_SPACING = 12;
// Search suggestions are set apart from pages by a line, with this much room above and below it.
static constexpr int GROUP_SEPARATOR_MARGIN = 5;
static constexpr int GROUP_SEPARATOR_INSET = 8;
static constexpr int GROUP_SEPARATOR_HEIGHT = (2 * GROUP_SEPARATOR_MARGIN) + 1;

static constexpr size_t MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS = 8;

enum AutocompleteRole {
    TitleRole = Qt::UserRole + 1,
    SubtitleRole,
    UrlRole,
    FaviconRole,
    SourceRole,
    SuggestionIndexRole,
    HighlightInputRole,
    StartsGroupRole,
};

static QFont autocomplete_title_font()
{
    QFont font = QApplication::font();
    font.setPixelSize(13);
    font.setWeight(QFont::Normal);
    return font;
}

static QFont autocomplete_detail_font()
{
    QFont font = QApplication::font();
    font.setPixelSize(12);
    return font;
}

static void draw_autocomplete_text(QPainter& painter, QRect const& rect, QString const& text, QString const& highlight_input, QFont font, QColor color)
{
    QFontMetrics font_metrics(font);
    auto input = ak_string_from_qstring(highlight_input);
    // NB: Most of a matching suggestion is drawn in demibold. Elide at the heavier weight, or the end of the text and
    //     its ellipsis would not fit in the rect.
    auto elision_font = font;
    if (!WebView::autocomplete_match_ranges(input, ak_string_from_qstring(text)).is_empty())
        elision_font.setWeight(QFont::DemiBold);
    auto elided_text = QFontMetrics(elision_font).elidedText(text, Qt::ElideRight, rect.width());
    auto displayed_text = ak_string_from_qstring(elided_text);

    QList<QTextLayout::FormatRange> formats;
    // The text you typed is drawn at the regular weight, and the rest of the suggestion stands out in demibold.
    auto match_ranges = WebView::autocomplete_match_ranges(input, displayed_text);
    if (!match_ranges.is_empty()) {
        QTextCharFormat format;
        format.setFontWeight(QFont::DemiBold);
        formats.append({ 0, static_cast<int>(elided_text.length()), move(format) });
    }
    for (auto const& range : match_ranges) {
        auto bytes = displayed_text.bytes_as_string_view();
        auto prefix = qstring_from_ak_string(MUST(String::from_utf8(bytes.substring_view(0, range.start))));
        auto match = qstring_from_ak_string(MUST(String::from_utf8(bytes.substring_view(range.start, range.length))));
        QTextCharFormat format;
        format.setFontWeight(QFont::Normal);
        formats.append({ static_cast<int>(prefix.length()), static_cast<int>(match.length()), move(format) });
    }

    QTextLayout layout(elided_text, font);
    layout.setFormats(formats);
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(rect.width());
    layout.endLayout();
    painter.setPen(color);
    layout.draw(&painter, QPointF(rect.left(), rect.top() + (rect.height() - font_metrics.height()) / 2), {}, rect);
}

class AutocompleteModel final : public QAbstractListModel {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit AutocompleteModel(QObject* parent)
        : QAbstractListModel(parent)
    {
    }

    void set_suggestions(Vector<WebView::AutocompleteSuggestion> suggestions)
    {
        beginResetModel();
        m_suggestions = move(suggestions);
        m_favicon_cache.clear();

        for (size_t index = 0; index < m_suggestions.size(); ++index) {
            auto const& suggestion = m_suggestions[index];
            if (!suggestion.favicon_png.has_value())
                continue;
            auto icon = icon_from_png(suggestion.favicon_png->bytes(), CELL_ICON_SIZE);
            if (icon.isNull())
                continue;
            m_favicon_cache.append({ index, move(icon) });
        }

        endResetModel();
    }

    int rowCount(QModelIndex const& parent = {}) const override
    {
        if (parent.isValid())
            return 0;
        return static_cast<int>(m_suggestions.size());
    }

    QVariant data(QModelIndex const& index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_suggestions.size()))
            return {};

        auto suggestion_index = static_cast<size_t>(index.row());
        auto const& suggestion = m_suggestions[suggestion_index];

        switch (role) {
        case Qt::DisplayRole:
        case UrlRole:
            return qstring_from_ak_string(WebView::autocomplete_suggestion_display_text(suggestion));
        case TitleRole:
            if (suggestion.title.has_value())
                return qstring_from_ak_string(*suggestion.title);
            return {};
        case SubtitleRole:
            if (suggestion.subtitle.has_value())
                return qstring_from_ak_string(*suggestion.subtitle);
            return {};
        case FaviconRole:
            for (auto const& entry : m_favicon_cache) {
                if (entry.suggestion_index == suggestion_index)
                    return entry.icon;
            }
            return {};
        case SourceRole:
            return static_cast<int>(suggestion.source);
        case SuggestionIndexRole:
            return index.row();
        case HighlightInputRole:
            return qstring_from_ak_string(suggestion.highlight_input);
        case StartsGroupRole: {
            // Pages that follow search suggestions start a group of their own.
            if (suggestion_index == 0)
                return false;
            auto is_search = [](auto const& suggestion) { return suggestion.source == WebView::AutocompleteSuggestionSource::Search; };
            return is_search(m_suggestions[suggestion_index - 1]) != is_search(suggestion);
        }
        default:
            return {};
        }
    }

    Qt::ItemFlags flags(QModelIndex const& index) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_suggestions.size()))
            return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    int table_row_for_suggestion_index(int suggestion_index) const
    {
        if (suggestion_index < 0 || suggestion_index >= static_cast<int>(m_suggestions.size()))
            return -1;
        return suggestion_index;
    }

    size_t visible_suggestion_count() const
    {
        return m_suggestions.size();
    }

private:
    struct FaviconEntry {
        size_t suggestion_index;
        QIcon icon;
    };

    Vector<WebView::AutocompleteSuggestion> m_suggestions;
    Vector<FaviconEntry> m_favicon_cache;
};

class AutocompleteDelegate final : public QStyledItemDelegate {
public:
    AK_ALLOC_WITH_KMALLOC;

    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const& index) const override
    {
        if (!index.isValid())
            return {};
        auto height = ROW_HEIGHT;
        if (index.data(StartsGroupRole).toBool())
            height += GROUP_SEPARATOR_HEIGHT;
        return QSize(0, height);
    }

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        auto const& panel = Theme::panel();

        auto row_rect = option.rect;
        if (index.data(StartsGroupRole).toBool()) {
            auto separator_y = row_rect.top() + GROUP_SEPARATOR_MARGIN;
            painter->fillRect(QRect(row_rect.left() + GROUP_SEPARATOR_INSET, separator_y, row_rect.width() - (2 * GROUP_SEPARATOR_INSET), 1), panel.separator);
            row_rect.setTop(row_rect.top() + GROUP_SEPARATOR_HEIGHT);
        }

        bool selected = option.state & QStyle::State_Selected;
        bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(selected ? Theme::selected_row() : panel.control);
            painter->drawRoundedRect(QRectF(row_rect), ROW_CORNER_RADIUS, ROW_CORNER_RADIUS);
        }

        auto text_color = selected ? QColor(Qt::white) : panel.text;
        auto detail_color = selected ? QColor(255, 255, 255, 224) : panel.muted;

        auto favicon = index.data(FaviconRole).value<QIcon>();
        auto source = static_cast<WebView::AutocompleteSuggestionSource>(index.data(SourceRole).toInt());

        auto url_text = index.data(UrlRole).toString();
        auto title_text = index.data(TitleRole).toString();
        auto subtitle_text = index.data(SubtitleRole).toString();
        auto highlight_input = index.data(HighlightInputRole).toString();

        // A row shows what it is in full, then a muted detail: where a search goes, or the address of a page.
        auto primary_text = title_text.isEmpty() ? url_text : title_text;
        QString detail_text;
        if (source == WebView::AutocompleteSuggestionSource::Search || source == WebView::AutocompleteSuggestionSource::ChooseSearchEngine)
            detail_text = subtitle_text;
        else if (source == WebView::AutocompleteSuggestionSource::Bookmark)
            detail_text = title_text.isEmpty() ? QStringLiteral("Bookmark") : QStringLiteral("Bookmark · %1").arg(url_text);
        else if (!title_text.isEmpty())
            detail_text = url_text;

        int x = row_rect.left() + CELL_HORIZONTAL_PADDING;
        QRect icon_rect(x, row_rect.top() + (row_rect.height() - CELL_ICON_SIZE) / 2, CELL_ICON_SIZE, CELL_ICON_SIZE);
        auto icon_color = selected ? QColor(Qt::white) : panel.muted;
        if (source == WebView::AutocompleteSuggestionSource::Search || source == WebView::AutocompleteSuggestionSource::ChooseSearchEngine)
            create_chrome_icon(ChromeIcon::Search, icon_color).paint(painter, icon_rect);
        else if (!favicon.isNull())
            favicon.paint(painter, icon_rect);
        else
            create_chrome_icon(ChromeIcon::Globe, icon_color).paint(painter, icon_rect);
        x = icon_rect.right() + 1 + CELL_SPACING;

        auto right = row_rect.right() - CELL_HORIZONTAL_PADDING;

        auto title_font = autocomplete_title_font();
        auto detail_font = autocomplete_detail_font();
        auto available = max(0, right - x + 1);
        // NB: Measure the title at the heavier weight that most of a matching title is drawn in.
        auto measuring_font = title_font;
        measuring_font.setWeight(QFont::DemiBold);
        auto title_width = min(available, QFontMetrics(measuring_font).horizontalAdvance(primary_text) + 2);
        // NB: The offer to choose a search engine does not show the query, so nothing in it is highlighted.
        auto title_highlight = source == WebView::AutocompleteSuggestionSource::ChooseSearchEngine ? QString {} : highlight_input;
        draw_autocomplete_text(*painter, QRect(x, row_rect.top(), title_width, row_rect.height()), primary_text, title_highlight, title_font, text_color);

        if (!detail_text.isEmpty()) {
            auto detail_x = x + title_width + CELL_SPACING;
            auto detail_width = right - detail_x + 1;
            if (detail_width > 24) {
                painter->setFont(detail_font);
                painter->setPen(detail_color);
                auto elided = QFontMetrics(detail_font).elidedText(detail_text, Qt::ElideRight, detail_width);
                painter->drawText(QRect(detail_x, row_rect.top(), detail_width, row_rect.height()), Qt::AlignVCenter | Qt::AlignLeft, elided);
            }
        }

        painter->restore();
    }
};

Autocomplete::Autocomplete(QLineEdit* anchor)
    : QObject(anchor)
    , m_anchor(anchor)
{
    m_model = new AutocompleteModel(this);
    m_delegate = new AutocompleteDelegate(this);
    qApp->installEventFilter(this);
}

QListView* Autocomplete::create_list_view(QWidget& parent)
{
    auto* list_view = new QListView(&parent);
    list_view->setObjectName("LadybirdAutocompleteList");
    list_view->setFocusPolicy(Qt::NoFocus);
    list_view->setSelectionMode(QAbstractItemView::SingleSelection);
    list_view->setMouseTracking(true);
    list_view->setFrameShape(QFrame::NoFrame);
    list_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list_view->setModel(m_model);
    list_view->setItemDelegate(m_delegate);
    list_view->installEventFilter(this);

    connect(list_view, &QAbstractItemView::clicked, this, [this](QModelIndex const& index) {
        if (!is_selectable_row(index.row()))
            return;
        emit suggestion_clicked(index.data(SuggestionIndexRole).toInt());
    });
    return list_view;
}

void Autocomplete::set_host(QWidget* host)
{
    if (m_host == host)
        return;

    auto was_visible = close();
    delete m_list_view.data();

    m_host = host;
    if (host) {
        m_list_view = create_list_view(*host);
        m_list_view->hide();
        host->layout()->addWidget(m_list_view);
        update_chrome_style();
    }
    if (was_visible)
        show_current_suggestions();
}

Autocomplete::~Autocomplete()
{
    qApp->removeEventFilter(this);
    delete m_list_view.data();
}

void Autocomplete::update_chrome_style()
{
    if (!m_list_view || m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    auto palette = QApplication::palette();
    m_list_view->setPalette(palette);
    m_list_view->setStyleSheet(ChromeStyle::autocomplete_popup_style_sheet(palette));
    m_list_view->viewport()->update();
    m_is_updating_chrome_style = false;
}

void Autocomplete::schedule_chrome_style_update()
{
    if (m_has_pending_chrome_style_update)
        return;

    m_has_pending_chrome_style_update = true;
    QTimer::singleShot(0, this, [this] {
        update_chrome_style();
        m_has_pending_chrome_style_update = false;
    });
}

void Autocomplete::show_with_suggestions(Vector<WebView::AutocompleteSuggestion> suggestions, Optional<size_t> selected_suggestion_index)
{
    m_model->set_suggestions(move(suggestions));
    show_current_suggestions();
    set_selected_suggestion(selected_suggestion_index);
}

void Autocomplete::show_current_suggestions()
{
    // NB: Only a field with a host, which is the field that is shown, lists suggestions.
    if (m_model->rowCount() == 0 || !m_list_view) {
        close();
        return;
    }

    update_chrome_style();
    // NB: The host may be hidden until it sees the list is open, so the list's own visibility isn't watched.
    m_list_view->setFixedHeight(list_height());
    m_list_view->show();
    set_anchor_popup_open(true);
}

void Autocomplete::set_selected_suggestion(Optional<size_t> suggestion_index)
{
    if (!m_list_view)
        return;

    int table_row = suggestion_index.has_value()
        ? m_model->table_row_for_suggestion_index(static_cast<int>(*suggestion_index))
        : -1;

    if (table_row == -1) {
        m_list_view->setCurrentIndex({});
        return;
    }

    auto index = m_model->index(table_row, 0);
    m_list_view->setCurrentIndex(index);
    m_list_view->scrollTo(index);
}

bool Autocomplete::close()
{
    if (!is_visible())
        return false;
    m_list_view->hide();
    set_anchor_popup_open(false);
    return true;
}

void Autocomplete::set_anchor_popup_open(bool open)
{
    if (m_anchor->property("autocompleteOpen").toBool() == open)
        return;
    m_anchor->setProperty("autocompleteOpen", open);
    m_anchor->style()->unpolish(m_anchor);
    m_anchor->style()->polish(m_anchor);
    m_anchor->update();
}

bool Autocomplete::is_visible() const
{
    return m_list_view && !m_list_view->isHidden();
}

bool Autocomplete::eventFilter(QObject* watched, QEvent* event)
{
    auto type = event->type();
    if ((watched == m_anchor->window() && first_is_one_of(type, QEvent::Hide, QEvent::Resize)) || (watched == qApp && type == QEvent::Quit)) {
        if (close())
            emit dismissed();
    }

    if (type == QEvent::ApplicationPaletteChange || type == QEvent::ThemeChange
        || (type == QEvent::PaletteChange && (watched == m_anchor || watched == m_list_view)))
        schedule_chrome_style_update();

    if (event->type() == QEvent::MouseButtonPress && is_visible()) {
        auto* mouse_event = static_cast<QMouseEvent*>(event);
        auto global = mouse_event->globalPosition().toPoint();
        auto host_global = QRect(m_host->mapToGlobal(QPoint(0, 0)), m_host->size());
        auto anchor_global = QRect(m_anchor->mapToGlobal(QPoint(0, 0)), m_anchor->size());
        if (!host_global.contains(global) && !anchor_global.contains(global)) {
            if (close())
                emit dismissed();
        }
    }
    return QObject::eventFilter(watched, event);
}

int Autocomplete::list_height() const
{
    int visible_count = static_cast<int>(min(m_model->visible_suggestion_count(), MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS));
    int total_height = 0;
    for (int i = 0; i < visible_count; ++i) {
        QStyleOptionViewItem option;
        option.initFrom(m_list_view);
        total_height += m_delegate->sizeHint(option, m_model->index(i, 0)).height();
    }

    // The list ends a little above the window's bottom edge.
    auto* top_window = m_anchor->window();
    auto list_top = m_anchor->mapTo(top_window, QPoint(0, m_anchor->height() + FIELD_DIVIDER_HEIGHT + LIST_PADDING)).y();
    int available_height = max(0, top_window->height() - list_top - LIST_PADDING);
    return min(total_height, available_height);
}

bool Autocomplete::is_selectable_row(int row) const
{
    return row >= 0 && row < m_model->rowCount();
}

}
