/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibWebView/Autocomplete.h>
#include <UI/Qt/Autocomplete.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/StringUtils.h>

#include <QAbstractListModel>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPoint>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int POPUP_PADDING = 8;
static constexpr int CELL_HORIZONTAL_PADDING = 8;
static constexpr int CELL_VERTICAL_PADDING = 10;
static constexpr int CELL_ICON_SIZE = 16;
static constexpr int CELL_ICON_TEXT_SPACING = 6;
static constexpr int CELL_LABEL_VERTICAL_SPACING = 4;
static constexpr int SECTION_HEADER_HORIZONTAL_PADDING = 10;
static constexpr int SECTION_HEADER_VERTICAL_PADDING = 4;
static constexpr int MINIMUM_POPUP_WIDTH = 100;
static constexpr size_t MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS = 8;

enum AutocompleteRole {
    RowKindRole = Qt::UserRole + 1,
    HeaderTextRole,
    TitleRole,
    SubtitleRole,
    UrlRole,
    FaviconRole,
    SourceRole,
    SuggestionIndexRole,
};

enum class RowKind {
    SectionHeader,
    Suggestion,
};

struct RowModel {
    RowKind kind;
    String header_text;
    size_t suggestion_index { 0 };
};

static QFont autocomplete_primary_font()
{
    QFont font = QApplication::font();
    font.setWeight(QFont::DemiBold);
    return font;
}

static QFont autocomplete_secondary_font()
{
    QFont font = QApplication::font();
    if (font.pointSizeF() > 0)
        font.setPointSizeF(font.pointSizeF() - 1.0);
    return font;
}

static QFont autocomplete_section_header_font()
{
    QFont font = autocomplete_secondary_font();
    font.setWeight(QFont::DemiBold);
    return font;
}

static QIcon globe_icon()
{
    static QIcon icon = create_tvg_icon_with_theme_colors("globe", QApplication::palette());
    return icon;
}

static QIcon search_icon()
{
    static QIcon icon = create_tvg_icon_with_theme_colors("search", QApplication::palette());
    return icon;
}

class AutocompleteModel final : public QAbstractListModel {
public:
    explicit AutocompleteModel(QObject* parent)
        : QAbstractListModel(parent)
    {
    }

    void set_suggestions(Vector<WebView::AutocompleteSuggestion> suggestions)
    {
        beginResetModel();
        m_suggestions = move(suggestions);
        m_rows.clear();
        m_favicon_cache.clear();

        auto current_section = WebView::AutocompleteSuggestionSection::None;
        for (size_t index = 0; index < m_suggestions.size(); ++index) {
            auto const& suggestion = m_suggestions[index];
            if (suggestion.section != WebView::AutocompleteSuggestionSection::None
                && suggestion.section != current_section) {
                current_section = suggestion.section;
                m_rows.append({
                    .kind = RowKind::SectionHeader,
                    .header_text = MUST(String::from_utf8(WebView::autocomplete_section_title(current_section))),
                });
            }
            m_rows.append({ .kind = RowKind::Suggestion, .header_text = {}, .suggestion_index = index });
        }

        for (size_t index = 0; index < m_suggestions.size(); ++index) {
            auto const& suggestion = m_suggestions[index];
            if (!suggestion.favicon_base64_png.has_value())
                continue;
            auto decoded = decode_base64(*suggestion.favicon_base64_png);
            if (decoded.is_error())
                continue;
            auto bytes = decoded.release_value();
            QPixmap pixmap;
            if (!pixmap.loadFromData(reinterpret_cast<uchar const*>(bytes.data()), static_cast<uint>(bytes.size())))
                continue;
            m_favicon_cache.append({ index, QIcon(pixmap) });
        }

        endResetModel();
    }

    int rowCount(QModelIndex const& parent = {}) const override
    {
        if (parent.isValid())
            return 0;
        return static_cast<int>(m_rows.size());
    }

    QVariant data(QModelIndex const& index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
            return {};

        auto const& row = m_rows[index.row()];
        if (role == RowKindRole)
            return static_cast<int>(row.kind);

        if (row.kind == RowKind::SectionHeader) {
            if (role == HeaderTextRole || role == Qt::DisplayRole)
                return qstring_from_ak_string(row.header_text);
            return {};
        }

        auto const& suggestion = m_suggestions[row.suggestion_index];

        switch (role) {
        case Qt::DisplayRole:
        case UrlRole:
            return qstring_from_ak_string(suggestion.text);
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
                if (entry.suggestion_index == row.suggestion_index)
                    return entry.icon;
            }
            return {};
        case SourceRole:
            return static_cast<int>(suggestion.source);
        case SuggestionIndexRole:
            return static_cast<int>(row.suggestion_index);
        default:
            return {};
        }
    }

    Qt::ItemFlags flags(QModelIndex const& index) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
            return Qt::NoItemFlags;
        auto const& row = m_rows[index.row()];
        if (row.kind == RowKind::SectionHeader)
            return Qt::ItemIsEnabled;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    Vector<RowModel> const& rows() const { return m_rows; }
    Vector<WebView::AutocompleteSuggestion> const& suggestions() const { return m_suggestions; }

    int table_row_for_suggestion_index(int suggestion_index) const
    {
        if (suggestion_index < 0)
            return -1;
        for (size_t i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i].kind == RowKind::Suggestion
                && m_rows[i].suggestion_index == static_cast<size_t>(suggestion_index))
                return static_cast<int>(i);
        }
        return -1;
    }

    size_t visible_suggestion_count() const
    {
        size_t count = 0;
        for (auto const& row : m_rows) {
            if (row.kind == RowKind::Suggestion)
                ++count;
        }
        return count;
    }

private:
    struct FaviconEntry {
        size_t suggestion_index;
        QIcon icon;
    };

    Vector<WebView::AutocompleteSuggestion> m_suggestions;
    Vector<RowModel> m_rows;
    Vector<FaviconEntry> m_favicon_cache;
};

class AutocompleteDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const& index) const override
    {
        if (!index.isValid())
            return {};
        auto kind = static_cast<RowKind>(index.data(RowKindRole).toInt());
        if (kind == RowKind::SectionHeader) {
            QFontMetrics fm(autocomplete_section_header_font());
            return QSize(0, fm.height() + SECTION_HEADER_VERTICAL_PADDING * 2);
        }
        QFontMetrics primary_fm(autocomplete_primary_font());
        QFontMetrics secondary_fm(autocomplete_secondary_font());
        int content_height = std::max(CELL_ICON_SIZE,
            primary_fm.height() + CELL_LABEL_VERTICAL_SPACING + secondary_fm.height());
        return QSize(0, content_height + CELL_VERTICAL_PADDING * 2);
    }

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        painter->save();
        auto kind = static_cast<RowKind>(index.data(RowKindRole).toInt());

        if (kind == RowKind::SectionHeader) {
            auto text = index.data(HeaderTextRole).toString();
            painter->setFont(autocomplete_section_header_font());
            painter->setPen(option.palette.color(QPalette::PlaceholderText));
            auto rect = option.rect.adjusted(
                SECTION_HEADER_HORIZONTAL_PADDING, SECTION_HEADER_VERTICAL_PADDING,
                -SECTION_HEADER_HORIZONTAL_PADDING, -SECTION_HEADER_VERTICAL_PADDING);
            painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
            painter->restore();
            return;
        }

        bool selected = option.state & QStyle::State_Selected;
        if (selected) {
            auto accent = option.palette.color(QPalette::Highlight);
            accent.setAlpha(64);
            auto rect = option.rect.adjusted(2, 3, -2, -3);
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(accent);
            painter->drawRoundedRect(rect, 6, 6);
        }

        auto favicon = index.data(FaviconRole).value<QIcon>();
        auto source = static_cast<WebView::AutocompleteSuggestionSource>(index.data(SourceRole).toInt());

        auto url_text = index.data(UrlRole).toString();
        auto title_text = index.data(TitleRole).toString();
        auto subtitle_text = index.data(SubtitleRole).toString();
        auto secondary_text = subtitle_text.isEmpty() ? url_text : subtitle_text;

        int icon_x = option.rect.left() + CELL_HORIZONTAL_PADDING;
        int icon_y = option.rect.top() + (option.rect.height() - CELL_ICON_SIZE) / 2;
        QRect icon_rect(icon_x, icon_y, CELL_ICON_SIZE, CELL_ICON_SIZE);

        if (source == WebView::AutocompleteSuggestionSource::Search) {
            search_icon().paint(painter, icon_rect);
        } else if (source == WebView::AutocompleteSuggestionSource::History && !favicon.isNull()) {
            favicon.paint(painter, icon_rect);
        } else {
            globe_icon().paint(painter, icon_rect);
        }

        int text_x = icon_x + CELL_ICON_SIZE + CELL_ICON_TEXT_SPACING;
        int text_width = option.rect.right() - text_x - CELL_HORIZONTAL_PADDING;
        if (text_width < 0)
            text_width = 0;

        QFontMetrics primary_fm(autocomplete_primary_font());
        QFontMetrics secondary_fm(autocomplete_secondary_font());

        if (!title_text.isEmpty()) {
            int block_height = primary_fm.height() + CELL_LABEL_VERTICAL_SPACING + secondary_fm.height();
            int block_y = option.rect.top() + (option.rect.height() - block_height) / 2;

            painter->setFont(autocomplete_primary_font());
            painter->setPen(option.palette.color(QPalette::Text));
            auto elided_title = primary_fm.elidedText(title_text, Qt::ElideRight, text_width);
            painter->drawText(QRect(text_x, block_y, text_width, primary_fm.height()),
                Qt::AlignLeft | Qt::AlignVCenter, elided_title);

            painter->setFont(autocomplete_secondary_font());
            painter->setPen(option.palette.color(QPalette::PlaceholderText));
            auto elided_secondary = secondary_fm.elidedText(secondary_text, Qt::ElideRight, text_width);
            painter->drawText(
                QRect(text_x, block_y + primary_fm.height() + CELL_LABEL_VERTICAL_SPACING,
                    text_width, secondary_fm.height()),
                Qt::AlignLeft | Qt::AlignVCenter, elided_secondary);
        } else {
            painter->setFont(QApplication::font());
            painter->setPen(option.palette.color(QPalette::Text));
            QFontMetrics fm(QApplication::font());
            auto elided_url = fm.elidedText(url_text, Qt::ElideRight, text_width);
            painter->drawText(
                QRect(text_x, option.rect.top(), text_width, option.rect.height()),
                Qt::AlignLeft | Qt::AlignVCenter, elided_url);
        }

        painter->restore();
    }
};

Autocomplete::Autocomplete(QLineEdit* anchor)
    : QObject(anchor)
    , m_anchor(anchor)
    , m_autocomplete(make<WebView::Autocomplete>())
{
    // The popup is parented to the anchor's top-level window in
    // position_popup() rather than made its own window, so that showing
    // it never causes the address bar to lose keyboard focus.
    m_popup = new QFrame();
    m_popup->setFocusPolicy(Qt::NoFocus);
    m_popup->setFrameShape(QFrame::StyledPanel);
    m_popup->setFrameShadow(QFrame::Raised);
    m_popup->setAutoFillBackground(true);
    m_popup->hide();

    m_list_view = new QListView(m_popup);
    m_list_view->setFocusPolicy(Qt::NoFocus);
    m_list_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list_view->setMouseTracking(true);
    m_list_view->setFrameShape(QFrame::NoFrame);
    m_list_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_model = new AutocompleteModel(this);
    m_delegate = new AutocompleteDelegate(this);
    m_list_view->setModel(m_model);
    m_list_view->setItemDelegate(m_delegate);

    auto* layout = new QVBoxLayout(m_popup);
    layout->setContentsMargins(0, POPUP_PADDING, 0, POPUP_PADDING);
    layout->setSpacing(0);
    layout->addWidget(m_list_view);

    connect(m_list_view, &QAbstractItemView::clicked, this, [this](QModelIndex const& index) {
        if (!is_selectable_row(index.row()))
            return;
        emit suggestion_activated(index.data(UrlRole).toString());
    });

    connect(m_list_view, &QAbstractItemView::entered, this, [this](QModelIndex const& index) {
        if (!is_selectable_row(index.row()))
            return;
        if (m_list_view->currentIndex() == index)
            return;
        select_row(index.row());
    });

    m_autocomplete->on_autocomplete_query_complete = [this](auto suggestions, auto result_kind) {
        if (on_query_complete)
            on_query_complete(move(suggestions), result_kind);
    };

    qApp->installEventFilter(this);
}

Autocomplete::~Autocomplete()
{
    qApp->removeEventFilter(this);
    delete m_popup;
}

void Autocomplete::query_autocomplete_engine(String query)
{
    m_autocomplete->query_autocomplete_engine(move(query), MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS);
}

void Autocomplete::cancel_pending_query()
{
    m_autocomplete->cancel_pending_query();
}

void Autocomplete::show_with_suggestions(Vector<WebView::AutocompleteSuggestion> suggestions, int selected_suggestion_index)
{
    m_model->set_suggestions(move(suggestions));
    if (m_model->rowCount() == 0) {
        close();
        return;
    }

    position_popup();
    if (!m_popup->isVisible())
        m_popup->show();

    int table_row = m_model->table_row_for_suggestion_index(selected_suggestion_index);
    if (table_row == -1)
        clear_selection();
    else
        select_row(table_row, false);
}

bool Autocomplete::close()
{
    if (!m_popup->isVisible())
        return false;
    m_popup->hide();
    emit did_close();
    return true;
}

bool Autocomplete::is_visible() const
{
    return m_popup && m_popup->isVisible();
}

void Autocomplete::clear_selection()
{
    m_list_view->setCurrentIndex({});
}

Optional<String> Autocomplete::selected_suggestion() const
{
    if (!is_visible())
        return {};
    auto index = m_list_view->currentIndex();
    if (!index.isValid() || !is_selectable_row(index.row()))
        return {};
    auto suggestion_index = index.data(SuggestionIndexRole).toInt();
    if (suggestion_index < 0 || suggestion_index >= static_cast<int>(m_model->suggestions().size()))
        return {};
    return m_model->suggestions()[suggestion_index].text;
}

bool Autocomplete::select_next_suggestion()
{
    if (m_model->rowCount() == 0)
        return false;

    if (!m_popup->isVisible()) {
        position_popup();
        m_popup->show();
        int row = step_to_selectable_row(-1, 1);
        if (row != -1)
            select_row(row);
        return true;
    }

    auto current = m_list_view->currentIndex();
    int start = current.isValid() ? current.row() : -1;
    int row = step_to_selectable_row(start, 1);
    if (row != -1)
        select_row(row);
    return true;
}

bool Autocomplete::select_previous_suggestion()
{
    if (m_model->rowCount() == 0)
        return false;

    if (!m_popup->isVisible()) {
        position_popup();
        m_popup->show();
        int row = step_to_selectable_row(0, -1);
        if (row != -1)
            select_row(row);
        return true;
    }

    auto current = m_list_view->currentIndex();
    int start = current.isValid() ? current.row() : 0;
    int row = step_to_selectable_row(start, -1);
    if (row != -1)
        select_row(row);
    return true;
}

bool Autocomplete::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress && is_visible()) {
        auto* mouse_event = static_cast<QMouseEvent*>(event);
        auto global = mouse_event->globalPosition().toPoint();
        auto popup_global = QRect(m_popup->mapToGlobal(QPoint(0, 0)), m_popup->size());
        auto anchor_global = QRect(m_anchor->mapToGlobal(QPoint(0, 0)), m_anchor->size());
        if (!popup_global.contains(global) && !anchor_global.contains(global))
            close();
    }
    return QObject::eventFilter(watched, event);
}

void Autocomplete::position_popup()
{
    int visible_count = static_cast<int>(std::min(m_model->visible_suggestion_count(), MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS));
    if (visible_count == 0)
        return;

    int total_height = 0;
    int seen_suggestions = 0;
    int row_count = m_model->rowCount();
    for (int i = 0; i < row_count; ++i) {
        auto index = m_model->index(i, 0);
        QStyleOptionViewItem option;
        option.initFrom(m_list_view);
        int h = m_delegate->sizeHint(option, index).height();
        total_height += h;
        if (static_cast<RowKind>(index.data(RowKindRole).toInt()) == RowKind::Suggestion) {
            ++seen_suggestions;
            if (seen_suggestions >= visible_count)
                break;
        }
    }

    auto* top_window = m_anchor->window();
    if (!top_window)
        return;
    if (m_popup->parentWidget() != top_window)
        m_popup->setParent(top_window);

    int width = std::max(m_anchor->width(), MINIMUM_POPUP_WIDTH);
    int frame_overhead = m_popup->frameWidth() * 2;
    int popup_height = total_height + POPUP_PADDING * 2 + frame_overhead;

    m_list_view->setFixedHeight(total_height);
    m_popup->setFixedSize(width, popup_height);

    auto pos_in_window = m_anchor->mapTo(top_window, QPoint(0, m_anchor->height()));
    m_popup->move(pos_in_window);
    m_popup->raise();
}

bool Autocomplete::is_selectable_row(int row) const
{
    if (row < 0 || row >= m_model->rowCount())
        return false;
    auto const& rows = m_model->rows();
    return rows[row].kind == RowKind::Suggestion;
}

int Autocomplete::step_to_selectable_row(int from, int direction) const
{
    int n = m_model->rowCount();
    if (n == 0)
        return -1;
    int candidate = from;
    for (int attempt = 0; attempt < n; ++attempt) {
        candidate += direction;
        if (candidate < 0)
            candidate = n - 1;
        else if (candidate >= n)
            candidate = 0;
        if (is_selectable_row(candidate))
            return candidate;
    }
    return -1;
}

void Autocomplete::select_row(int row, bool notify)
{
    if (!is_selectable_row(row))
        return;
    auto index = m_model->index(row, 0);
    m_list_view->setCurrentIndex(index);
    m_list_view->scrollTo(index);
    if (notify)
        emit suggestion_highlighted(index.data(UrlRole).toString());
}

}
