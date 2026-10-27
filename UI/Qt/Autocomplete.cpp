/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/GenericShorthands.h>
#include <AK/Platform.h>
#include <AK/kmalloc.h>
#include <LibWebView/Autocomplete.h>
#include <UI/Qt/Autocomplete.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>

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
#include <QStyle>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWindow>

namespace Ladybird {

// The suggestions float in a card a little below the address field, as wide as the field.
static constexpr int POPUP_ANCHOR_GAP = 4;
static constexpr int POPUP_PADDING = 6;
static constexpr int POPUP_CORNER_RADIUS = 12;
// The card's shadow is drawn in a window of its own, with this much room around the card.
// NB: The shadow falls downward, so it needs less room above the card.
static constexpr QMargins POPUP_SHADOW_MARGINS { 60, 36, 60, 90 };
static constexpr int ROW_HEIGHT = 34;
static constexpr int ROW_CORNER_RADIUS = 6;
static constexpr int CELL_HORIZONTAL_PADDING = 12;
static constexpr int CELL_ICON_SIZE = 16;
static constexpr int CELL_SPACING = 12;
// Search suggestions are set apart from pages by a line, with this much room above and below it.
static constexpr int GROUP_SEPARATOR_MARGIN = 5;
static constexpr int GROUP_SEPARATOR_INSET = 8;
static constexpr int GROUP_SEPARATOR_HEIGHT = (2 * GROUP_SEPARATOR_MARGIN) + 1;
static constexpr int MINIMUM_POPUP_WIDTH = 100;

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

// The popup window is this card, with transparent corners.
class AutocompleteCard final : public QFrame {
    AK_ALLOC_WITH_KMALLOC;

public:
    using QFrame::QFrame;

protected:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        auto const& panel = Theme::panel();
        painter.setBrush(panel.surface);
        painter.setPen(QPen(panel.edge, 1));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), POPUP_CORNER_RADIUS, POPUP_CORNER_RADIUS);
    }
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
            QPixmap pixmap;
            if (!pixmap.loadFromData(suggestion.favicon_png->data(), static_cast<uint>(suggestion.favicon_png->size())))
                continue;
            m_favicon_cache.append({ index, QIcon(pixmap) });
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
    create_popup();
    qApp->installEventFilter(this);
}

void Autocomplete::create_popup()
{
    // Use a non-activating top-level widget for its own translucent backing store. Its native window is embedded in the
    // browser window in position_popup().
    m_popup = new AutocompleteCard(m_anchor->window(), Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    connect(m_popup, &QObject::destroyed, this, [this] {
        // The popup dies with its parent window, which can go away while this object lives on (for
        // example when the tab is moved to another window); it is recreated on the next show.
        m_popup = nullptr;
        m_list_view = nullptr;
        m_opening_animation = nullptr;
        set_anchor_popup_open(false);
    });
    m_popup->setObjectName("LadybirdAutocompletePopup");
    m_popup->setAttribute(Qt::WA_TranslucentBackground);
#if defined(AK_OS_MACOS)
    // The web content view is a native QRhiWidget on macOS, so the popup must
    // also be native to receive mouse events while overlapping it.
    m_popup->setAttribute(Qt::WA_NativeWindow);
#endif
    m_popup->setAttribute(Qt::WA_ShowWithoutActivating);
    m_popup->setAttribute(Qt::WA_X11DoNotAcceptFocus);
    m_popup->setFocusPolicy(Qt::NoFocus);
    m_popup->setFrameShape(QFrame::NoFrame);
    m_popup->hide();

    // The card casts the same shadow as popovers. The shadow is a window of its own behind the card, and it lets the
    // mouse through to the page, which a transparent room around the card in the card's window would not.
    m_shadow = new QWidget(m_anchor->window(), Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowTransparentForInput);
    m_shadow->setAttribute(Qt::WA_TranslucentBackground);
#if defined(AK_OS_MACOS)
    m_shadow->setAttribute(Qt::WA_NativeWindow);
#endif
    m_shadow->setAttribute(Qt::WA_ShowWithoutActivating);
    m_shadow->setAttribute(Qt::WA_X11DoNotAcceptFocus);
    m_shadow->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* shadow_layout = new QVBoxLayout(m_shadow);
    shadow_layout->setContentsMargins(POPUP_SHADOW_MARGINS);
    auto* shadow_caster = new AutocompleteCard(m_shadow);
    cast_popover_shadow(*shadow_caster);
    shadow_layout->addWidget(shadow_caster);
    m_shadow->hide();

    m_list_view = new QListView(m_popup);
    m_list_view->setObjectName("LadybirdAutocompleteList");
    m_list_view->setFocusPolicy(Qt::NoFocus);
    m_list_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list_view->setMouseTracking(true);
    m_list_view->setFrameShape(QFrame::NoFrame);
    m_list_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list_view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_list_view->setModel(m_model);
    m_list_view->setItemDelegate(m_delegate);
    update_chrome_style();

    auto* layout = new QVBoxLayout(m_popup);
    layout->setContentsMargins(POPUP_PADDING, POPUP_PADDING, POPUP_PADDING, POPUP_PADDING);
    layout->setSpacing(0);
    layout->addWidget(m_list_view);

    connect(m_list_view, &QAbstractItemView::clicked, this, [this](QModelIndex const& index) {
        if (!is_selectable_row(index.row()))
            return;
        emit suggestion_clicked(index.data(SuggestionIndexRole).toInt());
    });
}

Autocomplete::~Autocomplete()
{
    qApp->removeEventFilter(this);
    if (m_popup) {
        disconnect(m_popup, nullptr, this, nullptr);
        delete m_popup;
    }
    delete m_shadow.data();
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
    m_shadow->update();
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
        // The panel fades in from 4 px above its place, unless the system asks for less motion.
        if (!m_opening_animation) {
            m_opening_animation = new QVariantAnimation(m_popup);
            m_opening_animation->setDuration(Motion::FAST);
            m_opening_animation->setEasingCurve(Motion::enter_easing());
            m_opening_animation->setStartValue(0.0);
            m_opening_animation->setEndValue(1.0);
            QObject::connect(m_opening_animation, &QVariantAnimation::valueChanged, m_popup, [this](QVariant const& value) {
                auto progress = value.toReal();
                m_opening_offset = SystemAppearance::prefers_reduced_motion() ? 0 : -4 * (1 - progress);
                set_floating_window_opacity(*m_popup, progress);
                set_floating_window_opacity(*m_shadow, progress);
                position_popup();
            });
        }
        auto reduce_motion = SystemAppearance::prefers_reduced_motion();
        m_opening_offset = reduce_motion ? 0 : -4;
        set_floating_window_opacity(*m_popup, 0);
        set_floating_window_opacity(*m_shadow, 0);
        m_shadow->show();
        m_popup->show();
        position_popup();
        m_opening_animation->stop();
        m_opening_animation->start();
    }
    m_shadow->raise();
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
    m_shadow->hide();
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
    return m_popup && m_popup->isVisible();
}

bool Autocomplete::eventFilter(QObject* watched, QEvent* event)
{
    auto type = event->type();
    if (watched == m_popup && (type == QEvent::Show || type == QEvent::Hide))
        set_anchor_popup_open(type == QEvent::Show);

    // Qt still considers the embedded popup a top-level widget. Close it before application shutdown attempts to close
    // its native child window directly.
    if ((watched == m_anchor->window() && first_is_one_of(type, QEvent::Hide, QEvent::Resize)) || (watched == qApp && type == QEvent::Quit)) {
        if (close())
            emit dismissed();
    }

    if (is_visible() && (watched == m_anchor || watched == m_anchor->window())
        && (type == QEvent::Move || type == QEvent::Resize))
        position_popup();

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
    int visible_count = static_cast<int>(min(m_model->visible_suggestion_count(), MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS));
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
        ++seen_suggestions;
        if (seen_suggestions >= visible_count)
            break;
    }

    auto* top_window = m_anchor->window();
    if (!top_window)
        return;
    for (QWidget* window : { static_cast<QWidget*>(m_shadow.data()), static_cast<QWidget*>(m_popup) }) {
        if (window->parentWidget() != top_window) {
            window->setParent(top_window, window->windowFlags());
            window->setAttribute(Qt::WA_ShowWithoutActivating);
            window->setAttribute(Qt::WA_X11DoNotAcceptFocus);
        }

        // Embed the native window so it stacks above native web content without being animated independently of the
        // location field by the window system.
        (void)window->winId();
        window->windowHandle()->setParent(top_window->windowHandle());
    }

    // The card is as wide as the field and sits a little below it.
    int card_width = max(m_anchor->width(), MINIMUM_POPUP_WIDTH);
    auto card_position = m_anchor->mapTo(top_window, QPoint(0, m_anchor->height() + POPUP_ANCHOR_GAP + qRound(m_opening_offset)));
    int vertical_overhead = 2 * POPUP_PADDING;
    int available_height = max(0, top_window->height() - card_position.y() - vertical_overhead);
    int list_height = min(total_height, available_height);

    m_list_view->setFixedHeight(list_height);
    m_popup->setFixedSize(card_width, list_height + vertical_overhead);

    auto card = QRect(card_position, m_popup->size());
    m_shadow->setGeometry(card.marginsAdded(POPUP_SHADOW_MARGINS));
    m_popup->setGeometry(card);
    m_shadow->raise();
    m_popup->raise();
}

bool Autocomplete::is_selectable_row(int row) const
{
    return row >= 0 && row < m_model->rowCount();
}

}
