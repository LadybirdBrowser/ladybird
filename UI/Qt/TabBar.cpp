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
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWindow>

namespace Ladybird {

static constexpr auto LADYBIRD_TAB_MIME_TYPE = "application/x-ladybird-tab";

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

TabBar::TabBar(TabWidget* tab_widget)
    : QTabBar(tab_widget)
    , m_tab_widget(tab_widget)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::NoFocus);
    setIconSize({ 16, 16 });
    setMinimumHeight(42);

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
}

void TabBar::set_available_width(int width)
{
    if (m_available_width != width) {
        m_available_width = width;
        updateGeometry();
    }
}

QSize TabBar::tabSizeHint(int index) const
{
    auto hint = QTabBar::tabSizeHint(index);

    if (auto count = this->count(); count > 0) {
        auto width = (m_available_width > 0 ? m_available_width : this->width()) / count;
        width = min(240, width);
        width = max(128, width);

        hint.setWidth(width);
    }

    hint.setHeight(39);
    return hint;
}

void TabBar::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    auto border = ChromeStyle::chrome_border(palette());
    auto dark = ChromeStyle::is_dark(palette());

    auto text_color = palette().color(QPalette::Text);

    for (int index = 0; index < count(); ++index) {
        auto tab_rect = tabRect(index);
        if (!tab_rect.isValid() || tab_rect.right() < 0 || tab_rect.left() > width())
            continue;

        bool is_selected = index == currentIndex();
        auto hover_progress = index == m_hover_animation_tab_index ? m_hover_progress : (index == m_hovered_tab_index ? 1.0 : 0.0);
        bool is_hovered = hover_progress > 0.0;

        auto shape_rect = QRectF(tab_rect).adjusted(3.0, 1.5, -3.0, 0.5);
        auto tab_path = tab_shape_path(shape_rect, 10.0, 9.0);
        auto surface = ChromeStyle::chrome_surface(palette());

        if (is_selected) {
            auto selected_gradient = QLinearGradient(shape_rect.topLeft(), shape_rect.bottomLeft());
            selected_gradient.setColorAt(0.0, ChromeStyle::chrome_active_tab_surface_top(palette()));
            selected_gradient.setColorAt(1.0, ChromeStyle::chrome_active_tab_surface_bottom(palette()));
            auto active_border = border;
            active_border.setAlpha(38);
            painter.setBrush(selected_gradient);
            painter.setPen(QPen(active_border, 1));
            painter.drawPath(tab_path);
        } else if (is_hovered) {
            auto hover = dark ? ChromeStyle::chrome_surface_hover(palette()) : ChromeStyle::mix(surface, ChromeStyle::chrome_surface_hover(palette()), 0.28);
            hover.setAlpha(static_cast<int>((dark ? 120 : 136) * hover_progress));
            auto hover_border = border;
            hover_border.setAlpha(static_cast<int>(44 * hover_progress));
            painter.setBrush(hover);
            painter.setPen(QPen(hover_border, 1));
            painter.drawPath(tab_path);
        } else {
            auto inactive = ChromeStyle::chrome_surface_recessed(palette());
            inactive.setAlpha(dark ? 48 : 118);
            auto inactive_border = border;
            inactive_border.setAlpha(14);
            painter.setBrush(inactive);
            painter.setPen(QPen(inactive_border, 1));
            painter.drawPath(tab_path);
        }

        if (!is_selected && !is_hovered && index > 0 && index != currentIndex() + 1) {
            auto separator = border;
            separator.setAlpha(32);
            painter.setPen(separator);
            painter.drawLine(QPoint(tab_rect.left(), 17), QPoint(tab_rect.left(), height() - 17));
        }

        auto contents_rect = shape_rect.toAlignedRect().adjusted(16, 0, -14, 0);
        if (auto* left_button = tabButton(index, QTabBar::LeftSide); left_button && left_button->isVisible())
            contents_rect.setLeft(max(contents_rect.left(), left_button->geometry().right() + 6));
        if (auto* right_button = tabButton(index, QTabBar::RightSide); right_button && right_button->isVisible())
            contents_rect.setRight(min(contents_rect.right(), right_button->geometry().left() - 6));

        auto icon = tabIcon(index);
        if (!icon.isNull() && contents_rect.width() > 26) {
            constexpr int icon_size = 16;
            QRect icon_rect {
                contents_rect.left(),
                contents_rect.top() + (contents_rect.height() - icon_size) / 2,
                icon_size,
                icon_size,
            };
            icon.paint(&painter, icon_rect);
            contents_rect.setLeft(icon_rect.right() + 7);
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
        auto indicator_x = m_drop_indicator_index >= count()
            ? tabRect(count() - 1).right() + 3
            : tabRect(m_drop_indicator_index).left() + 1;
        indicator_x = max(2, min(width() - 3, indicator_x));
        auto indicator_color = ChromeStyle::chrome_accent(palette());
        indicator_color.setAlpha(220);

        painter.setPen(QPen(indicator_color, 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(indicator_x, 9), QPointF(indicator_x, height() - 7));
    }
}

void TabBar::contextMenuEvent(QContextMenuEvent* event)
{
    if (!m_tab_widget)
        return;

    auto tab_index = tabAt(event->pos());
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
    auto pressed_tab = tabAt(event->pos());
    m_pressed_tab = (m_tab_widget && pressed_tab >= 0) ? m_tab_widget->tab(pressed_tab) : nullptr;

    if (pressed_tab >= 0) {
        auto rect_of_current_tab = tabRect(pressed_tab);
        m_position_in_selected_tab_while_dragging = event->pos() - rect_of_current_tab.topLeft();
        m_drag_start_position = event->pos();
    }

    QTabBar::mousePressEvent(event);

    if (m_pressed_tab)
        event->accept();
    else
        event->ignore();
}

void TabBar::mouseMoveEvent(QMouseEvent* event)
{
    if (auto hovered_tab = tabAt(event->pos()); hovered_tab != m_hovered_tab_index) {
        m_hovered_tab_index = hovered_tab;
        start_hover_animation(hovered_tab, hovered_tab >= 0 ? 1.0 : 0.0);
        update();
    }

    if (count() == 0) {
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
    auto had_pressed_tab = !!m_pressed_tab;
    m_pressed_tab = nullptr;
    QTabBar::mouseReleaseEvent(event);
    if (had_pressed_tab)
        event->accept();
}

void TabBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (tabAt(event->pos()) < 0 && event->button() == Qt::LeftButton) {
        toggle_window_maximized();
        event->accept();
        return;
    }

    QTabBar::mouseDoubleClickEvent(event);
}

int TabBar::insertion_index_at(QPoint const& position) const
{
    if (count() == 0)
        return 0;

    auto index = tabAt(position);
    if (index < 0)
        return position.x() < tabRect(0).center().x() ? 0 : count();

    if (position.x() > tabRect(index).center().x())
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
    auto tab_rect = tabRect(index);
    QPixmap pixmap(tab_rect.size() * devicePixelRatioF());
    pixmap.setDevicePixelRatio(devicePixelRatioF());
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    const_cast<TabBar&>(*this).render(&painter, QPoint(-tab_rect.x(), -tab_rect.y()), QRegion(tab_rect), QWidget::DrawChildren);
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
                auto tab_bar_row_global = QRect(
                    m_tab_widget->tab_bar_row()->mapToGlobal(QPoint(0, 0)),
                    m_tab_widget->tab_bar_row()->size());
                if (!tab_bar_row_global.contains(QCursor::pos()) && m_tab_widget->count() > 1)
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

void TabBar::toggle_window_maximized()
{
    auto* top_level_window = window();
    if (top_level_window->isMaximized())
        top_level_window->showNormal();
    else
        top_level_window->showMaximized();
}

TabWidget::TabWidget(QWidget* parent)
    : QWidget(parent)
{
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

    m_new_tab_button = new QToolButton(this);
    m_new_tab_button->setObjectName("LadybirdNewTabButton");
    m_new_tab_button->setIconSize(QSize(20, 20));
    m_new_tab_button->setFixedSize(32, 32);
    m_new_tab_button->setFocusPolicy(Qt::NoFocus);
    m_new_tab_button->setToolTip("New Tab");

    m_minimize_window_button = new WindowControlButton("LadybirdWindowButton", "Minimize", { 18, 18 }, { 40, 40 }, this);
    m_maximize_window_button = new WindowControlButton("LadybirdWindowButton", "Maximize", { 18, 18 }, { 40, 40 }, this);
    m_close_window_button = new WindowControlButton("LadybirdCloseWindowButton", "Close", { 18, 18 }, { 40, 40 }, this);

    recreate_icons();

    auto* tab_bar_row_layout = new QHBoxLayout();
    tab_bar_row_layout->setSpacing(4);
    tab_bar_row_layout->setContentsMargins(12, 3, 4, 1);
    tab_bar_row_layout->addWidget(m_tab_bar);
    tab_bar_row_layout->addWidget(m_new_tab_button, 0, Qt::AlignVCenter);
    tab_bar_row_layout->addStretch(1);
    tab_bar_row_layout->addWidget(m_minimize_window_button);
    tab_bar_row_layout->addWidget(m_maximize_window_button);
    tab_bar_row_layout->addWidget(m_close_window_button);

    m_tab_bar_row = new QWidget(this);
    m_tab_bar_row->setObjectName("LadybirdTabStrip");
    m_tab_bar_row->setMinimumHeight(46);
    m_tab_bar_row->setLayout(tab_bar_row_layout);
    m_tab_bar_row->installEventFilter(this);

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(0);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->addWidget(m_tab_bar_row);
    main_layout->addWidget(m_stacked_widget, 1);
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

    update_tab_layout();
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

void TabWidget::set_window_controls_visible(bool visible)
{
    m_minimize_window_button->setVisible(visible);
    m_maximize_window_button->setVisible(visible);
    m_close_window_button->setVisible(visible);
    update_tab_layout();
}

bool TabWidget::event(QEvent* event)
{
    if (auto type = event->type(); type == QEvent::MouseButtonRelease) {
        auto const* mouse_event = static_cast<QMouseEvent const*>(event);

        if (mouse_event->button() == Qt::MiddleButton) {
            if (auto index = m_tab_bar->tabAt(mouse_event->pos()); index != -1) {
                emit tab_close_requested(index);
                return true;
            }
        }
    } else if (type == QEvent::PaletteChange) {
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
    if (watched == m_tab_bar_row) {
        auto is_empty_tab_strip_area = [this](QMouseEvent const& mouse_event) {
            return m_tab_bar_row->childAt(mouse_event.pos()) == nullptr;
        };

        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && is_empty_tab_strip_area(*mouse_event)) {
                toggle_window_maximized();
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && is_empty_tab_strip_area(*mouse_event) && start_window_move())
                return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void TabWidget::accept_tab_drag(QDragMoveEvent* event)
{
    if (!s_active_tab_drag_source || !event->mimeData()->hasFormat(LADYBIRD_TAB_MIME_TYPE)) {
        event->ignore();
        return;
    }

    auto position_in_tab_bar_row = m_tab_bar_row->mapFrom(this, event->position().toPoint());
    if (!m_tab_bar_row->rect().contains(position_in_tab_bar_row)) {
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

void TabWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update_tab_layout();
}

void TabWidget::update_tab_layout()
{
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

void TabWidget::recreate_icons()
{
    m_new_tab_button->setIcon(create_chrome_icon(ChromeIcon::NewTab, palette()));
    update_window_button_icons();
}

void TabWidget::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    m_tab_bar_row->setStyleSheet(ChromeStyle::tab_widget_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

void TabWidget::update_window_button_icons()
{
    auto is_maximized = window()->isMaximized();
    m_minimize_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowMinimize, palette()));
    m_maximize_window_button->setIcon(create_chrome_icon(is_maximized ? ChromeIcon::WindowRestore : ChromeIcon::WindowMaximize, palette()));
    m_maximize_window_button->setToolTip(is_maximized ? "Restore" : "Maximize");
    m_close_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowClose, palette()));
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
