/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Platform.h>
#include <LibWebView/Autocomplete.h>
#include <UI/Qt/Autocomplete.h>
#include <UI/Qt/ChromeStyle.h>
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
#include <QTimer>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int POPUP_PADDING = 6;
static constexpr int CELL_HORIZONTAL_PADDING = 8;
static constexpr int CELL_VERTICAL_PADDING = 8;
static constexpr int CELL_ICON_SIZE = 16;
static constexpr int CELL_ICON_TEXT_SPACING = 6;
static constexpr int CELL_LABEL_VERTICAL_SPACING = 3;
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

static bool autocomplete_color_is_dark(QColor const& color)
{
    return color.lightness() < 128;
}

static QColor autocomplete_selection_fill(QPalette const& palette)
{
    auto text = ChromeStyle::chrome_text(palette);
    if (!autocomplete_color_is_dark(text))
        return ChromeStyle::mix(ChromeStyle::chrome_surface_pressed(palette), text, 0.06);

    auto surface = palette.color(QPalette::Base);
    if (autocomplete_color_is_dark(surface))
        surface = palette.color(QPalette::Window);
    if (autocomplete_color_is_dark(surface))
        surface = QColor(255, 255, 255);
    return ChromeStyle::mix(surface, QColor(0, 0, 0), 0.08);
}

static QColor autocomplete_selection_border(QPalette const& palette)
{
    return ChromeStyle::mix(ChromeStyle::chrome_border(palette), autocomplete_selection_fill(palette), ChromeStyle::is_dark(palette) ? 0.32 : 0.24);
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
            auto header_color = ChromeStyle::chrome_muted_text(option.palette);
            header_color.setAlpha(170);
            painter->setPen(header_color);
            auto rect = option.rect.adjusted(
                SECTION_HEADER_HORIZONTAL_PADDING, SECTION_HEADER_VERTICAL_PADDING,
                -SECTION_HEADER_HORIZONTAL_PADDING, -SECTION_HEADER_VERTICAL_PADDING);
            painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
            painter->restore();
            return;
        }

        bool selected = option.state & QStyle::State_Selected;
        if (selected) {
            auto rect = option.rect.adjusted(3, 2, -3, -2);
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(autocomplete_selection_border(option.palette), 1));
            painter->setBrush(autocomplete_selection_fill(option.palette));
            painter->drawRoundedRect(rect, 7, 7);
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
            create_chrome_icon(ChromeIcon::Search, option.palette).paint(painter, icon_rect);
        } else if (source == WebView::AutocompleteSuggestionSource::History && !favicon.isNull()) {
            favicon.paint(painter, icon_rect);
        } else {
            create_chrome_icon(ChromeIcon::Globe, option.palette).paint(painter, icon_rect);
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
            painter->setPen(ChromeStyle::chrome_text(option.palette));
            auto elided_title = primary_fm.elidedText(title_text, Qt::ElideRight, text_width);
            painter->drawText(QRect(text_x, block_y, text_width, primary_fm.height()),
                Qt::AlignLeft | Qt::AlignVCenter, elided_title);

            painter->setFont(autocomplete_secondary_font());
            auto secondary_color = ChromeStyle::chrome_muted_text(option.palette);
            secondary_color.setAlpha(180);
            painter->setPen(secondary_color);
            auto elided_secondary = secondary_fm.elidedText(secondary_text, Qt::ElideRight, text_width);
            painter->drawText(
                QRect(text_x, block_y + primary_fm.height() + CELL_LABEL_VERTICAL_SPACING,
                    text_width, secondary_fm.height()),
                Qt::AlignLeft | Qt::AlignVCenter, elided_secondary);
        } else {
            painter->setFont(QApplication::font());
            painter->setPen(ChromeStyle::chrome_text(option.palette));
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
{
    m_model = new AutocompleteModel(this);
    m_delegate = new AutocompleteDelegate(this);
    create_popup();
    qApp->installEventFilter(this);
}

void Autocomplete::create_popup()
{
    // Use a non-activating top-level window so it can stack above native web
    // content windows without moving keyboard focus away from the address bar.
    m_popup = new QFrame(m_anchor->window(), Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    connect(m_popup, &QObject::destroyed, this, [this] {
        // The popup dies with its parent window, which can go away while this object lives on (for
        // example when the tab is moved to another window); it is recreated on the next show.
        m_popup = nullptr;
        m_list_view = nullptr;
    });
    m_popup->setObjectName("LadybirdAutocompletePopup");
#if defined(AK_OS_MACOS)
    // The web content view is a native QRhiWidget on macOS, so the popup must
    // also be native to receive mouse events while overlapping it.
    m_popup->setAttribute(Qt::WA_NativeWindow);
#endif
    m_popup->setAttribute(Qt::WA_ShowWithoutActivating);
    m_popup->setAttribute(Qt::WA_X11DoNotAcceptFocus);
    m_popup->setFocusPolicy(Qt::NoFocus);
    m_popup->setFrameShape(QFrame::StyledPanel);
    m_popup->setFrameShadow(QFrame::Raised);
    m_popup->setAutoFillBackground(true);
    m_popup->hide();

    m_list_view = new QListView(m_popup);
    m_list_view->setObjectName("LadybirdAutocompleteList");
    m_list_view->setFocusPolicy(Qt::NoFocus);
    m_list_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list_view->setMouseTracking(true);
    m_list_view->setFrameShape(QFrame::NoFrame);
    m_list_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list_view->setModel(m_model);
    m_list_view->setItemDelegate(m_delegate);
    update_chrome_style();

    auto* layout = new QVBoxLayout(m_popup);
    layout->setContentsMargins(0, POPUP_PADDING, 0, POPUP_PADDING);
    layout->setSpacing(0);
    layout->addWidget(m_list_view);

    connect(m_list_view, &QAbstractItemView::clicked, this, [this](QModelIndex const& index) {
        if (!is_selectable_row(index.row()))
            return;
        emit suggestion_clicked(index.data(SuggestionIndexRole).toInt());
    });

    connect(m_list_view, &QAbstractItemView::entered, this, [this](QModelIndex const& index) {
        if (!is_selectable_row(index.row()))
            return;
        if (m_list_view->currentIndex() == index)
            return;
        emit suggestion_hovered(index.data(SuggestionIndexRole).toInt());
    });
}

Autocomplete::~Autocomplete()
{
    qApp->removeEventFilter(this);
    if (m_popup)
        delete m_popup;
}

void Autocomplete::update_chrome_style()
{
    if (!m_popup || m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    auto palette = QApplication::palette();
    m_popup->setPalette(palette);
    m_list_view->setPalette(palette);
    m_popup->setStyleSheet(ChromeStyle::autocomplete_popup_style_sheet(palette));
    m_popup->update();
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
    if (!m_popup)
        create_popup();

    m_model->set_suggestions(move(suggestions));
    if (m_model->rowCount() == 0) {
        close();
        return;
    }

    update_chrome_style();
    position_popup();
    if (!m_popup->isVisible()) {
        m_popup->show();
        position_popup();
    }
    m_popup->raise();

    set_selected_suggestion(selected_suggestion_index);
}

void Autocomplete::set_selected_suggestion(Optional<size_t> suggestion_index)
{
    if (!m_popup)
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
    if (!m_popup || !m_popup->isVisible())
        return false;
    m_popup->hide();
    return true;
}

bool Autocomplete::is_visible() const
{
    return m_popup && m_popup->isVisible();
}

bool Autocomplete::eventFilter(QObject* watched, QEvent* event)
{
    auto type = event->type();
    if (type == QEvent::ApplicationPaletteChange || type == QEvent::ThemeChange
        || (type == QEvent::PaletteChange && (watched == m_anchor || watched == m_popup || watched == m_list_view)))
        schedule_chrome_style_update();

    if (event->type() == QEvent::MouseButtonPress && is_visible()) {
        auto* mouse_event = static_cast<QMouseEvent*>(event);
        auto global = mouse_event->globalPosition().toPoint();
        auto popup_global = QRect(m_popup->mapToGlobal(QPoint(0, 0)), m_popup->size());
        auto anchor_global = QRect(m_anchor->mapToGlobal(QPoint(0, 0)), m_anchor->size());
        if (!popup_global.contains(global) && !anchor_global.contains(global)) {
            if (close())
                emit dismissed();
        }
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
    if (m_popup->parentWidget() != top_window) {
        m_popup->setParent(top_window, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        m_popup->setAttribute(Qt::WA_ShowWithoutActivating);
        m_popup->setAttribute(Qt::WA_X11DoNotAcceptFocus);
    }

    int width = std::max(m_anchor->width(), MINIMUM_POPUP_WIDTH);
    int frame_overhead = m_popup->frameWidth() * 2;
    int popup_height = total_height + POPUP_PADDING * 2 + frame_overhead;

    m_list_view->setFixedHeight(total_height);
    m_popup->setFixedSize(width, popup_height);

    auto popup_position = m_anchor->mapToGlobal(QPoint(0, m_anchor->height()));
    m_popup->move(popup_position);
    m_popup->raise();
}

bool Autocomplete::is_selectable_row(int row) const
{
    if (row < 0 || row >= m_model->rowCount())
        return false;
    auto const& rows = m_model->rows();
    return rows[row].kind == RowKind::Suggestion;
}

}
