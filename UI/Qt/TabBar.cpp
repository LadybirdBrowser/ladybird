/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/WindowControlButton.h>

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariant>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWindow>

namespace Ladybird {

static constexpr auto LADYBIRD_TAB_MIME_TYPE = "application/x-ladybird-tab";
static constexpr int HORIZONTAL_TAB_STRIP_HEIGHT = 44;
static constexpr int HORIZONTAL_TAB_HEIGHT = 38;
static constexpr int HORIZONTAL_TAB_MIN_WIDTH = 128;
static constexpr int HORIZONTAL_TAB_MAX_WIDTH = 240;
static constexpr int VERTICAL_TAB_HEIGHT = 38;
static constexpr int VERTICAL_TABS_COLLAPSED_WIDTH = browser_chrome_layout_policy().collapsed_sidebar_width;
static constexpr int VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH = browser_chrome_layout_policy().expanded_sidebar_width;
static constexpr int VERTICAL_TABS_MIN_EXPANDED_WIDTH = 190;
static constexpr int VERTICAL_TABS_MAX_EXPANDED_WIDTH = 400;
static constexpr int VERTICAL_TABS_RESIZE_HIT_AREA_WIDTH = 5;
static constexpr int VERTICAL_TABS_COLLAPSED_SIDE_MARGIN = 6;
static constexpr int VERTICAL_TABS_EXPANDED_SIDE_MARGIN = 5;
static constexpr int VERTICAL_TABS_TOP_MARGIN = 8;
static constexpr int TAB_CARD_SHAPE_HORIZONTAL_INSET = 5;
static constexpr qreal HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET = 4.0;
static constexpr int TAB_CARD_SHAPE_VERTICAL_INSET = 3;
static constexpr int HORIZONTAL_NEW_TAB_BUTTON_SHAPE_SIZE = 32;
static constexpr int TAB_CONTENT_HORIZONTAL_INSET = 8;
static constexpr int VERTICAL_TABS_HOVER_COLLAPSE_POLL_INTERVAL_MS = 250;
static constexpr int TAB_PREVIEW_HOVER_DELAY_MS = 350;
static constexpr int TAB_PREVIEW_THUMBNAIL_WIDTH = 320;
static constexpr int TAB_PREVIEW_THUMBNAIL_HEIGHT = 180;
static constexpr int TAB_PREVIEW_SHADOW_MARGIN = 12;
static constexpr int TAB_PREVIEW_CARD_GAP = 6;
static constexpr int TAB_BUTTON_SIZE = 22;
static constexpr int TAB_ICON_SIZE = 16;
static constexpr int COLLAPSED_VERTICAL_TAB_BUTTON_SIZE = 16;
static constexpr int COLLAPSED_VERTICAL_TAB_ICON_SIZE = 12;
static constexpr auto COLLAPSED_VERTICAL_TAB_BUTTON_PROPERTY = "collapsedVerticalTabButton";
static constexpr auto FULL_WIDTH_TOOLBAR_PROPERTY = "fullWidthToolbar";
static constexpr auto VERTICAL_TABS_EXPANDED_PROPERTY = "verticalTabsExpanded";
static constexpr auto VERTICAL_TABS_BUTTON_PROPERTY = "verticalTabsButton";
static constexpr auto VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY = "hovered";
static constexpr auto VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY = "active";

static void set_dynamic_property_if_needed(QWidget& widget, char const* property, QVariant const& value)
{
    if (widget.property(property) == value)
        return;

    widget.setProperty(property, value);
    widget.style()->unpolish(&widget);
    widget.style()->polish(&widget);
    widget.update();
}

static QPointer<TabWidget> s_active_tab_drag_source;
static QPointer<Tab> s_active_tab_dragged_tab;
static QPointer<TabWidget> s_pending_tab_drop_target;
static int s_pending_tab_drop_index { -1 };

static bool window_uses_client_side_decorations(QWidget& widget)
{
    auto* top_level_window = qobject_cast<BrowserWindow*>(widget.window());
    return !top_level_window || top_level_window->uses_client_side_decorations();
}

static QPainterPath tab_shape_path(QRectF const& rect, qreal top_radius, qreal bottom_radius)
{
    top_radius = min(top_radius, rect.height() / 2.0);
    bottom_radius = min(bottom_radius, rect.height() / 2.0);

    QPainterPath path;
    path.moveTo(rect.left() + top_radius, rect.top());
    path.lineTo(rect.right() - top_radius, rect.top());
    path.quadTo(rect.right(), rect.top(), rect.right(), rect.top() + top_radius);
    path.lineTo(rect.right(), rect.bottom() - bottom_radius);
    path.quadTo(rect.right(), rect.bottom(), rect.right() - bottom_radius, rect.bottom());
    path.lineTo(rect.left() + bottom_radius, rect.bottom());
    path.quadTo(rect.left(), rect.bottom(), rect.left(), rect.bottom() - bottom_radius);
    path.lineTo(rect.left(), rect.top() + top_radius);
    path.quadTo(rect.left(), rect.top(), rect.left() + top_radius, rect.top());
    path.closeSubpath();
    return path;
}

static void clear_layout(QLayout& layout)
{
    while (auto* item = layout.takeAt(0))
        delete item;
}

static constexpr int vertical_tabs_side_margin(bool expanded)
{
    return expanded ? VERTICAL_TABS_EXPANDED_SIDE_MARGIN : VERTICAL_TABS_COLLAPSED_SIDE_MARGIN;
}

static constexpr int vertical_tabs_horizontal_margin_width(TabLayout tab_layout)
{
    return vertical_tabs_side_margin(tab_layout != TabLayout::VerticalCollapsed) * 2;
}

static constexpr int vertical_tab_width(int available_width, TabLayout tab_layout)
{
    if (available_width > 0)
        return available_width;
    auto width = tab_layout == TabLayout::VerticalCollapsed ? VERTICAL_TABS_COLLAPSED_WIDTH : VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH;
    return width - vertical_tabs_horizontal_margin_width(tab_layout);
}

static constexpr int clamp_vertical_tabs_expanded_width(int width)
{
    return clamp(width, VERTICAL_TABS_MIN_EXPANDED_WIDTH, VERTICAL_TABS_MAX_EXPANDED_WIDTH);
}

static QRectF tab_card_shape_rect(QRectF const& rect)
{
    return rect.adjusted(TAB_CARD_SHAPE_HORIZONTAL_INSET, TAB_CARD_SHAPE_VERTICAL_INSET, -TAB_CARD_SHAPE_HORIZONTAL_INSET, -TAB_CARD_SHAPE_VERTICAL_INSET);
}

static QRectF horizontal_tab_card_shape_rect(QRectF const& rect)
{
    return rect.adjusted(HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET, TAB_CARD_SHAPE_VERTICAL_INSET, -HORIZONTAL_TAB_CARD_SHAPE_HORIZONTAL_INSET, -TAB_CARD_SHAPE_VERTICAL_INSET);
}

static QRectF horizontal_new_tab_button_shape_rect(QRectF const& rect)
{
    auto x = rect.left() + (rect.width() - HORIZONTAL_NEW_TAB_BUTTON_SHAPE_SIZE) / 2.0;
    auto y = rect.top() + (rect.height() - HORIZONTAL_NEW_TAB_BUTTON_SHAPE_SIZE) / 2.0;
    return { x, y, HORIZONTAL_NEW_TAB_BUTTON_SHAPE_SIZE, HORIZONTAL_NEW_TAB_BUTTON_SHAPE_SIZE };
}

static QRect tab_card_shape_rect(QRect const& rect)
{
    return rect.adjusted(TAB_CARD_SHAPE_HORIZONTAL_INSET, TAB_CARD_SHAPE_VERTICAL_INSET, -TAB_CARD_SHAPE_HORIZONTAL_INSET, -TAB_CARD_SHAPE_VERTICAL_INSET);
}

static QColor tab_hover_surface(QPalette const& palette, qreal hover_progress)
{
    auto dark = ChromeStyle::is_dark(palette);
    auto color = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
    color.setAlpha(static_cast<int>((dark ? 24 : 16) * hover_progress));
    return color;
}

static QColor selected_tab_border(QPalette const& palette, bool collapsed)
{
    auto dark = ChromeStyle::is_dark(palette);
    auto color = dark ? QColor(255, 255, 255) : QColor(0, 0, 0);
    color.setAlpha(collapsed ? (dark ? 32 : 30) : (dark ? 26 : 24));
    return color;
}

static QColor selected_tab_shadow(QPalette const& palette, int layer)
{
    auto dark = ChromeStyle::is_dark(palette);
    auto color = QColor(0, 0, 0);
    if (layer == 0)
        color.setAlpha(dark ? 112 : 22);
    else
        color.setAlpha(dark ? 50 : 10);
    return color;
}

static QRectF collapsed_vertical_tab_shape_rect(QRectF const& rect)
{
    return rect.adjusted(4.0, 3.0, -4.0, -3.0);
}

static QRect collapsed_vertical_tab_shape_rect(QRect const& rect)
{
    return rect.adjusted(4, 3, -4, -3);
}

class NewTabButton final : public QToolButton {
public:
    explicit NewTabButton(TabBar& tab_bar, QWidget* parent)
        : QToolButton(parent)
        , m_tab_bar(tab_bar)
    {
    }

private:
    bool is_hovered() const
    {
        return rect().contains(mapFromGlobal(QCursor::pos()));
    }

    virtual void enterEvent(QEnterEvent* event) override
    {
        QToolButton::enterEvent(event);
        update();
    }

    virtual void leaveEvent(QEvent* event) override
    {
        QToolButton::leaveEvent(event);
        update();
    }

    virtual void paintEvent(QPaintEvent* event) override
    {
        if (!property(VERTICAL_TABS_BUTTON_PROPERTY).toBool()) {
            QToolButton::paintEvent(event);
            return;
        }

        auto expanded = property(VERTICAL_TABS_EXPANDED_PROPERTY).toBool();

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        auto dark = ChromeStyle::is_dark(palette());
        auto is_horizontal_new_tab_button = m_tab_bar.tab_layout() == TabLayout::Horizontal;
        auto shape_rect = expanded
            ? tab_card_shape_rect(QRectF(rect()))
            : (is_horizontal_new_tab_button
                      ? horizontal_new_tab_button_shape_rect(QRectF(rect()))
                      : QRectF(rect()).adjusted(4.0, 3.0, -4.0, -3.0));
        auto tab_path = tab_shape_path(shape_rect, 9.0, 9.0);
        auto hovered = is_hovered();

        if (isDown()) {
            painter.setBrush(ChromeStyle::chrome_surface_pressed(palette()));
            painter.setPen(QPen(ChromeStyle::chrome_control_border(palette()), 1));
            painter.drawPath(tab_path);
        } else if (hovered) {
            painter.setBrush(tab_hover_surface(palette(), 1.0));
            painter.setPen(Qt::NoPen);
            painter.drawPath(tab_path);
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(TAB_CONTENT_HORIZONTAL_INSET, 0, -TAB_CONTENT_HORIZONTAL_INSET, 0);
        auto icon_size = QSize(TAB_ICON_SIZE, TAB_ICON_SIZE);
        QRect icon_rect {
            expanded ? contents_rect.left() - ((icon_size.width() - TAB_ICON_SIZE) / 2) : rect().center().x() - (icon_size.width() / 2),
            contents_rect.top() + ((contents_rect.height() - icon_size.height()) / 2),
            icon_size.width(),
            icon_size.height(),
        };
        if (is_horizontal_new_tab_button) {
            icon_rect.translate(0, 1);
            painter.translate(0.5, 0);
        } else if (!expanded) {
            painter.translate(1.0, 0);
        }
        icon().paint(&painter, icon_rect);
        if (!expanded)
            return;

        contents_rect.setLeft(icon_rect.right() + 8);

        auto text_color = ChromeStyle::mix(ChromeStyle::chrome_button_text(palette()), ChromeStyle::chrome_muted_text(palette()), dark ? 0.26 : 0.40);
        if (hovered || isDown())
            text_color = ChromeStyle::chrome_button_text(palette());
        painter.setPen(text_color);
        auto text_font = m_tab_bar.font();
        painter.setFont(text_font);

        QFontMetrics font_metrics(text_font);
        auto title = font_metrics.elidedText(text(), Qt::ElideRight, max(0, contents_rect.width()));
        contents_rect.translate(0, -1);
        painter.drawText(contents_rect, Qt::AlignLeft | Qt::AlignVCenter, title);
    }

    TabBar& m_tab_bar;
};

class TabPreviewThumbnail final : public QWidget {
public:
    explicit TabPreviewThumbnail(QWidget* parent)
        : QWidget(parent)
    {
        setFixedSize(TAB_PREVIEW_THUMBNAIL_WIDTH, TAB_PREVIEW_THUMBNAIL_HEIGHT);
    }

    void set_colors(QColor background, QColor border)
    {
        m_background = background;
        m_border = border;
        update();
    }

    void set_pixmap(QPixmap pixmap)
    {
        m_pixmap = AK::move(pixmap);
        update();
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        auto outer_rect = rect();
        outer_rect.adjust(0, 0, -1, -1);

        QPainterPath outer_path;
        outer_path.addRoundedRect(outer_rect, 7, 7);
        painter.fillPath(outer_path, m_background);

        if (!m_pixmap.isNull()) {
            auto image_rect = rect().adjusted(1, 1, -1, -1);
            QPainterPath image_path;
            image_path.addRoundedRect(image_rect, 6, 6);

            painter.save();
            painter.setClipPath(image_path);

            auto x = image_rect.x() + (image_rect.width() - m_pixmap.width()) / 2;
            auto y = image_rect.y() + (image_rect.height() - m_pixmap.height()) / 2;
            painter.drawPixmap(x, y, m_pixmap);
            painter.restore();
        }

        painter.setPen(m_border);
        painter.drawPath(outer_path);
    }

    QColor m_background;
    QColor m_border;
    QPixmap m_pixmap;
};

class TabPreviewPopup final : public QWidget {
public:
    explicit TabPreviewPopup(QWidget* parent)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);

        auto* outer_layout = new QVBoxLayout(this);
        outer_layout->setContentsMargins(TAB_PREVIEW_SHADOW_MARGIN, TAB_PREVIEW_SHADOW_MARGIN, TAB_PREVIEW_SHADOW_MARGIN, TAB_PREVIEW_SHADOW_MARGIN);
        outer_layout->setSpacing(0);

        m_card = new QFrame(this);
        m_card->setObjectName("LadybirdTabPreviewCard");

        auto* shadow = new QGraphicsDropShadowEffect(m_card);
        shadow->setBlurRadius(28);
        shadow->setOffset(0, 8);
        shadow->setColor(QColor(0, 0, 0, 92));
        m_card->setGraphicsEffect(shadow);

        auto* card_layout = new QVBoxLayout(m_card);
        card_layout->setContentsMargins(10, 10, 10, 10);
        card_layout->setSpacing(8);

        m_thumbnail = new TabPreviewThumbnail(m_card);

        m_title_label = new QLabel(m_card);
        m_title_label->setObjectName("LadybirdTabPreviewTitle");
        m_title_label->setFixedWidth(TAB_PREVIEW_THUMBNAIL_WIDTH);
        m_title_label->setTextFormat(Qt::PlainText);

        m_url_label = new QLabel(m_card);
        m_url_label->setObjectName("LadybirdTabPreviewURL");
        m_url_label->setFixedWidth(TAB_PREVIEW_THUMBNAIL_WIDTH);
        m_url_label->setTextFormat(Qt::PlainText);

        card_layout->addWidget(m_thumbnail);
        card_layout->addWidget(m_title_label);
        card_layout->addWidget(m_url_label);
        outer_layout->addWidget(m_card);
    }

    void set_preview(QPalette const& palette, QString title, QString url, QPixmap const& thumbnail)
    {
        update_chrome_style(palette);

        if (title.trimmed().isEmpty())
            title = "Untitled";

        QFontMetrics title_metrics(m_title_label->font());
        QFontMetrics url_metrics(m_url_label->font());

        m_thumbnail->set_pixmap(thumbnail);
        m_title_label->setText(title_metrics.elidedText(title, Qt::ElideRight, TAB_PREVIEW_THUMBNAIL_WIDTH));
        m_url_label->setText(url_metrics.elidedText(url, Qt::ElideMiddle, TAB_PREVIEW_THUMBNAIL_WIDTH));
        adjustSize();
    }

private:
    void update_chrome_style(QPalette const& palette)
    {
        auto card_background = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface(palette));
        auto thumbnail_background = ChromeStyle::chrome_surface_recessed(palette);
        auto border_color = ChromeStyle::chrome_control_border(palette);
        auto border = ChromeStyle::style_sheet_color(border_color);
        auto text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_text(palette));
        auto muted_text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_muted_text(palette));

        m_thumbnail->set_colors(thumbnail_background, border_color);

        setStyleSheet(qformatted(R"(
QFrame#LadybirdTabPreviewCard {{
    background: {};
    border: 1px solid {};
    border-radius: 10px;
}}

QLabel#LadybirdTabPreviewTitle {{
    color: {};
    font-weight: 600;
}}

QLabel#LadybirdTabPreviewURL {{
    color: {};
}}
)",
            card_background, border, text, muted_text));
    }

    QFrame* m_card { nullptr };
    TabPreviewThumbnail* m_thumbnail { nullptr };
    QLabel* m_title_label { nullptr };
    QLabel* m_url_label { nullptr };
};

TabBar::TabBar(TabWidget* tab_widget)
    : QTabBar(tab_widget)
    , m_tab_widget(tab_widget)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::NoFocus);
    setIconSize({ 16, 16 });
    setMinimumHeight(39);
    recreate_icons();

    m_tab_preview_timer = new QTimer(this);
    m_tab_preview_timer->setInterval(TAB_PREVIEW_HOVER_DELAY_MS);
    m_tab_preview_timer->setSingleShot(true);
    connect(m_tab_preview_timer, &QTimer::timeout, this, &TabBar::show_tab_preview);

    m_tab_preview_popup = new TabPreviewPopup(this);

    m_hover_animation = new QVariantAnimation(this);
    m_hover_animation->setDuration(120);
    m_hover_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_hover_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_hover_progress = value.toReal();
        update();
    });
    connect(m_hover_animation, &QVariantAnimation::finished, this, [this] {
        if (m_hover_progress <= 0.0)
            m_hover_animation_tab_index = -1;
    });
    connect(this, &QTabBar::currentChanged, this, [this](int index) {
        hide_tab_preview();
        ensure_tab_visible(index);
        update_tab_button_geometry();
    });
}

void TabBar::set_available_width(int width)
{
    if (m_available_width != width) {
        m_available_width = width;
        refresh_tab_layout();
    }
}

void TabBar::set_tab_layout(TabLayout tab_layout)
{
    if (m_tab_layout == tab_layout)
        return;

    m_tab_layout = tab_layout;

    if (m_tab_layout == TabLayout::Horizontal) {
        setShape(QTabBar::RoundedNorth);
        setMinimumSize({ 0, HORIZONTAL_TAB_HEIGHT });
        setMaximumWidth(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setUsesScrollButtons(true);
        set_vertical_scroll_offset(0);
    } else {
        setShape(QTabBar::RoundedWest);
        setMinimumSize({ 0, 0 });
        setMaximumWidth(QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setUsesScrollButtons(false);
    }

    refresh_tab_layout();
}

void TabBar::refresh_tab_layout()
{
    set_vertical_scroll_offset(m_vertical_scroll_offset);
    updateGeometry();
    update_tab_button_geometry();
    update();
}

void TabBar::recreate_icons()
{
    m_fallback_tab_icon = create_chrome_icon(ChromeIcon::Globe, palette());
    update();
}

QSize TabBar::sizeHint() const
{
    if (tab_layout() != TabLayout::Horizontal)
        return vertical_size_hint(count());
    return QTabBar::sizeHint();
}

QSize TabBar::minimumSizeHint() const
{
    if (tab_layout() != TabLayout::Horizontal)
        return vertical_size_hint(count() > 0 ? 1 : 0);
    return QTabBar::minimumSizeHint();
}

QSize TabBar::tabSizeHint(int index) const
{
    auto hint = QTabBar::tabSizeHint(index);

    if (tab_layout() != TabLayout::Horizontal) {
        hint.setWidth(vertical_tab_width(m_available_width, tab_layout()));
        hint.setHeight(VERTICAL_TAB_HEIGHT);
        return hint;
    }

    if (auto count = this->count(); count > 0) {
        auto width = (m_available_width > 0 ? m_available_width : this->width()) / count;
        width = min(HORIZONTAL_TAB_MAX_WIDTH, width);
        width = max(HORIZONTAL_TAB_MIN_WIDTH, width);

        hint.setWidth(width);
    }

    hint.setHeight(HORIZONTAL_TAB_HEIGHT);
    return hint;
}

void TabBar::resizeEvent(QResizeEvent* event)
{
    hide_tab_preview();
    QTabBar::resizeEvent(event);
    set_vertical_scroll_offset(m_vertical_scroll_offset);
    ensure_tab_visible(currentIndex());
    update_tab_button_geometry();
}

void TabBar::tabLayoutChange()
{
    hide_tab_preview();
    QTabBar::tabLayoutChange();
    set_vertical_scroll_offset(m_vertical_scroll_offset);
    update_tab_button_geometry();
}

bool TabBar::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* help_event = static_cast<QHelpEvent*>(event);
        auto hovered_tab = tab_index_at(help_event->pos());
        if (hovered_tab >= 0 && hovered_tab != currentIndex()) {
            if (auto* tab = m_tab_widget ? m_tab_widget->tab(hovered_tab) : nullptr) {
                if (tab->view().tab_preview_pixmap({ TAB_PREVIEW_THUMBNAIL_WIDTH, TAB_PREVIEW_THUMBNAIL_HEIGHT }).has_value()) {
                    event->accept();
                    return true;
                }
            }
        }
    }

    return QTabBar::event(event);
}

void TabBar::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    auto border = ChromeStyle::chrome_border(palette());
    auto dark = ChromeStyle::is_dark(palette());
    auto text_color = ChromeStyle::chrome_text(palette());
    auto is_vertical = tab_layout() != TabLayout::Horizontal;
    auto is_collapsed_vertical = tab_layout() == TabLayout::VerticalCollapsed;

    for (int index = 0; index < count(); ++index) {
        auto tab_rect = visual_tab_rect(index);
        if (!tab_rect.isValid())
            continue;
        if (is_vertical && (tab_rect.bottom() < 0 || tab_rect.top() > height()))
            continue;
        if (!is_vertical && (tab_rect.right() < 0 || tab_rect.left() > width()))
            continue;

        bool is_selected = index == currentIndex();
        auto hover_progress = index == m_hover_animation_tab_index ? m_hover_progress : (index == m_hovered_tab_index ? 1.0 : 0.0);
        bool is_hovered = hover_progress > 0.0;

        QRectF shape_rect;
        if (is_collapsed_vertical)
            shape_rect = collapsed_vertical_tab_shape_rect(QRectF(tab_rect));
        else if (m_tab_layout == TabLayout::Horizontal)
            shape_rect = horizontal_tab_card_shape_rect(QRectF(tab_rect));
        else
            shape_rect = tab_card_shape_rect(QRectF(tab_rect));
        auto tab_path = tab_shape_path(shape_rect, 9.0, 9.0);

        if (is_selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(selected_tab_shadow(palette(), 1));
            painter.drawPath(tab_path.translated(0, 2));
            painter.setBrush(selected_tab_shadow(palette(), 0));
            painter.drawPath(tab_path.translated(0, 1));

            auto selected_gradient = QLinearGradient(shape_rect.topLeft(), shape_rect.bottomLeft());
            selected_gradient.setColorAt(0.0, ChromeStyle::chrome_active_tab_surface_top(palette()));
            selected_gradient.setColorAt(1.0, ChromeStyle::chrome_active_tab_surface_bottom(palette()));
            painter.setBrush(selected_gradient);
            painter.setPen(QPen(selected_tab_border(palette(), is_collapsed_vertical), 1));
            painter.drawPath(tab_path);
        } else if (is_hovered) {
            painter.setBrush(tab_hover_surface(palette(), hover_progress));
            painter.setPen(Qt::NoPen);
            painter.drawPath(tab_path);
        }

        if (!is_selected && !is_hovered && index > 0 && index != currentIndex() + 1 && !is_collapsed_vertical) {
            auto separator = border;
            separator.setAlpha(dark ? 24 : 20);
            painter.setPen(separator);
            if (is_vertical)
                painter.drawLine(QPoint(tab_rect.left() + 16, tab_rect.top()), QPoint(tab_rect.right() - 16, tab_rect.top()));
            else
                painter.drawLine(QPoint(tab_rect.left(), 15), QPoint(tab_rect.left(), height() - 15));
        }

        auto icon = tabIcon(index);
        if (icon.isNull())
            icon = m_fallback_tab_icon;

        if (is_collapsed_vertical) {
            QRect icon_rect {
                tab_rect.center().x() - (TAB_ICON_SIZE / 2) + 1,
                tab_rect.center().y() - (TAB_ICON_SIZE / 2) + 1,
                TAB_ICON_SIZE,
                TAB_ICON_SIZE,
            };
            if (!is_selected)
                painter.setOpacity(is_hovered ? 0.92 : (dark ? 0.88 : 0.84));
            icon.paint(&painter, icon_rect);
            painter.setOpacity(1.0);
            continue;
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(TAB_CONTENT_HORIZONTAL_INSET, 0, -TAB_CONTENT_HORIZONTAL_INSET, 0);
        if (auto* left_button = tabButton(index, QTabBar::LeftSide); left_button && left_button->isVisible())
            contents_rect.setLeft(max(contents_rect.left(), left_button->geometry().right() + 6));
        if (auto* right_button = tabButton(index, QTabBar::RightSide); right_button && right_button->isVisible())
            contents_rect.setRight(min(contents_rect.right(), right_button->geometry().left() - 6));

        if (contents_rect.width() > 26) {
            QRect icon_rect {
                contents_rect.left(),
                contents_rect.top() + ((contents_rect.height() - TAB_ICON_SIZE) / 2),
                TAB_ICON_SIZE,
                TAB_ICON_SIZE,
            };
            if (!is_selected)
                painter.setOpacity(is_hovered ? 0.92 : (dark ? 0.88 : 0.84));
            icon.paint(&painter, icon_rect);
            painter.setOpacity(1.0);
            contents_rect.setLeft(icon_rect.right() + 8);
        }

        QFont tab_font = font();
        if (is_selected)
            tab_font.setWeight(QFont::DemiBold);
        painter.setFont(tab_font);

        QFontMetrics font_metrics(tab_font);
        auto title = font_metrics.elidedText(tabText(index), Qt::ElideRight, max(0, contents_rect.width()));
        auto tab_text_color = text_color;
        if (!is_selected)
            tab_text_color.setAlpha(is_hovered ? (dark ? 236 : 228) : (dark ? 226 : 216));
        painter.setPen(tab_text_color);
        painter.drawText(contents_rect, Qt::AlignLeft | Qt::AlignVCenter, title);
    }

    if (m_drop_indicator_index >= 0 && count() > 0) {
        auto indicator_color = ChromeStyle::chrome_accent(palette());
        indicator_color.setAlpha(220);
        painter.setPen(QPen(indicator_color, 3, Qt::SolidLine, Qt::RoundCap));

        if (is_vertical) {
            auto indicator_y = m_drop_indicator_index >= count()
                ? visual_tab_rect(count() - 1).bottom() + 3
                : visual_tab_rect(m_drop_indicator_index).top() + 1;
            indicator_y = max(3, min(height() - 3, indicator_y));
            painter.drawLine(QPointF(10, indicator_y), QPointF(width() - 10, indicator_y));
            return;
        }

        auto indicator_x = m_drop_indicator_index >= count()
            ? visual_tab_rect(count() - 1).right() + 3
            : visual_tab_rect(m_drop_indicator_index).left() + 1;
        indicator_x = max(2, min(width() - 3, indicator_x));

        painter.drawLine(QPointF(indicator_x, 8), QPointF(indicator_x, height() - 6));
    }
}

void TabBar::contextMenuEvent(QContextMenuEvent* event)
{
    if (!m_tab_widget)
        return;

    auto tab_index = tab_index_at(event->pos());
    if (tab_index < 0)
        return;

    if (auto* tab = m_tab_widget->tab(tab_index))
        tab->context_menu()->exec(event->globalPos());
}

void TabBar::dragEnterEvent(QDragEnterEvent* event)
{
    if (!m_tab_widget || !s_active_tab_drag_source || !event->mimeData()->hasFormat(LADYBIRD_TAB_MIME_TYPE)) {
        event->ignore();
        return;
    }

    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void TabBar::dragLeaveEvent(QDragLeaveEvent* event)
{
    hide_tab_preview();
    m_drop_indicator_index = -1;
    update();
    QTabBar::dragLeaveEvent(event);
}

void TabBar::dragMoveEvent(QDragMoveEvent* event)
{
    if (!m_tab_widget || !s_active_tab_drag_source || !event->mimeData()->hasFormat(LADYBIRD_TAB_MIME_TYPE)) {
        event->ignore();
        return;
    }

    auto drop_indicator_index = drop_indicator_index_for_insertion_index(insertion_index_at(event->position().toPoint()));
    if (m_drop_indicator_index != drop_indicator_index) {
        m_drop_indicator_index = drop_indicator_index;
        update();
    }

    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void TabBar::dropEvent(QDropEvent* event)
{
    hide_tab_preview();
    if (!m_tab_widget || !s_active_tab_drag_source || !event->mimeData()->hasFormat(LADYBIRD_TAB_MIME_TYPE)) {
        event->ignore();
        return;
    }

    s_pending_tab_drop_target = m_tab_widget;
    s_pending_tab_drop_index = insertion_index_at(event->position().toPoint());
    m_drop_indicator_index = -1;
    update();
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

bool TabBar::eventFilter(QObject* watched, QEvent* event)
{
    auto hovered_tab_for_button = [this](QObject* button) {
        for (int index = 0; index < count(); ++index) {
            if (tabButton(index, QTabBar::LeftSide) == button || tabButton(index, QTabBar::RightSide) == button)
                return index;
        }
        return -1;
    };

    if (auto hovered_tab = hovered_tab_for_button(watched); hovered_tab >= 0) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::MouseMove) {
            set_hovered_tab_index(hovered_tab);
        } else if (event->type() == QEvent::Leave) {
            auto cursor_position = mapFromGlobal(QCursor::pos());
            set_hovered_tab_index(tab_index_at(cursor_position));
        }
    }

    return QTabBar::eventFilter(watched, event);
}

void TabBar::mousePressEvent(QMouseEvent* event)
{
    hide_tab_preview();
    auto pressed_tab = tab_index_at(event->pos());
    m_pressed_tab = (m_tab_widget && pressed_tab >= 0) ? m_tab_widget->tab(pressed_tab) : nullptr;

    if (pressed_tab >= 0) {
        auto rect_of_current_tab = visual_tab_rect(pressed_tab);
        m_position_in_selected_tab_while_dragging = event->pos() - rect_of_current_tab.topLeft();
        m_drag_start_position = event->pos();
    }

    if (tab_layout() != TabLayout::Horizontal) {
        if (pressed_tab >= 0) {
            setCurrentIndex(pressed_tab);
        } else if (event->button() == Qt::LeftButton && start_window_move()) {
            event->accept();
            return;
        }
    } else {
        if (pressed_tab < 0 && event->button() == Qt::LeftButton && start_window_move()) {
            event->accept();
            return;
        }
        QTabBar::mousePressEvent(event);
    }

    if (m_pressed_tab)
        event->accept();
    else
        event->ignore();
}

void TabBar::mouseMoveEvent(QMouseEvent* event)
{
    set_hovered_tab_index(tab_index_at(event->pos()));

    if (count() == 0) {
        if (tab_layout() == TabLayout::Horizontal)
            QTabBar::mouseMoveEvent(event);
        return;
    }

    if (m_pressed_tab && event->buttons().testFlag(Qt::LeftButton)) {
        auto pressed_tab_index = m_tab_widget ? m_tab_widget->index_of(m_pressed_tab) : -1;
        if (pressed_tab_index < 0) {
            m_pressed_tab = nullptr;
        } else if ((event->pos() - m_drag_start_position).manhattanLength() >= QApplication::startDragDistance()) {
            start_tab_drag(pressed_tab_index);
            event->accept();
            return;
        }
    }

    if (m_pressed_tab)
        event->accept();
    else
        event->ignore();
}

void TabBar::leaveEvent(QEvent* event)
{
    hide_tab_preview();
    set_hovered_tab_index(-1);
    QTabBar::leaveEvent(event);
}

void TabBar::mouseReleaseEvent(QMouseEvent* event)
{
    auto pressed_tab = m_pressed_tab;
    m_pressed_tab = nullptr;

    if (event->button() == Qt::MiddleButton && pressed_tab && m_tab_widget) {
        if (auto index = m_tab_widget->index_of(pressed_tab); index >= 0 && tab_index_at(event->pos()) == index) {
            emit tabCloseRequested(index);
            event->accept();
            return;
        }
    }

    if (tab_layout() == TabLayout::Horizontal)
        QTabBar::mouseReleaseEvent(event);

    if (pressed_tab)
        event->accept();
}

void TabBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (window_uses_client_side_decorations(*this) && tab_index_at(event->pos()) < 0 && event->button() == Qt::LeftButton) {
        toggle_window_maximized();
        event->accept();
        return;
    }

    QTabBar::mouseDoubleClickEvent(event);
}

void TabBar::wheelEvent(QWheelEvent* event)
{
    if (tab_layout() == TabLayout::Horizontal || max_vertical_scroll_offset() <= 0) {
        QTabBar::wheelEvent(event);
        return;
    }

    auto old_offset = m_vertical_scroll_offset;
    auto pixel_delta = event->pixelDelta().y();
    auto angle_delta = event->angleDelta().y();

    if (pixel_delta != 0) {
        set_vertical_scroll_offset(m_vertical_scroll_offset - pixel_delta);
    } else if (angle_delta != 0) {
        auto scroll_delta = angle_delta * VERTICAL_TAB_HEIGHT / 120;
        if (scroll_delta == 0)
            scroll_delta = angle_delta > 0 ? 1 : -1;
        set_vertical_scroll_offset(m_vertical_scroll_offset - scroll_delta);
    }

    if (m_vertical_scroll_offset != old_offset)
        event->accept();
    else
        event->ignore();
}

int TabBar::insertion_index_at(QPoint const& position) const
{
    if (count() == 0)
        return 0;

    if (tab_layout() != TabLayout::Horizontal) {
        auto unscrolled_y = position.y() + m_vertical_scroll_offset;
        if (unscrolled_y <= 0)
            return 0;
        if (unscrolled_y >= count() * VERTICAL_TAB_HEIGHT)
            return count();

        auto index = unscrolled_y / VERTICAL_TAB_HEIGHT;
        if (unscrolled_y > (index * VERTICAL_TAB_HEIGHT) + (VERTICAL_TAB_HEIGHT / 2))
            return index + 1;
        return index;
    }

    auto index = tab_index_at(position);
    if (index < 0)
        return position.x() < visual_tab_rect(0).center().x() ? 0 : count();

    if (position.x() > visual_tab_rect(index).center().x())
        return index + 1;
    return index;
}

int TabBar::drop_indicator_index_for_insertion_index(int insertion_index) const
{
    if (s_active_tab_drag_source == m_tab_widget && s_active_tab_dragged_tab) {
        auto dragged_tab_index = m_tab_widget->index_of(s_active_tab_dragged_tab);
        if (dragged_tab_index >= 0 && (insertion_index == dragged_tab_index || insertion_index == dragged_tab_index + 1))
            return -1;
    }

    return insertion_index;
}

QPixmap TabBar::render_tab_drag_pixmap(int index) const
{
    auto tab_rect = visual_tab_rect(index);
    QPixmap pixmap(tab_rect.size() * devicePixelRatioF());
    pixmap.setDevicePixelRatio(devicePixelRatioF());
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setOpacity(0.75);
    const_cast<TabBar&>(*this).render(&painter, QPoint(), QRegion(tab_rect), QWidget::DrawChildren);
    return pixmap;
}

void TabBar::start_tab_drag(int index)
{
    hide_tab_preview();

    if (!m_tab_widget)
        return;

    auto* tab = m_tab_widget->tab(index);
    if (!tab)
        return;

    QPointer<Tab> dragged_tab = tab;
    auto* drag = new QDrag(this);
    auto* mime_data = new QMimeData;
    mime_data->setData(LADYBIRD_TAB_MIME_TYPE, QByteArray {});
    mime_data->setText(tab->title());
    drag->setMimeData(mime_data);
    drag->setPixmap(render_tab_drag_pixmap(index));
    drag->setHotSpot(m_position_in_selected_tab_while_dragging);

    s_active_tab_drag_source = m_tab_widget;
    s_active_tab_dragged_tab = dragged_tab;
    s_pending_tab_drop_target = nullptr;
    s_pending_tab_drop_index = -1;

    auto action = drag->exec(Qt::MoveAction, Qt::MoveAction);

    auto* source_window = qobject_cast<BrowserWindow*>(window());
    if (source_window && dragged_tab) {
        auto current_index = source_window->tab_index(dragged_tab);
        if (current_index >= 0) {
            if (action == Qt::MoveAction && s_pending_tab_drop_target) {
                if (auto* target_window = qobject_cast<BrowserWindow*>(s_pending_tab_drop_target->window()))
                    source_window->move_tab_to_window(current_index, *target_window, s_pending_tab_drop_index);
            } else if (action == Qt::IgnoreAction) {
                if (!m_tab_widget->tab_strip_global_rect().contains(QCursor::pos()) && m_tab_widget->count() > 1)
                    source_window->detach_tab_to_new_window(current_index, QCursor::pos());
            }
        }
    }

    s_active_tab_drag_source = nullptr;
    s_active_tab_dragged_tab = nullptr;
    s_pending_tab_drop_target = nullptr;
    s_pending_tab_drop_index = -1;
    m_pressed_tab = nullptr;
}

void TabBar::start_hover_animation(int tab_index, qreal target_progress)
{
    if (!m_hover_animation)
        return;

    if (tab_index < 0) {
        m_hover_animation_tab_index = -1;
        m_hover_progress = 0.0;
        update();
        return;
    }

    m_hover_animation->stop();
    auto start_progress = (m_hover_animation_tab_index == tab_index) ? m_hover_progress : 0.0;
    m_hover_animation_tab_index = tab_index;
    m_hover_animation->setStartValue(start_progress);
    m_hover_animation->setEndValue(target_progress);
    m_hover_animation->start();
}

void TabBar::schedule_tab_preview(int index)
{
    if (!m_tab_preview_timer || !m_tab_preview_popup)
        return;

    if (!m_tab_widget || index < 0 || index == currentIndex() || index >= count() || m_pressed_tab) {
        hide_tab_preview();
        return;
    }

    if (m_tab_preview_index == index && m_tab_preview_popup->isVisible())
        return;

    m_tab_preview_index = index;
    m_tab_preview_popup->hide();
    QToolTip::hideText();
    m_tab_preview_timer->start();
}

void TabBar::show_tab_preview()
{
    if (!m_tab_widget || !m_tab_preview_popup)
        return;

    auto index = m_tab_preview_index;
    if (index < 0 || index != m_hovered_tab_index || index == currentIndex() || index >= count()) {
        hide_tab_preview();
        return;
    }

    auto* tab = m_tab_widget->tab(index);
    if (!tab) {
        hide_tab_preview();
        return;
    }

    auto thumbnail = tab->view().tab_preview_pixmap({ TAB_PREVIEW_THUMBNAIL_WIDTH, TAB_PREVIEW_THUMBNAIL_HEIGHT });
    if (!thumbnail.has_value()) {
        hide_tab_preview();
        return;
    }

    m_tab_preview_popup->set_preview(palette(), tab->title(), qstring_from_ak_string(tab->view().url().serialize()), *thumbnail);
    m_tab_preview_popup->move(tab_preview_position_for(index, m_tab_preview_popup->sizeHint()));
    m_tab_preview_popup->show();
    m_tab_preview_popup->raise();
}

void TabBar::hide_tab_preview()
{
    if (m_tab_preview_timer)
        m_tab_preview_timer->stop();

    m_tab_preview_index = -1;

    if (m_tab_preview_popup)
        m_tab_preview_popup->hide();
}

QPoint TabBar::tab_preview_position_for(int index, QSize const& popup_size) const
{
    auto tab_rect = visual_tab_rect(index);
    auto screen = QGuiApplication::screenAt(mapToGlobal(tab_rect.center()));
    if (!screen)
        screen = window()->screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    auto available_geometry = screen ? screen->availableGeometry() : QRect {};

    QPoint position;
    if (tab_layout() == TabLayout::Horizontal) {
        position = mapToGlobal(QPoint {
            tab_rect.center().x() - popup_size.width() / 2,
            tab_rect.bottom() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGIN,
        });

        if (position.y() + popup_size.height() > available_geometry.bottom())
            position.setY(mapToGlobal(QPoint { 0, tab_rect.top() }).y() - popup_size.height() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGIN);
    } else {
        position = mapToGlobal(QPoint {
            tab_rect.right() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGIN,
            tab_rect.center().y() - popup_size.height() / 2,
        });

        if (position.x() + popup_size.width() > available_geometry.right())
            position.setX(mapToGlobal(QPoint { tab_rect.left(), 0 }).x() - popup_size.width() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGIN);
    }

    auto minimum_x = available_geometry.left() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGIN;
    auto maximum_x = available_geometry.right() - popup_size.width() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGIN;
    if (minimum_x <= maximum_x)
        position.setX(clamp(position.x(), minimum_x, maximum_x));

    auto minimum_y = available_geometry.top() + TAB_PREVIEW_CARD_GAP - TAB_PREVIEW_SHADOW_MARGIN;
    auto maximum_y = available_geometry.bottom() - popup_size.height() - TAB_PREVIEW_CARD_GAP + TAB_PREVIEW_SHADOW_MARGIN;
    if (minimum_y <= maximum_y)
        position.setY(clamp(position.y(), minimum_y, maximum_y));

    return position;
}

QRect TabBar::visual_tab_rect(int index) const
{
    if (tab_layout() == TabLayout::Horizontal)
        return tabRect(index);

    if (index < 0 || index >= count())
        return {};

    auto tab_width = width();
    if (tab_width <= 0)
        tab_width = vertical_size_hint(count()).width();

    return { 0, (index * VERTICAL_TAB_HEIGHT) - m_vertical_scroll_offset, tab_width, VERTICAL_TAB_HEIGHT };
}

int TabBar::tab_index_at(QPoint const& position) const
{
    if (tab_layout() == TabLayout::Horizontal)
        return tabAt(position);

    if (position.x() < 0 || position.x() >= width() || position.y() < 0 || position.y() >= height())
        return -1;

    auto index = (position.y() + m_vertical_scroll_offset) / VERTICAL_TAB_HEIGHT;
    if (index < 0 || index >= count())
        return -1;

    if (!visual_tab_rect(index).contains(position))
        return -1;

    return index;
}

QSize TabBar::vertical_size_hint(int tab_count) const
{
    return { vertical_tab_width(m_available_width, tab_layout()), tab_count * VERTICAL_TAB_HEIGHT };
}

int TabBar::max_vertical_scroll_offset() const
{
    if (tab_layout() == TabLayout::Horizontal)
        return 0;
    return max(0, (count() * VERTICAL_TAB_HEIGHT) - height());
}

void TabBar::set_vertical_scroll_offset(int offset)
{
    auto clamped_offset = max(0, min(offset, max_vertical_scroll_offset()));
    if (m_vertical_scroll_offset == clamped_offset)
        return;

    m_vertical_scroll_offset = clamped_offset;
    update_tab_button_geometry();
    update();
}

void TabBar::ensure_tab_visible(int index)
{
    if (tab_layout() == TabLayout::Horizontal || index < 0 || index >= count())
        return;

    auto tab_top = index * VERTICAL_TAB_HEIGHT;
    auto tab_bottom = tab_top + VERTICAL_TAB_HEIGHT;

    if (tab_top < m_vertical_scroll_offset)
        set_vertical_scroll_offset(tab_top);
    else if (tab_bottom > m_vertical_scroll_offset + height())
        set_vertical_scroll_offset(tab_bottom - height());
}

void TabBar::set_hovered_tab_index(int index)
{
    if (m_hovered_tab_index == index)
        return;

    auto previous_hovered_tab = m_hovered_tab_index;
    m_hovered_tab_index = index;
    update_tab_button_geometry();
    start_hover_animation(index >= 0 ? index : previous_hovered_tab, index >= 0 ? 1.0 : 0.0);
    schedule_tab_preview(index);
    update();
}

void TabBar::update_tab_button_geometry()
{
    auto set_button_collapsed_overlay = [](QWidget* button, bool enabled) {
        if (auto* tab_bar_button = qobject_cast<TabBarButton*>(button))
            tab_bar_button->set_collapsed_vertical_overlay(enabled);
    };

    auto prepare_button = [&](QWidget* button, bool collapsed_overlay) {
        if (!button)
            return;
        button->installEventFilter(this);
        set_button_collapsed_overlay(button, collapsed_overlay);
    };

    if (tab_layout() == TabLayout::Horizontal) {
        for (int index = 0; index < count(); ++index) {
            prepare_button(tabButton(index, QTabBar::LeftSide), false);
            prepare_button(tabButton(index, QTabBar::RightSide), false);
        }
        return;
    }

    // Keep this out of paintEvent(): setVisible(), setGeometry(), and raise()
    // dirty child widgets, and doing that while painting can schedule another
    // tab bar repaint before the current one has finished flushing.
    auto place_expanded_button = [&](int index, QTabBar::ButtonPosition position, QRect shape_rect) {
        auto* button = tabButton(index, position);
        if (!button)
            return;
        prepare_button(button, false);

        auto should_be_visible = position != QTabBar::RightSide || index == currentIndex() || index == m_hovered_tab_index;
        bool did_update_button = false;
        if (button->isVisible() != should_be_visible) {
            button->setVisible(should_be_visible);
            did_update_button = true;
        }

        auto button_size = button->size();
        if (button_size.isEmpty())
            button_size = button->sizeHint();
        if (button_size.isEmpty())
            return;

        auto x = position == QTabBar::RightSide ? shape_rect.right() - button_size.width() - 6 : shape_rect.left() + 6;
        auto y = shape_rect.top() + ((shape_rect.height() - button_size.height()) / 2);
        QRect button_geometry { { x, y }, button_size };
        if (button->geometry() != button_geometry) {
            button->setGeometry(button_geometry);
            did_update_button = true;
        }

        if (did_update_button)
            button->raise();
    };

    auto place_collapsed_button = [&](int index, QTabBar::ButtonPosition position, QRect tab_rect, QRect shape_rect) {
        auto* button = tabButton(index, position);
        if (!button)
            return;
        prepare_button(button, true);

        auto should_be_visible = position == QTabBar::LeftSide || index == m_hovered_tab_index;
        bool did_update_button = false;
        if (button->isVisible() != should_be_visible) {
            button->setVisible(should_be_visible);
            did_update_button = true;
        }

        auto button_size = button->size();
        if (button_size.isEmpty())
            button_size = button->sizeHint();
        if (button_size.isEmpty())
            return;

        QPoint button_position;
        if (position == QTabBar::RightSide) {
            button_position = {
                max(tab_rect.left(), shape_rect.left() - 5),
                max(tab_rect.top(), shape_rect.top() - 5),
            };
        } else {
            auto x = min(tab_rect.right() - button_size.width() + 1, shape_rect.right() - button_size.width() + 5);
            auto y = min(tab_rect.bottom() - button_size.height() + 1, shape_rect.bottom() - button_size.height() + 5);
            button_position = { x, y };
        }

        QRect button_geometry { button_position, button_size };
        if (button->geometry() != button_geometry) {
            button->setGeometry(button_geometry);
            did_update_button = true;
        }

        if (did_update_button)
            button->raise();
    };

    for (int index = 0; index < count(); ++index) {
        auto tab_rect = visual_tab_rect(index);
        if (!tab_rect.isValid())
            continue;

        if (tab_layout() == TabLayout::VerticalCollapsed) {
            auto shape_rect = collapsed_vertical_tab_shape_rect(tab_rect);
            place_collapsed_button(index, QTabBar::LeftSide, tab_rect, shape_rect);
            place_collapsed_button(index, QTabBar::RightSide, tab_rect, shape_rect);
        } else {
            auto shape_rect = tab_card_shape_rect(tab_rect);
            place_expanded_button(index, QTabBar::LeftSide, shape_rect);
            place_expanded_button(index, QTabBar::RightSide, shape_rect);
        }
    }
}

void TabBar::toggle_window_maximized()
{
    auto* top_level_window = window();
    if (top_level_window->isMaximized())
        top_level_window->showNormal();
    else
        top_level_window->showMaximized();
}

bool TabBar::start_window_move()
{
    auto* handle = window()->windowHandle();
    if (!handle)
        return false;
#if defined(AK_OS_MACOS)
    if (start_appkit_window_drag(*this))
        return true;
#endif
    return handle->startSystemMove();
}

TabWidget::TabWidget(QWidget* parent)
    : QWidget(parent)
{
    if (auto* top_level_window = window(); top_level_window != this)
        top_level_window->installEventFilter(this);

    m_vertical_tabs_expanded_width = Application::settings().tab_settings().vertical_tabs_expanded_width.value_or(VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH);
    m_vertical_tabs_expanded_width = clamp_vertical_tabs_expanded_width(m_vertical_tabs_expanded_width);

    m_tab_bar = new TabBar(this);
    setAcceptDrops(true);

    m_tab_bar->setDocumentMode(true);
    m_tab_bar->setElideMode(Qt::TextElideMode::ElideRight);
    m_tab_bar->setMovable(false);
    m_tab_bar->setTabsClosable(true);
    m_tab_bar->setExpanding(false);
    m_tab_bar->setUsesScrollButtons(true);
    m_tab_bar->setDrawBase(false);
    m_tab_bar->installEventFilter(this);

    m_toolbar_container = new QStackedWidget(this);
    m_toolbar_container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_toolbar_container->installEventFilter(this);
    m_stacked_widget = new QStackedWidget(this);
    m_page_column = new QWidget(this);
    m_page_column_layout = new QVBoxLayout(m_page_column);
    m_page_column_layout->setSpacing(0);
    m_page_column_layout->setContentsMargins(0, 0, 0, 0);

    m_new_tab_button = new NewTabButton(*m_tab_bar, this);
    m_new_tab_button->setObjectName("LadybirdNewTabButton");
    m_new_tab_button->setIconSize(QSize(18, 18));
    m_new_tab_button->setFixedSize(32, 32);
    m_new_tab_button->setFocusPolicy(Qt::NoFocus);
    m_new_tab_button->setToolTip("New Tab");
    m_new_tab_button->installEventFilter(this);

    auto window_control_buttons = create_window_control_buttons(*this, "LadybirdTabStripWindowControls", { 18, 18 }, { 40, 40 });
    m_window_controls = window_control_buttons.container;
    m_minimize_window_button = window_control_buttons.minimize;
    m_maximize_window_button = window_control_buttons.maximize;
    m_close_window_button = window_control_buttons.close;

    recreate_icons();

    m_tab_bar_row = new QWidget(this);
    m_tab_bar_row->setObjectName("LadybirdTabStrip");
    m_tab_bar_row_layout = new QHBoxLayout(m_tab_bar_row);
    m_tab_bar_row->installEventFilter(this);

    m_vertical_tabs_content = new QWidget(this);
    m_vertical_tabs_content_layout = new QHBoxLayout(m_vertical_tabs_content);
    m_vertical_tabs_content_layout->setSpacing(0);
    m_vertical_tabs_content_layout->setContentsMargins(0, 0, 0, 0);
    m_vertical_tabs_content->installEventFilter(this);

    m_vertical_tab_bar_column = new QWidget(this);
    m_vertical_tab_bar_column->setObjectName("LadybirdVerticalTabBar");
#if defined(AK_OS_MACOS)
    m_vertical_tab_bar_column->setAttribute(Qt::WA_NativeWindow);
#endif
    m_vertical_tab_bar_column_layout = new QVBoxLayout(m_vertical_tab_bar_column);
    m_vertical_tab_bar_column->setProperty(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, false);
    m_vertical_tab_bar_column->setProperty(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, false);
    m_vertical_tab_bar_column->installEventFilter(this);

    m_vertical_tabs_content_separator = new QWidget(this);
    m_vertical_tabs_content_separator->setObjectName("LadybirdVerticalTabsContentSeparator");
    m_vertical_tabs_content_separator->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_vertical_tabs_content_separator->hide();

    m_vertical_tabs_reserved_space = new QWidget(m_vertical_tabs_content);
    m_vertical_tabs_reserved_space->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

    m_vertical_tabs_resize_handle = new QWidget(this);
    m_vertical_tabs_resize_handle->setObjectName("LadybirdVerticalTabsResizeHandle");
#if defined(AK_OS_MACOS)
    m_vertical_tabs_resize_handle->setAttribute(Qt::WA_NativeWindow);
#endif
    m_vertical_tabs_resize_handle->setCursor(Qt::SizeHorCursor);
    m_vertical_tabs_resize_handle->installEventFilter(this);
    m_vertical_tabs_resize_handle->hide();

    m_vertical_tabs_hover_collapse_timer = new QTimer(this);
    m_vertical_tabs_hover_collapse_timer->setInterval(VERTICAL_TABS_HOVER_COLLAPSE_POLL_INTERVAL_MS);
    connect(m_vertical_tabs_hover_collapse_timer, &QTimer::timeout, this, [this]() {
        update_vertical_tabs_hover_expanded();
    });

    m_main_layout = new QVBoxLayout(this);
    m_main_layout->setSpacing(0);
    m_main_layout->setContentsMargins(0, 0, 0, 0);
    rebuild_layout();
    update_chrome_style();

    connect(m_tab_bar, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < m_stacked_widget->count())
            m_stacked_widget->setCurrentIndex(index);

        emit current_tab_changed(index);
        m_toolbar_container->setCurrentIndex(index);
        update_vertical_tabs_overlay_geometry();
        update_vertical_tabs_resize_handle();
    });

    connect(m_tab_bar, &QTabBar::tabCloseRequested, this, &TabWidget::tab_close_requested);

    connect(m_tab_bar, &QTabBar::tabMoved, this, [this](int from, int to) {
        ScopeGuard guard { [&]() { m_stacked_widget->blockSignals(false); } };
        m_stacked_widget->blockSignals(true);

        auto* widget = m_stacked_widget->widget(from);
        m_stacked_widget->removeWidget(widget);
        m_stacked_widget->insertWidget(to, widget);

        auto* toolbar = as<Tab>(widget)->toolbar_container();
        if (auto toolbar_index = m_toolbar_container->indexOf(toolbar); toolbar_index != -1) {
            m_toolbar_container->removeWidget(toolbar);
            m_toolbar_container->insertWidget(to, toolbar);
        }
        m_stacked_widget->setCurrentIndex(m_tab_bar->currentIndex());
    });

    connect(m_minimize_window_button, &QToolButton::clicked, this, [this] {
        window()->showMinimized();
    });
    connect(m_maximize_window_button, &QToolButton::clicked, this, [this] {
        toggle_window_maximized();
    });
    connect(m_close_window_button, &QToolButton::clicked, this, [this] {
        window()->close();
    });
}

void TabWidget::add_tab(Tab* widget, QString const& label)
{
    insert_tab(m_tab_bar->count(), widget, label);
}

void TabWidget::insert_tab(int index, Tab* widget, QString const& label)
{
    m_stacked_widget->insertWidget(index, widget);
    m_tab_bar->insertTab(index, label);
    widget->set_toolbar_container_in_tab_layout(false);
    m_toolbar_container->insertWidget(index, widget->toolbar_container());

    widget->set_vertical_tabs_enabled(m_vertical_tabs_enabled);

    update_toolbar_placement();
    update_tab_layout();
    update_tab_button_visibility();
}

void TabWidget::remove_tab(int index)
{
    take_tab(index);
}

Tab* TabWidget::take_tab(int index)
{
    auto* widget = m_stacked_widget->widget(index);
    if (!widget)
        return nullptr;

    m_stacked_widget->removeWidget(widget);
    auto* toolbar = as<Tab>(widget)->toolbar_container();
    if (m_toolbar_container->indexOf(toolbar) != -1)
        m_toolbar_container->removeWidget(toolbar);
    as<Tab>(widget)->set_toolbar_container_in_tab_layout(true);

    m_tab_bar->removeTab(index);

    if (m_tab_bar->count() > 0 && m_tab_bar->currentIndex() >= 0)
        m_stacked_widget->setCurrentIndex(m_tab_bar->currentIndex());

    update_tab_layout();
    update_tab_button_visibility();
    return as<Tab>(widget);
}

void TabWidget::set_current_tab(Tab* widget)
{
    if (auto index = m_stacked_widget->indexOf(widget); index >= 0)
        set_current_index(index);
}

int TabWidget::index_of(Tab* widget) const
{
    return m_stacked_widget->indexOf(widget);
}

void TabWidget::set_new_tab_action(QAction* action)
{
    disconnect(m_new_tab_button, &QToolButton::clicked, nullptr, nullptr);

    if (!action)
        return;

    connect(m_new_tab_button, &QToolButton::clicked, action, &QAction::trigger);
}

void TabWidget::set_tab_bar_visible(bool visible)
{
    if (m_tab_bar_visible == visible)
        return;

    m_tab_bar_visible = visible;
    update_tab_chrome_visibility();
    update_tab_layout();
}

void TabWidget::set_window_controls_visible(bool visible)
{
    m_window_controls_visible = visible;
    update_tab_toolbar_window_controls_visibility();
    update_tab_chrome_visibility();
    update_tab_layout();
}

void TabWidget::set_vertical_tabs_enabled(bool enabled)
{
    if (m_vertical_tabs_enabled == enabled)
        return;

    m_vertical_tabs_enabled = enabled;
    if (!m_vertical_tabs_enabled)
        set_vertical_tabs_hover_expanded(false);
    for (int index = 0; index < m_stacked_widget->count(); ++index) {
        tab(index)->set_vertical_tabs_enabled(enabled);
    }
    rebuild_layout();
}

void TabWidget::set_vertical_tabs_expanded(bool expanded)
{
    if (m_vertical_tabs_expanded == expanded)
        return;

    m_vertical_tabs_expanded = expanded;
    set_vertical_tabs_hover_expanded(false);
    rebuild_layout();
}

void TabWidget::set_vertical_tabs_expand_on_hover(bool expand_on_hover)
{
    if (m_vertical_tabs_expand_on_hover == expand_on_hover)
        return;

    m_vertical_tabs_expand_on_hover = expand_on_hover;
    if (!m_vertical_tabs_expand_on_hover)
        set_vertical_tabs_hover_expanded(false);
    rebuild_layout();
}

bool TabWidget::event(QEvent* event)
{
    if (auto type = event->type(); type == QEvent::PaletteChange) {
        recreate_icons();
        update_chrome_style();
    } else if (type == QEvent::WindowStateChange) {
        update_window_button_icons();
    } else if (type == QEvent::Leave) {
        defer_update_vertical_tabs_hover_expanded();
    }

    return QWidget::event(event);
}

void TabWidget::dragEnterEvent(QDragEnterEvent* event)
{
    accept_tab_drag(event);
}

void TabWidget::dragMoveEvent(QDragMoveEvent* event)
{
    accept_tab_drag(event);
}

void TabWidget::dropEvent(QDropEvent* event)
{
    accept_tab_drop(event, m_tab_bar->count());
}

bool TabWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == window() && event->type() == QEvent::Leave)
        defer_update_vertical_tabs_hover_expanded();

    if (watched == m_vertical_tabs_resize_handle) {
        auto reset_resize_handle = [this] {
            m_is_resizing_vertical_tabs = false;
            m_vertical_tabs_resize_handle->releaseMouse();
            QApplication::restoreOverrideCursor();
            set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, false);
        };

        if (event->type() == QEvent::Enter) {
            set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, true);
        } else if (event->type() == QEvent::Leave) {
            if (!m_is_resizing_vertical_tabs)
                set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, false);
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && m_vertical_tabs_expanded) {
                apply_vertical_tabs_expanded_width(VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH);
                persist_vertical_tabs_expanded_width();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() != Qt::LeftButton)
                return QWidget::eventFilter(watched, event);

            m_is_resizing_vertical_tabs = true;
            m_vertical_tabs_resize_start_global_x = mouse_event->globalPosition().toPoint().x();
            m_vertical_tabs_resize_start_width = m_vertical_tabs_expanded_width;
            m_vertical_tabs_resize_handle->grabMouse();
            QApplication::setOverrideCursor(Qt::SizeHorCursor);
            set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, true);
            return true;
        } else if (event->type() == QEvent::MouseMove) {
            if (!m_is_resizing_vertical_tabs)
                return QWidget::eventFilter(watched, event);

            auto* mouse_event = static_cast<QMouseEvent*>(event);
            auto delta = mouse_event->globalPosition().toPoint().x() - m_vertical_tabs_resize_start_global_x;
            apply_vertical_tabs_expanded_width(m_vertical_tabs_resize_start_width + delta);
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            if (!m_is_resizing_vertical_tabs)
                return QWidget::eventFilter(watched, event);

            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() != Qt::LeftButton)
                return QWidget::eventFilter(watched, event);

            persist_vertical_tabs_expanded_width();
            reset_resize_handle();
            set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, m_vertical_tabs_resize_handle->underMouse());
            return true;
        }
    }

    if (watched == m_vertical_tabs_content && event->type() == QEvent::Leave)
        defer_update_vertical_tabs_hover_expanded();

    if (watched == m_toolbar_container && event->type() == QEvent::Resize)
        update_tab_layout();

    auto is_vertical_tabs_hover_target = watched == m_vertical_tab_bar_column
        || watched == m_tab_bar
        || watched == m_new_tab_button;

    if (is_vertical_tabs_hover_target) {
        if (event->type() == QEvent::Enter) {
            set_vertical_tabs_hover_expanded(true);
        } else if (event->type() == QEvent::Leave) {
            defer_update_vertical_tabs_hover_expanded();
        }
    }

    if ((watched == m_tab_bar_row || watched == m_vertical_tab_bar_column) && window_uses_client_side_decorations(*this)) {
        auto is_empty_chrome_area = [this, watched](QMouseEvent const& mouse_event) {
            if (watched == m_vertical_tab_bar_column) {
                auto* child = m_vertical_tab_bar_column->childAt(mouse_event.pos());
                return child == nullptr;
            }

            return m_tab_bar_row->childAt(mouse_event.pos()) == nullptr;
        };

        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && is_empty_chrome_area(*mouse_event)) {
                toggle_window_maximized();
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && is_empty_chrome_area(*mouse_event) && start_window_move())
                return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void TabWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update_tab_layout();
}

void TabWidget::accept_tab_drag(QDragMoveEvent* event)
{
    if (!s_active_tab_drag_source || !event->mimeData()->hasFormat(LADYBIRD_TAB_MIME_TYPE)) {
        event->ignore();
        return;
    }

    auto* drag_area = tab_drag_area_widget();
    auto position_in_drag_area = drag_area->mapFrom(this, event->position().toPoint());
    if (!drag_area->rect().contains(position_in_drag_area)) {
        event->ignore();
        return;
    }

    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void TabWidget::accept_tab_drop(QDropEvent* event, int index)
{
    if (!s_active_tab_drag_source || !event->mimeData()->hasFormat(LADYBIRD_TAB_MIME_TYPE)) {
        event->ignore();
        return;
    }

    s_pending_tab_drop_target = this;
    s_pending_tab_drop_index = index;
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

QRect TabWidget::tab_strip_global_rect() const
{
    if (auto* widget = tab_drag_area_widget())
        return { widget->mapToGlobal(QPoint(0, 0)), widget->size() };
    return {};
}

TabLayout TabWidget::current_tab_layout() const
{
    if (!m_vertical_tabs_enabled)
        return TabLayout::Horizontal;
    return vertical_tabs_effectively_expanded() ? TabLayout::VerticalExpanded : TabLayout::VerticalCollapsed;
}

QWidget* TabWidget::tab_drag_area_widget() const
{
    return m_tab_bar->tab_layout() == TabLayout::Horizontal ? m_tab_bar_row : m_vertical_tab_bar_column;
}

bool TabWidget::vertical_tabs_effectively_expanded() const
{
    return m_vertical_tabs_expanded || m_vertical_tabs_hover_expanded;
}

bool TabWidget::can_expand_vertical_tabs_on_hover() const
{
    return m_vertical_tabs_enabled && m_vertical_tabs_expand_on_hover && !m_vertical_tabs_expanded;
}

bool TabWidget::cursor_is_over_vertical_tabs() const
{
    if (!m_vertical_tabs_content->isVisible())
        return false;

    if (m_vertical_tab_bar_column->underMouse() || m_tab_bar->underMouse() || m_new_tab_button->underMouse())
        return true;

    auto vertical_tabs_rect = QRect {
        m_vertical_tabs_content->mapToGlobal(QPoint { 0, 0 }),
        QSize { current_vertical_tabs_width(), m_vertical_tabs_content->height() }
    };
    return window()->underMouse() && vertical_tabs_rect.contains(QCursor::pos());
}

int TabWidget::vertical_tabs_layout_width() const
{
    if (!m_tab_bar_visible)
        return 0;
    return m_vertical_tabs_expanded ? m_vertical_tabs_expanded_width : VERTICAL_TABS_COLLAPSED_WIDTH;
}

bool TabWidget::should_show_window_controls_in_tab_toolbar() const
{
    return m_window_controls_visible && m_tab_bar->tab_layout() != TabLayout::Horizontal;
}

void TabWidget::update_toolbar_placement()
{
    auto use_full_width_toolbar = current_tab_layout() != TabLayout::Horizontal;

    for (int index = 0; index < m_stacked_widget->count(); ++index) {
        auto* current_tab = tab(index);
        auto* toolbar = current_tab->toolbar_container();
        set_dynamic_property_if_needed(*toolbar, FULL_WIDTH_TOOLBAR_PROPERTY, use_full_width_toolbar);
    }

    m_toolbar_container->setCurrentIndex(m_tab_bar->currentIndex());
    update_tab_toolbar_window_controls_visibility();
}

void TabWidget::update_tab_toolbar_window_controls_visibility()
{
    auto show_controls = should_show_window_controls_in_tab_toolbar();
    for (int index = 0; index < m_stacked_widget->count(); ++index)
        tab(index)->set_toolbar_window_controls_visible(show_controls);
}

void TabWidget::rebuild_layout()
{
    clear_layout(*m_main_layout);
    clear_layout(*m_tab_bar_row_layout);
    clear_layout(*m_page_column_layout);
    clear_layout(*m_vertical_tab_bar_column_layout);
    clear_layout(*m_vertical_tabs_content_layout);

    m_tab_bar->set_tab_layout(current_tab_layout());
    update_toolbar_placement();

    if (m_tab_bar->tab_layout() != TabLayout::Horizontal) {
        rebuild_layout_for_vertical_tabs();
        m_main_layout->addWidget(m_toolbar_container);
        m_page_column->hide();

        m_vertical_tabs_content_layout->addWidget(m_vertical_tabs_reserved_space);
        m_vertical_tabs_content_layout->addWidget(m_stacked_widget, 1);
        m_main_layout->addWidget(m_vertical_tabs_content, 1);
        m_vertical_tabs_content->show();
    } else {
        rebuild_layout_for_horizontal_tabs();
        rebuild_page_column();

        m_main_layout->addWidget(m_tab_bar_row);
        m_main_layout->addWidget(m_page_column, 1);
        m_page_column->show();
        m_vertical_tabs_content->hide();
    }

    update_tab_button_visibility();
    update_tab_chrome_visibility();
    update_tab_layout();
}

void TabWidget::rebuild_layout_for_horizontal_tabs()
{
    m_tab_bar_row->setMinimumHeight(HORIZONTAL_TAB_STRIP_HEIGHT);
    m_tab_bar_row_layout->setSpacing(0);
    m_tab_bar_row_layout->setContentsMargins(12, 2, 4, 1);

    m_new_tab_button->setText({});
    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_EXPANDED_PROPERTY, false);
    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_BUTTON_PROPERTY, true);
    m_new_tab_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_new_tab_button->setFixedSize(HORIZONTAL_TAB_HEIGHT, HORIZONTAL_TAB_HEIGHT);
    m_new_tab_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    if (use_left_traffic_light_window_controls()) {
        m_tab_bar_row_layout->addWidget(m_window_controls, 0, Qt::AlignVCenter);
        m_tab_bar_row_layout->addSpacing(16);
        m_tab_bar_row_layout->addWidget(m_tab_bar);
        m_tab_bar_row_layout->addWidget(m_new_tab_button, 0, Qt::AlignVCenter);
        m_tab_bar_row_layout->addStretch(1);
        return;
    }

    m_tab_bar_row_layout->addWidget(m_tab_bar);
    m_tab_bar_row_layout->addWidget(m_new_tab_button, 0, Qt::AlignVCenter);
    m_tab_bar_row_layout->addStretch(1);
    m_tab_bar_row_layout->addWidget(m_window_controls);
}

void TabWidget::rebuild_layout_for_vertical_tabs()
{
    auto reserved_width = vertical_tabs_layout_width();
    auto side_bar_width = current_vertical_tabs_width();
    m_vertical_tabs_reserved_space->setFixedWidth(reserved_width);
    m_vertical_tab_bar_column->setFixedWidth(side_bar_width);

    m_vertical_tab_bar_column_layout->setSpacing(0);
    auto tab_layout = m_tab_bar->tab_layout();
    auto side_margin = vertical_tabs_side_margin(tab_layout != TabLayout::VerticalCollapsed);
    m_window_controls->layout()->setContentsMargins(0, 0, 0, 0);
    m_vertical_tab_bar_column_layout->setContentsMargins(side_margin, VERTICAL_TABS_TOP_MARGIN, side_margin, 8);

    update_vertical_tabs_button_layout();

    m_vertical_tab_bar_column_layout->addWidget(m_tab_bar);
    m_vertical_tab_bar_column_layout->addWidget(m_new_tab_button);
    m_vertical_tab_bar_column_layout->addStretch(1);
}

void TabWidget::rebuild_page_column()
{
    m_page_column_layout->addWidget(m_toolbar_container);
    m_page_column_layout->addWidget(m_stacked_widget, 1);
}

int TabWidget::current_vertical_tabs_width() const
{
    return vertical_tabs_effectively_expanded() ? m_vertical_tabs_expanded_width : VERTICAL_TABS_COLLAPSED_WIDTH;
}

void TabWidget::apply_vertical_tabs_expanded_width(int width)
{
    auto clamped_width = clamp_vertical_tabs_expanded_width(width);
    if (m_vertical_tabs_expanded_width == clamped_width)
        return;

    m_vertical_tabs_expanded_width = clamped_width;
    update_tab_layout();
}

void TabWidget::persist_vertical_tabs_expanded_width()
{
    auto tab_settings = Application::settings().tab_settings();

    using ValueType = decltype(tab_settings.vertical_tabs_expanded_width)::ValueType;
    tab_settings.vertical_tabs_expanded_width = clamp(m_vertical_tabs_expanded_width, 0, NumericLimits<ValueType>::max());

    Application::settings().set_tab_settings(tab_settings);
}

void TabWidget::set_resize_handle_property(char const* property, bool enabled)
{
    set_dynamic_property_if_needed(*m_vertical_tab_bar_column, property, enabled);
}

void TabWidget::update_vertical_tabs_resize_handle()
{
    auto is_vertical = m_tab_bar->tab_layout() != TabLayout::Horizontal;
    auto show_resize_handle = m_tab_bar_visible && is_vertical && m_vertical_tabs_expanded;
    m_vertical_tabs_resize_handle->setVisible(show_resize_handle);
    if (!show_resize_handle) {
        m_vertical_tabs_resize_handle->releaseMouse();
        if (m_is_resizing_vertical_tabs)
            QApplication::restoreOverrideCursor();
        m_is_resizing_vertical_tabs = false;
        set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, false);
        set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, false);
        return;
    }

    auto handle_width = VERTICAL_TABS_RESIZE_HIT_AREA_WIDTH;
    auto divider_x = vertical_tabs_layout_width() - 1;
    auto chrome_rect = vertical_tabs_chrome_rect();
    m_vertical_tabs_resize_handle->setGeometry(
        divider_x - (handle_width / 2),
        chrome_rect.y(),
        handle_width,
        chrome_rect.height());
    m_vertical_tabs_resize_handle->raise();
}

void TabWidget::update_vertical_tabs_content_separator()
{
    auto show_content_separator = m_tab_bar_visible && m_tab_bar->tab_layout() != TabLayout::Horizontal;
    m_vertical_tabs_content_separator->setVisible(show_content_separator);
    if (!show_content_separator)
        return;

    auto separator_x = max(0, current_vertical_tabs_width() - 1);
    auto separator_y = max(0, m_toolbar_container->height() - 1);
    m_vertical_tabs_content_separator->setGeometry(separator_x, separator_y, max(0, width() - separator_x), 1);
    m_vertical_tabs_content_separator->raise();
}

void TabWidget::update_vertical_tabs_action_labels()
{
    auto text = vertical_tabs_effectively_expanded() ? "New Tab" : QString {};
    if (m_new_tab_button->text() != text)
        m_new_tab_button->setText(text);
}

void TabWidget::update_vertical_tabs_hover_layout()
{
    m_tab_bar->set_tab_layout(current_tab_layout());

    auto tab_layout = m_tab_bar->tab_layout();
    auto side_margin = vertical_tabs_side_margin(tab_layout != TabLayout::VerticalCollapsed);

    m_vertical_tab_bar_column_layout->setContentsMargins(side_margin, VERTICAL_TABS_TOP_MARGIN, side_margin, 8);
    update_vertical_tabs_button_layout();

    update_tab_button_visibility();
    update_tab_layout();
}

QRect TabWidget::vertical_tabs_chrome_rect() const
{
    auto chrome_height = m_toolbar_container->height();
    return {
        0,
        chrome_height,
        current_vertical_tabs_width(),
        max(0, height() - chrome_height)
    };
}

int TabWidget::vertical_tabs_tab_width() const
{
    return max(0, current_vertical_tabs_width() - vertical_tabs_horizontal_margin_width(m_tab_bar->tab_layout()));
}

void TabWidget::update_vertical_tabs_button_layout()
{
    auto expanded = vertical_tabs_effectively_expanded();
    auto tool_button_style = expanded ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly;
    if (m_new_tab_button->toolButtonStyle() != tool_button_style)
        m_new_tab_button->setToolButtonStyle(tool_button_style);
    update_vertical_tabs_action_labels();
    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_BUTTON_PROPERTY, true);
    set_dynamic_property_if_needed(*m_new_tab_button, VERTICAL_TABS_EXPANDED_PROPERTY, expanded);
    auto tab_width = vertical_tabs_tab_width();
    auto button_size = QSize { tab_width, VERTICAL_TAB_HEIGHT };
    if (m_new_tab_button->minimumSize() != button_size || m_new_tab_button->maximumSize() != button_size)
        m_new_tab_button->setFixedSize(button_size);
    m_new_tab_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void TabWidget::update_tab_layout()
{
    if (m_tab_bar->tab_layout() != TabLayout::Horizontal) {
        auto reserved_width = vertical_tabs_layout_width();
        auto side_bar_width = current_vertical_tabs_width();
        m_vertical_tabs_reserved_space->setFixedWidth(reserved_width);
        m_vertical_tab_bar_column->setFixedWidth(side_bar_width);
        update_vertical_tabs_button_layout();
        update_vertical_tabs_overlay_geometry();
        m_vertical_tabs_content_layout->activate();
        m_tab_bar->set_available_width(vertical_tabs_tab_width());
        update_vertical_tabs_resize_handle();
        update_vertical_tabs_content_separator();
        return;
    }

    update_vertical_tabs_resize_handle();
    update_vertical_tabs_content_separator();

    auto controls_width = m_new_tab_button->width();
    if (m_window_controls->isVisible())
        controls_width += m_window_controls->width();

    auto available_for_tabs = width() - controls_width - 36;

    m_tab_bar->set_available_width(available_for_tabs);

    auto tab_bar_width = min(available_for_tabs, m_tab_bar->count() * HORIZONTAL_TAB_MAX_WIDTH);
    m_tab_bar->setFixedWidth(max(0, tab_bar_width));
}

void TabWidget::update_tab_button_visibility()
{
    if (m_tab_bar->tab_layout() == TabLayout::VerticalCollapsed) {
        for (int index = 0; index < m_tab_bar->count(); ++index) {
            if (auto* button = m_tab_bar->tabButton(index, QTabBar::RightSide))
                button->hide();
        }

        m_tab_bar->refresh_tab_layout();
        return;
    }

    for (int index = 0; index < m_tab_bar->count(); ++index) {
        if (auto* button = m_tab_bar->tabButton(index, QTabBar::LeftSide))
            button->setVisible(true);
        if (auto* button = m_tab_bar->tabButton(index, QTabBar::RightSide))
            button->setVisible(true);
    }

    m_tab_bar->refresh_tab_layout();
}

void TabWidget::update_tab_chrome_visibility()
{
    auto tab_layout = m_tab_bar->tab_layout();
    auto is_horizontal = tab_layout == TabLayout::Horizontal;
    auto show_top_row = m_tab_bar_visible && is_horizontal;
    m_tab_bar_row->setVisible(show_top_row);
    m_toolbar_container->setVisible(m_tab_bar_visible);
    m_vertical_tab_bar_column->setVisible(m_tab_bar_visible && !is_horizontal);
    update_vertical_tabs_content_separator();

    auto show_window_controls = m_window_controls_visible
        && m_tab_bar_visible
        && is_horizontal;
    m_window_controls->setVisible(show_window_controls);
}

void TabWidget::recreate_icons()
{
    m_tab_bar->recreate_icons();
    m_new_tab_button->setIcon(create_chrome_icon(ChromeIcon::NewTab, palette()));
    update_vertical_tabs_action_labels();
    update_window_button_icons();
}

void TabWidget::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    auto style_sheet = ChromeStyle::tab_widget_style_sheet(palette());
    m_tab_bar_row->setStyleSheet(style_sheet);
    m_vertical_tab_bar_column->setStyleSheet(style_sheet);
    m_vertical_tabs_content_separator->setStyleSheet(style_sheet);
    m_vertical_tabs_resize_handle->setStyleSheet(style_sheet);
    m_is_updating_chrome_style = false;
}

void TabWidget::update_vertical_tabs_overlay_geometry()
{
    if (!m_tab_bar_visible || m_tab_bar->tab_layout() == TabLayout::Horizontal) {
        m_vertical_tab_bar_column->hide();
        return;
    }

    m_vertical_tab_bar_column->setGeometry(vertical_tabs_chrome_rect());
    m_vertical_tab_bar_column->show();
    m_vertical_tab_bar_column->raise();
}

void TabWidget::set_vertical_tabs_hover_expanded(bool expanded)
{
    expanded &= can_expand_vertical_tabs_on_hover();
    if (m_vertical_tabs_hover_expanded == expanded)
        return;

    m_vertical_tabs_hover_expanded = expanded;
    if (m_vertical_tabs_hover_expanded)
        m_vertical_tabs_hover_collapse_timer->start();
    else
        m_vertical_tabs_hover_collapse_timer->stop();
    update_vertical_tabs_hover_layout();
}

void TabWidget::defer_update_vertical_tabs_hover_expanded()
{
    QTimer::singleShot(0, this, [this]() {
        update_vertical_tabs_hover_expanded();
    });
}

void TabWidget::update_vertical_tabs_hover_expanded()
{
    if (!m_vertical_tabs_hover_expanded)
        return;

    if (!cursor_is_over_vertical_tabs())
        set_vertical_tabs_hover_expanded(false);
}

void TabWidget::update_window_button_icons()
{
    auto is_maximized = window()->isMaximized();
    m_minimize_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowMinimize, palette()));
    m_maximize_window_button->setIcon(create_chrome_icon(is_maximized ? ChromeIcon::WindowRestore : ChromeIcon::WindowMaximize, palette()));
    m_maximize_window_button->setToolTip(is_maximized ? "Restore" : "Maximize");
    m_close_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowClose, palette()));

    for (int index = 0; index < m_stacked_widget->count(); ++index)
        tab(index)->update_window_control_icons();
}

void TabWidget::toggle_window_maximized()
{
    auto* top_level_window = window();
    if (top_level_window->isMaximized())
        top_level_window->showNormal();
    else
        top_level_window->showMaximized();
    update_window_button_icons();
}

bool TabWidget::start_window_move()
{
    auto* handle = window()->windowHandle();
    if (!handle)
        return false;
#if defined(AK_OS_MACOS)
    if (start_appkit_window_drag(*this))
        return true;
#endif
    return handle->startSystemMove();
}

void TabBarButton::set_collapsed_vertical_overlay(bool enabled)
{
    if (property(COLLAPSED_VERTICAL_TAB_BUTTON_PROPERTY).toBool() == enabled)
        return;

    setProperty(COLLAPSED_VERTICAL_TAB_BUTTON_PROPERTY, enabled);

    QSize button_size { TAB_BUTTON_SIZE, TAB_BUTTON_SIZE };
    QSize icon_size { TAB_ICON_SIZE, TAB_ICON_SIZE };
    if (enabled) {
        button_size = { COLLAPSED_VERTICAL_TAB_BUTTON_SIZE, COLLAPSED_VERTICAL_TAB_BUTTON_SIZE };
        icon_size = { COLLAPSED_VERTICAL_TAB_ICON_SIZE, COLLAPSED_VERTICAL_TAB_ICON_SIZE };
    }

    setFixedSize(button_size);
    setIconSize(icon_size);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

TabBarButton::TabBarButton(QIcon const& icon, QWidget* parent)
    : QPushButton(icon, {}, parent)
{
    setObjectName("LadybirdTabButton");
    setFixedSize({ TAB_BUTTON_SIZE, TAB_BUTTON_SIZE });
    setIconSize({ TAB_ICON_SIZE, TAB_ICON_SIZE });
    setFocusPolicy(Qt::NoFocus);
    setFlat(true);
    hide();
}

bool TabBarButton::event(QEvent* event)
{
    if (event->type() == QEvent::Enter)
        setFlat(false);
    if (event->type() == QEvent::Leave)
        setFlat(true);

    return QPushButton::event(event);
}

}
