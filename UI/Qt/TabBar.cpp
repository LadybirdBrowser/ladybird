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
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/Settings.h>
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
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWindow>

namespace Ladybird {

static constexpr auto LADYBIRD_TAB_MIME_TYPE = "application/x-ladybird-tab";
static constexpr int HORIZONTAL_TAB_STRIP_HEIGHT = 43;
static constexpr int HORIZONTAL_TAB_HEIGHT = 36;
static constexpr int VERTICAL_TAB_HEIGHT = 38;
static constexpr int VERTICAL_TABS_COLLAPSED_WIDTH = 52;
static constexpr int VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH = 232;
static constexpr int VERTICAL_TABS_MIN_EXPANDED_WIDTH = 190;
static constexpr int VERTICAL_TABS_MAX_EXPANDED_WIDTH = 400;
static constexpr int VERTICAL_TABS_RESIZE_HIT_AREA_WIDTH = 5;
static constexpr int VERTICAL_TABS_SIDE_MARGIN = 6;
static constexpr int TAB_ICON_SIZE = 16;
static constexpr auto VERTICAL_TABS_EXPANDED_PROPERTY = "verticalTabsExpanded";
static constexpr auto VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY = "hovered";
static constexpr auto VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY = "active";

static QPointer<TabWidget> s_active_tab_drag_source;
static QPointer<Tab> s_active_tab_dragged_tab;
static QPointer<TabWidget> s_pending_tab_drop_target;
static int s_pending_tab_drop_index { -1 };

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

static constexpr int vertical_tab_width(int available_width, TabLayout tab_layout)
{
    if (available_width > 0)
        return available_width;
    auto width = tab_layout == TabLayout::VerticalCollapsed ? VERTICAL_TABS_COLLAPSED_WIDTH : VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH;
    return width - (VERTICAL_TABS_SIDE_MARGIN * 2);
}

static constexpr int clamp_vertical_tabs_expanded_width(int width)
{
    return clamp(width, VERTICAL_TABS_MIN_EXPANDED_WIDTH, VERTICAL_TABS_MAX_EXPANDED_WIDTH);
}

class NewTabButton final : public QToolButton {
public:
    using QToolButton::QToolButton;

private:
    virtual void paintEvent(QPaintEvent* event) override
    {
        if (!property(VERTICAL_TABS_EXPANDED_PROPERTY).toBool()) {
            QToolButton::paintEvent(event);
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        auto dark = ChromeStyle::is_dark(palette());
        auto surface = ChromeStyle::chrome_surface(palette());
        auto shape_rect = QRectF(rect()).adjusted(7.0, 3.0, -7.0, -3.0);
        auto tab_path = tab_shape_path(shape_rect, 9.0, 9.0);

        if (isDown()) {
            painter.setBrush(ChromeStyle::chrome_surface_pressed(palette()));
            painter.setPen(QPen(ChromeStyle::chrome_control_border(palette()), 1));
            painter.drawPath(tab_path);
        } else if (underMouse()) {
            auto hover = dark ? ChromeStyle::chrome_surface_hover(palette()) : ChromeStyle::mix(surface, ChromeStyle::chrome_surface_hover(palette()), 0.28);
            hover.setAlpha(dark ? 120 : 136);
            painter.setBrush(hover);
            painter.setPen(QPen(ChromeStyle::chrome_control_border(palette()), 1));
            painter.drawPath(tab_path);
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(12, 0, -12, 0);
        QRect icon_rect {
            contents_rect.left(),
            contents_rect.top() + ((contents_rect.height() - TAB_ICON_SIZE) / 2),
            TAB_ICON_SIZE,
            TAB_ICON_SIZE,
        };
        if (!underMouse() && !isDown())
            painter.setOpacity(dark ? 0.90 : 0.84);
        icon().paint(&painter, icon_rect);
        painter.setOpacity(1.0);
        contents_rect.setLeft(icon_rect.right() + 8);

        auto text_color = ChromeStyle::mix(ChromeStyle::chrome_button_text(palette()), ChromeStyle::chrome_muted_text(palette()), dark ? 0.26 : 0.40);
        if (underMouse() || isDown())
            text_color = ChromeStyle::chrome_button_text(palette());
        painter.setPen(text_color);
        painter.setFont(font());

        QFontMetrics font_metrics(font());
        auto title = font_metrics.elidedText(text(), Qt::ElideRight, max(0, contents_rect.width()));
        contents_rect.translate(0, -1);
        painter.drawText(contents_rect, Qt::AlignLeft | Qt::AlignVCenter, title);
    }
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
        ensure_tab_visible(index);
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
        setMinimumSize({ 0, 42 });
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
        width = min(240, width);
        width = max(128, width);

        hint.setWidth(width);
    }

    hint.setHeight(HORIZONTAL_TAB_HEIGHT);
    return hint;
}

void TabBar::resizeEvent(QResizeEvent* event)
{
    QTabBar::resizeEvent(event);
    set_vertical_scroll_offset(m_vertical_scroll_offset);
    ensure_tab_visible(currentIndex());
    update_tab_button_geometry();
}

void TabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    set_vertical_scroll_offset(m_vertical_scroll_offset);
    update_tab_button_geometry();
}

void TabBar::paintEvent(QPaintEvent*)
{
    update_tab_button_geometry();

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
            shape_rect = QRectF(tab_rect).adjusted(4.0, 3.0, -4.0, -3.0);
        else if (is_vertical)
            shape_rect = QRectF(tab_rect).adjusted(7.0, 3.0, -7.0, -3.0);
        else
            shape_rect = QRectF(tab_rect).adjusted(3.0, 2.0, -3.0, 1.0);
        auto tab_path = tab_shape_path(shape_rect, 9.0, is_vertical ? 9.0 : 8.0);
        auto surface = ChromeStyle::chrome_surface(palette());

        if (is_selected) {
            auto selected_gradient = QLinearGradient(shape_rect.topLeft(), shape_rect.bottomLeft());
            selected_gradient.setColorAt(0.0, ChromeStyle::chrome_active_tab_surface_top(palette()));
            selected_gradient.setColorAt(1.0, ChromeStyle::chrome_active_tab_surface_bottom(palette()));
            auto active_border = border;
            active_border.setAlpha(dark ? 62 : 72);
            painter.setBrush(selected_gradient);
            painter.setPen(QPen(active_border, 1));
            painter.drawPath(tab_path);
        } else if (is_hovered) {
            auto hover = dark ? ChromeStyle::chrome_surface_hover(palette()) : ChromeStyle::mix(surface, ChromeStyle::chrome_surface_hover(palette()), 0.28);
            hover.setAlpha(static_cast<int>((dark ? 120 : 136) * hover_progress));
            auto hover_border = border;
            hover_border.setAlpha(static_cast<int>((dark ? 58 : 64) * hover_progress));
            painter.setBrush(hover);
            painter.setPen(QPen(hover_border, 1));
            painter.drawPath(tab_path);
        } else if (is_vertical) {
            // Let background vertical tabs sit directly on the sidebar so they
            // do not read as disabled cards.
        } else {
            auto inactive = ChromeStyle::chrome_surface_recessed(palette());
            inactive.setAlpha(dark ? 104 : 150);
            auto inactive_border = border;
            inactive_border.setAlpha(dark ? 38 : 26);
            painter.setBrush(inactive);
            painter.setPen(QPen(inactive_border, 1));
            painter.drawPath(tab_path);
        }

        if (!is_selected && !is_hovered && index > 0 && index != currentIndex() + 1 && !is_collapsed_vertical) {
            auto separator = border;
            separator.setAlpha(dark ? 42 : 36);
            painter.setPen(separator);
            if (is_vertical)
                painter.drawLine(QPoint(tab_rect.left() + 16, tab_rect.top()), QPoint(tab_rect.right() - 16, tab_rect.top()));
            else
                painter.drawLine(QPoint(tab_rect.left(), 15), QPoint(tab_rect.left(), height() - 15));
        }

        auto icon = tabIcon(index);
        if (icon.isNull())
            icon = create_chrome_icon(ChromeIcon::Globe, palette());

        if (is_collapsed_vertical) {
            QRect icon_rect {
                tab_rect.center().x() - (TAB_ICON_SIZE / 2) + 1,
                tab_rect.center().y() - (TAB_ICON_SIZE / 2) + 1,
                TAB_ICON_SIZE,
                TAB_ICON_SIZE,
            };
            icon.paint(&painter, icon_rect);
            continue;
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(is_vertical ? 12 : 16, 0, is_vertical ? -12 : -14, 0);
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
            icon.paint(&painter, icon_rect);
            contents_rect.setLeft(icon_rect.right() + (is_vertical ? 8 : 7));
        }

        QFont tab_font = font();
        if (is_selected)
            tab_font.setWeight(QFont::DemiBold);
        painter.setFont(tab_font);

        QFontMetrics font_metrics(tab_font);
        auto title = font_metrics.elidedText(tabText(index), Qt::ElideRight, max(0, contents_rect.width()));
        painter.setPen(text_color);
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

void TabBar::mousePressEvent(QMouseEvent* event)
{
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
        QTabBar::mousePressEvent(event);
    }

    if (m_pressed_tab)
        event->accept();
    else
        event->ignore();
}

void TabBar::mouseMoveEvent(QMouseEvent* event)
{
    if (auto hovered_tab = tab_index_at(event->pos()); hovered_tab != m_hovered_tab_index) {
        m_hovered_tab_index = hovered_tab;
        start_hover_animation(hovered_tab, hovered_tab >= 0 ? 1.0 : 0.0);
        update();
    }

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
    auto previous_hovered_tab = m_hovered_tab_index;
    m_hovered_tab_index = -1;
    start_hover_animation(previous_hovered_tab, 0.0);
    update();
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
    if (tab_index_at(event->pos()) < 0 && event->button() == Qt::LeftButton) {
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

void TabBar::update_tab_button_geometry()
{
    if (tab_layout() == TabLayout::Horizontal || tab_layout() == TabLayout::VerticalCollapsed)
        return;

    auto place_button = [&](int index, QTabBar::ButtonPosition position, QRect shape_rect) {
        auto* button = tabButton(index, position);
        if (!button)
            return;
        button->setVisible(position != QTabBar::RightSide || index == currentIndex() || index == m_hovered_tab_index);

        auto button_size = button->size();
        if (button_size.isEmpty())
            button_size = button->sizeHint();
        if (button_size.isEmpty())
            return;

        auto x = position == QTabBar::RightSide ? shape_rect.right() - button_size.width() - 6 : shape_rect.left() + 6;
        auto y = shape_rect.top() + ((shape_rect.height() - button_size.height()) / 2);
        button->setGeometry({ { x, y }, button_size });
        button->raise();
    };

    for (int index = 0; index < count(); ++index) {
        auto tab_rect = visual_tab_rect(index);
        if (!tab_rect.isValid())
            continue;

        auto shape_rect = tab_rect.adjusted(7, 3, -7, -3);
        place_button(index, QTabBar::LeftSide, shape_rect);
        place_button(index, QTabBar::RightSide, shape_rect);
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
    return handle->startSystemMove();
}

TabWidget::TabWidget(QWidget* parent)
    : QWidget(parent)
{
    m_vertical_tabs_expanded_width = Settings::the()->vertical_tabs_expanded_width().value_or(VERTICAL_TABS_DEFAULT_EXPANDED_WIDTH);
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

    m_stacked_widget = new QStackedWidget(this);

    m_new_tab_button = new NewTabButton(this);
    m_new_tab_button->setObjectName("LadybirdNewTabButton");
    m_new_tab_button->setIconSize(QSize(18, 18));
    m_new_tab_button->setFixedSize(32, 32);
    m_new_tab_button->setFocusPolicy(Qt::NoFocus);
    m_new_tab_button->setToolTip("New Tab");

    m_minimize_window_button = new WindowControlButton("LadybirdWindowButton", "Minimize", { 18, 18 }, { 40, 40 }, this);
    m_maximize_window_button = new WindowControlButton("LadybirdWindowButton", "Maximize", { 18, 18 }, { 40, 40 }, this);
    m_close_window_button = new WindowControlButton("LadybirdCloseWindowButton", "Close", { 18, 18 }, { 40, 40 }, this);

    m_vertical_tabs_new_tab_separator = new QWidget(this);
    m_vertical_tabs_new_tab_separator->setObjectName("LadybirdVerticalTabsSeparator");
    m_vertical_tabs_new_tab_separator->setFixedHeight(1);
    m_vertical_tabs_new_tab_separator->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    recreate_icons();

    m_tab_bar_row = new QWidget(this);
    m_tab_bar_row->setObjectName("LadybirdTabStrip");
    m_tab_bar_row_layout = new QHBoxLayout(m_tab_bar_row);
    m_tab_bar_row->installEventFilter(this);

    m_vertical_tab_bar_column = new QWidget(this);
    m_vertical_tab_bar_column->setObjectName("LadybirdVerticalTabBar");
    m_vertical_tab_bar_column_layout = new QVBoxLayout(m_vertical_tab_bar_column);
    m_vertical_tab_bar_column->setProperty(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, false);
    m_vertical_tab_bar_column->setProperty(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, false);
    m_vertical_tab_bar_column->installEventFilter(this);

    m_vertical_tabs_content = new QWidget(this);
    m_vertical_tabs_content_layout = new QHBoxLayout(m_vertical_tabs_content);
    m_vertical_tabs_content_layout->setSpacing(0);
    m_vertical_tabs_content_layout->setContentsMargins(0, 0, 0, 0);

    m_vertical_tabs_resize_handle = new QWidget(m_vertical_tabs_content);
    m_vertical_tabs_resize_handle->setObjectName("LadybirdVerticalTabsResizeHandle");
    m_vertical_tabs_resize_handle->setCursor(Qt::SizeHorCursor);
    m_vertical_tabs_resize_handle->installEventFilter(this);
    m_vertical_tabs_resize_handle->hide();

    m_main_layout = new QVBoxLayout(this);
    m_main_layout->setSpacing(0);
    m_main_layout->setContentsMargins(0, 0, 0, 0);
    rebuild_layout();
    update_chrome_style();

    connect(m_tab_bar, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index < m_stacked_widget->count())
            m_stacked_widget->setCurrentIndex(index);

        emit current_tab_changed(index);
    });

    connect(m_tab_bar, &QTabBar::tabCloseRequested, this, &TabWidget::tab_close_requested);

    connect(m_tab_bar, &QTabBar::tabMoved, this, [this](int from, int to) {
        ScopeGuard guard { [&]() { m_stacked_widget->blockSignals(false); } };
        m_stacked_widget->blockSignals(true);

        auto* widget = m_stacked_widget->widget(from);
        m_stacked_widget->removeWidget(widget);
        m_stacked_widget->insertWidget(to, widget);
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

    widget->set_toolbar_window_controls_visible(m_window_controls_visible && m_vertical_tabs_enabled);
    widget->set_vertical_tabs_enabled(m_vertical_tabs_enabled);
    widget->set_vertical_tabs_expanded(m_vertical_tabs_expanded);

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
}

void TabWidget::set_window_controls_visible(bool visible)
{
    m_window_controls_visible = visible;
    auto show_tab_strip_window_controls = visible && m_tab_bar->tab_layout() == TabLayout::Horizontal;
    m_minimize_window_button->setVisible(show_tab_strip_window_controls);
    m_maximize_window_button->setVisible(show_tab_strip_window_controls);
    m_close_window_button->setVisible(show_tab_strip_window_controls);
    for (int index = 0; index < m_stacked_widget->count(); ++index)
        tab(index)->set_toolbar_window_controls_visible(visible && m_vertical_tabs_enabled);
    update_tab_chrome_visibility();
    update_tab_layout();
}

void TabWidget::set_vertical_tabs_enabled(bool enabled)
{
    if (m_vertical_tabs_enabled == enabled)
        return;

    m_vertical_tabs_enabled = enabled;
    for (int index = 0; index < m_stacked_widget->count(); ++index) {
        tab(index)->set_toolbar_window_controls_visible(m_window_controls_visible && enabled);
        tab(index)->set_vertical_tabs_enabled(enabled);
    }
    rebuild_layout();
}

void TabWidget::set_vertical_tabs_expanded(bool expanded)
{
    if (m_vertical_tabs_expanded == expanded)
        return;

    m_vertical_tabs_expanded = expanded;
    for (int index = 0; index < m_stacked_widget->count(); ++index)
        tab(index)->set_vertical_tabs_expanded(expanded);
    rebuild_layout();
}

bool TabWidget::event(QEvent* event)
{
    if (auto type = event->type(); type == QEvent::PaletteChange) {
        recreate_icons();
        update_chrome_style();
    } else if (type == QEvent::WindowStateChange) {
        update_window_button_icons();
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
    if (watched == m_vertical_tabs_resize_handle) {
        auto reset_resize_handle = [this] {
            m_is_resizing_vertical_tabs = false;
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

    if (watched == m_tab_bar_row || watched == m_vertical_tab_bar_column) {
        auto is_empty_chrome_area = [this, watched](QMouseEvent const& mouse_event) {
            if (watched == m_vertical_tab_bar_column) {
                auto* child = m_vertical_tab_bar_column->childAt(mouse_event.pos());
                return child == nullptr || child == m_vertical_tabs_new_tab_separator;
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
    return m_vertical_tabs_expanded ? TabLayout::VerticalExpanded : TabLayout::VerticalCollapsed;
}

QWidget* TabWidget::tab_drag_area_widget() const
{
    return m_tab_bar->tab_layout() == TabLayout::Horizontal ? m_tab_bar_row : m_vertical_tab_bar_column;
}

void TabWidget::rebuild_layout()
{
    clear_layout(*m_main_layout);
    clear_layout(*m_tab_bar_row_layout);
    clear_layout(*m_vertical_tab_bar_column_layout);
    clear_layout(*m_vertical_tabs_content_layout);

    m_tab_bar->set_tab_layout(current_tab_layout());

    if (m_tab_bar->tab_layout() != TabLayout::Horizontal) {
        rebuild_layout_for_vertical_tabs();

        m_vertical_tabs_content_layout->addWidget(m_vertical_tab_bar_column);
        m_vertical_tabs_content_layout->addWidget(m_stacked_widget, 1);
        m_main_layout->addWidget(m_vertical_tabs_content, 1);
        m_vertical_tabs_content->show();
    } else {
        rebuild_layout_for_horizontal_tabs();

        m_main_layout->addWidget(m_tab_bar_row);
        m_main_layout->addWidget(m_stacked_widget, 1);
        m_vertical_tabs_content->hide();
        m_vertical_tab_bar_column->hide();
    }

    update_tab_button_visibility();
    update_tab_chrome_visibility();
    update_tab_layout();
}

void TabWidget::rebuild_layout_for_horizontal_tabs()
{
    clear_layout(*m_tab_bar_row_layout);

    m_tab_bar_row->setMinimumHeight(HORIZONTAL_TAB_STRIP_HEIGHT);
    m_tab_bar_row_layout->setSpacing(4);
    m_tab_bar_row_layout->setContentsMargins(12, 2, 4, 1);

    m_new_tab_button->setText({});
    m_new_tab_button->setProperty(VERTICAL_TABS_EXPANDED_PROPERTY, false);
    m_new_tab_button->style()->unpolish(m_new_tab_button);
    m_new_tab_button->style()->polish(m_new_tab_button);
    m_new_tab_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_new_tab_button->setFixedSize(32, 32);
    m_new_tab_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_tab_bar_row_layout->addWidget(m_tab_bar);
    m_tab_bar_row_layout->addWidget(m_new_tab_button, 0, Qt::AlignVCenter);
    m_tab_bar_row_layout->addStretch(1);
    m_tab_bar_row_layout->addWidget(m_minimize_window_button);
    m_tab_bar_row_layout->addWidget(m_maximize_window_button);
    m_tab_bar_row_layout->addWidget(m_close_window_button);
}

void TabWidget::rebuild_layout_for_vertical_tabs()
{
    clear_layout(*m_tab_bar_row_layout);
    clear_layout(*m_vertical_tab_bar_column_layout);

    auto side_bar_width = current_vertical_tabs_width();
    m_vertical_tab_bar_column->setFixedWidth(side_bar_width);

    m_vertical_tab_bar_column_layout->setSpacing(m_vertical_tabs_expanded ? 0 : 4);
    m_vertical_tab_bar_column_layout->setContentsMargins(VERTICAL_TABS_SIDE_MARGIN, 8, VERTICAL_TABS_SIDE_MARGIN, 8);

    m_new_tab_button->setToolButtonStyle(m_vertical_tabs_expanded ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    update_vertical_tabs_action_labels();
    m_new_tab_button->setProperty(VERTICAL_TABS_EXPANDED_PROPERTY, m_vertical_tabs_expanded);
    m_new_tab_button->style()->unpolish(m_new_tab_button);
    m_new_tab_button->style()->polish(m_new_tab_button);
    m_vertical_tabs_new_tab_separator->setVisible(!m_vertical_tabs_expanded);
    if (m_vertical_tabs_expanded) {
        m_new_tab_button->setFixedHeight(VERTICAL_TAB_HEIGHT);
        m_new_tab_button->setMinimumWidth(32);
        m_new_tab_button->setMaximumWidth(QWIDGETSIZE_MAX);
        m_new_tab_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    } else {
        m_new_tab_button->setFixedSize(32, 32);
        m_new_tab_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    Qt::Alignment new_tab_button_alignment {};
    if (!m_vertical_tabs_expanded)
        new_tab_button_alignment = Qt::AlignHCenter;
    m_vertical_tab_bar_column_layout->addWidget(m_tab_bar);
    m_vertical_tab_bar_column_layout->addWidget(m_vertical_tabs_new_tab_separator);
    m_vertical_tab_bar_column_layout->addWidget(m_new_tab_button, 0, new_tab_button_alignment);
    m_vertical_tab_bar_column_layout->addStretch(1);
}

int TabWidget::current_vertical_tabs_width() const
{
    return m_vertical_tabs_expanded ? m_vertical_tabs_expanded_width : VERTICAL_TABS_COLLAPSED_WIDTH;
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
    Settings::the()->set_vertical_tabs_expanded_width(m_vertical_tabs_expanded_width);
}

void TabWidget::set_resize_handle_property(char const* property, bool enabled)
{
    if (m_vertical_tab_bar_column->property(property).toBool() == enabled)
        return;

    m_vertical_tab_bar_column->setProperty(property, enabled);
    m_vertical_tab_bar_column->style()->unpolish(m_vertical_tab_bar_column);
    m_vertical_tab_bar_column->style()->polish(m_vertical_tab_bar_column);
    m_vertical_tab_bar_column->update();
}

void TabWidget::update_vertical_tabs_resize_handle()
{
    auto is_vertical = m_tab_bar->tab_layout() != TabLayout::Horizontal;
    auto show_resize_handle = is_vertical && m_vertical_tabs_expanded;
    m_vertical_tabs_resize_handle->setVisible(show_resize_handle);
    if (!show_resize_handle) {
        m_is_resizing_vertical_tabs = false;
        set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_HOVERED_PROPERTY, false);
        set_resize_handle_property(VERTICAL_TABS_RESIZE_HANDLE_ACTIVE_PROPERTY, false);
        return;
    }

    auto handle_width = VERTICAL_TABS_RESIZE_HIT_AREA_WIDTH;
    auto divider_x = m_vertical_tab_bar_column->geometry().right();
    m_vertical_tabs_resize_handle->setGeometry(divider_x - (handle_width / 2), 0, handle_width, m_vertical_tabs_content->height());
    m_vertical_tabs_resize_handle->raise();
}

void TabWidget::update_vertical_tabs_action_labels()
{
    m_new_tab_button->setText(m_vertical_tabs_expanded ? "New Tab" : QString {});
}

void TabWidget::update_tab_layout()
{
    if (m_tab_bar->tab_layout() != TabLayout::Horizontal) {
        auto side_bar_width = current_vertical_tabs_width();
        m_vertical_tab_bar_column->setFixedWidth(side_bar_width);
        m_tab_bar->set_available_width(side_bar_width - (VERTICAL_TABS_SIDE_MARGIN * 2));
        update_vertical_tabs_resize_handle();
        return;
    }

    update_vertical_tabs_resize_handle();

    auto controls_width = m_new_tab_button->width();
    if (m_minimize_window_button->isVisible())
        controls_width += m_minimize_window_button->width();
    if (m_maximize_window_button->isVisible())
        controls_width += m_maximize_window_button->width();
    if (m_close_window_button->isVisible())
        controls_width += m_close_window_button->width();

    auto available_for_tabs = width() - controls_width - 36;

    m_tab_bar->set_available_width(available_for_tabs);
}

void TabWidget::update_tab_button_visibility()
{
    auto show_tab_buttons = m_tab_bar->tab_layout() != TabLayout::VerticalCollapsed;

    for (int index = 0; index < m_tab_bar->count(); ++index) {
        if (auto* button = m_tab_bar->tabButton(index, QTabBar::LeftSide))
            button->setVisible(show_tab_buttons);
        if (auto* button = m_tab_bar->tabButton(index, QTabBar::RightSide))
            button->setVisible(show_tab_buttons);
    }

    m_tab_bar->refresh_tab_layout();
}

void TabWidget::update_tab_chrome_visibility()
{
    auto show_top_row = m_tab_bar_visible && m_tab_bar->tab_layout() == TabLayout::Horizontal;
    m_tab_bar_row->setVisible(show_top_row);
    m_vertical_tab_bar_column->setVisible(m_tab_bar_visible && m_tab_bar->tab_layout() != TabLayout::Horizontal);
    auto show_tab_strip_window_controls = m_window_controls_visible && m_tab_bar->tab_layout() == TabLayout::Horizontal;
    m_minimize_window_button->setVisible(show_tab_strip_window_controls);
    m_maximize_window_button->setVisible(show_tab_strip_window_controls);
    m_close_window_button->setVisible(show_tab_strip_window_controls);
}

void TabWidget::recreate_icons()
{
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
    m_vertical_tabs_resize_handle->setStyleSheet(style_sheet);
    m_is_updating_chrome_style = false;
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
    return handle->startSystemMove();
}

TabBarButton::TabBarButton(QIcon const& icon, QWidget* parent)
    : QPushButton(icon, {}, parent)
{
    setObjectName("LadybirdTabButton");
    setFixedSize({ 22, 22 });
    setIconSize({ 16, 16 });
    setFocusPolicy(Qt::NoFocus);
    setFlat(true);
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
