/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/TabBar.h>

#include <QContextMenuEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

namespace Ladybird {

static QPainterPath active_tab_path(QRectF const& rect, qreal radius)
{
    QPainterPath path;
    path.moveTo(rect.left(), rect.bottom());
    path.lineTo(rect.left(), rect.top() + radius);
    path.quadTo(rect.left(), rect.top(), rect.left() + radius, rect.top());
    path.lineTo(rect.right() - radius, rect.top());
    path.quadTo(rect.right(), rect.top(), rect.right(), rect.top() + radius);
    path.lineTo(rect.right(), rect.bottom());
    path.closeSubpath();
    return path;
}

TabBar::TabBar(TabWidget* tab_widget)
    : QTabBar(tab_widget)
    , m_tab_widget(tab_widget)
{
    setMouseTracking(true);
    setMinimumHeight(38);
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

    hint.setHeight(34);
    return hint;
}

void TabBar::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), ChromeStyle::chrome_background(palette()));

    auto border = ChromeStyle::chrome_border(palette());
    auto accent = ChromeStyle::chrome_accent(palette());
    auto text_color = palette().color(QPalette::Text);
    auto muted_text_color = ChromeStyle::chrome_muted_text(palette());

    for (int index = 0; index < count(); ++index) {
        auto tab_rect = tabRect(index);
        if (!tab_rect.isValid() || tab_rect.right() < 0 || tab_rect.left() > width())
            continue;

        bool is_selected = index == currentIndex();
        bool is_hovered = index == m_hovered_tab_index;

        if (is_selected || is_hovered) {
            auto shape_rect = QRectF(tab_rect).adjusted(2.0, is_selected ? 4.0 : 7.0, -2.0, is_selected ? 0.0 : -6.0);
            auto surface = is_selected ? ChromeStyle::chrome_surface(palette()) : ChromeStyle::chrome_surface_hover(palette());

            auto tab_path = is_selected
                ? active_tab_path(shape_rect, 7.0)
                : [&] {
                      QPainterPath path;
                      path.addRoundedRect(shape_rect, 7.0, 7.0);
                      return path;
                  }();

            painter.setBrush(surface);
            painter.setPen(is_selected ? QPen(border, 1) : Qt::NoPen);
            painter.drawPath(tab_path);

            if (is_selected) {
                painter.setPen(QPen(accent, 2));
                painter.drawLine(QPointF(shape_rect.left() + 9.0, shape_rect.top() + 1.0),
                    QPointF(shape_rect.right() - 9.0, shape_rect.top() + 1.0));
            }
        } else if (index > 0 && index != currentIndex() + 1) {
            auto separator = border;
            separator.setAlpha(110);
            painter.setPen(separator);
            painter.drawLine(tab_rect.topLeft() + QPoint(0, 11), tab_rect.bottomLeft() - QPoint(0, 10));
        }

        auto contents_rect = tab_rect.adjusted(13, 1, -13, 0);
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
        painter.setPen(is_selected || is_hovered ? text_color : muted_text_color);
        painter.drawText(contents_rect, Qt::AlignLeft | Qt::AlignVCenter, title);
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

void TabBar::mousePressEvent(QMouseEvent* event)
{
    event->ignore();

    auto pressed_tab = tabAt(event->pos());
    if (pressed_tab < 0 && event->button() == Qt::LeftButton) {
        if (start_window_move())
            return;
    }

    if (pressed_tab >= 0) {
        auto rect_of_current_tab = tabRect(pressed_tab);
        m_x_position_in_selected_tab_while_dragging = event->pos().x() - rect_of_current_tab.x();
    }

    QTabBar::mousePressEvent(event);
}

void TabBar::mouseMoveEvent(QMouseEvent* event)
{
    event->ignore();

    if (auto hovered_tab = tabAt(event->pos()); hovered_tab != m_hovered_tab_index) {
        m_hovered_tab_index = hovered_tab;
        update();
    }

    if (count() == 0) {
        QTabBar::mouseMoveEvent(event);
        return;
    }

    auto rect_of_first_tab = tabRect(0);
    auto rect_of_last_tab = tabRect(count() - 1);

    auto boundary_limit_for_dragging_tab = QRect(rect_of_first_tab.x() + m_x_position_in_selected_tab_while_dragging, 0,
        rect_of_last_tab.x() + m_x_position_in_selected_tab_while_dragging, 0);

    if (event->pos().x() >= boundary_limit_for_dragging_tab.x() && event->pos().x() <= boundary_limit_for_dragging_tab.width()) {
        QTabBar::mouseMoveEvent(event);
    } else {
        auto pos = event->pos();
        if (event->pos().x() > boundary_limit_for_dragging_tab.width())
            pos.setX(boundary_limit_for_dragging_tab.width());
        else if (event->pos().x() < boundary_limit_for_dragging_tab.x())
            pos.setX(boundary_limit_for_dragging_tab.x());
        QMouseEvent ev(event->type(), pos, event->globalPosition(), event->button(), event->buttons(), event->modifiers());
        QTabBar::mouseMoveEvent(&ev);
    }
}

void TabBar::leaveEvent(QEvent* event)
{
    m_hovered_tab_index = -1;
    update();
    QTabBar::leaveEvent(event);
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

bool TabBar::start_window_move()
{
    auto* handle = window()->windowHandle();
    if (!handle)
        return false;
    return handle->startSystemMove();
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

    m_tab_bar->setDocumentMode(true);
    m_tab_bar->setElideMode(Qt::TextElideMode::ElideRight);
    m_tab_bar->setMovable(true);
    m_tab_bar->setTabsClosable(true);
    m_tab_bar->setExpanding(false);
    m_tab_bar->setUsesScrollButtons(true);
    m_tab_bar->setDrawBase(false);

    m_stacked_widget = new QStackedWidget(this);

    m_new_tab_button = new QToolButton(this);
    m_new_tab_button->setObjectName("LadybirdNewTabButton");
    m_new_tab_button->setIconSize(QSize(16, 16));
    m_new_tab_button->setToolTip("New Tab");

    m_minimize_window_button = new QToolButton(this);
    m_minimize_window_button->setObjectName("LadybirdWindowButton");
    m_minimize_window_button->setToolTip("Minimize");
    m_minimize_window_button->setIconSize(QSize(14, 14));

    m_maximize_window_button = new QToolButton(this);
    m_maximize_window_button->setObjectName("LadybirdWindowButton");
    m_maximize_window_button->setIconSize(QSize(14, 14));

    m_close_window_button = new QToolButton(this);
    m_close_window_button->setObjectName("LadybirdCloseWindowButton");
    m_close_window_button->setToolTip("Close");
    m_close_window_button->setIconSize(QSize(14, 14));

    recreate_icons();

    auto* tab_bar_row_layout = new QHBoxLayout();
    tab_bar_row_layout->setSpacing(0);
    tab_bar_row_layout->setContentsMargins(8, 0, 8, 0);
    tab_bar_row_layout->addWidget(m_tab_bar);
    tab_bar_row_layout->addWidget(m_new_tab_button);
    tab_bar_row_layout->addStretch(1);
    tab_bar_row_layout->addWidget(m_minimize_window_button);
    tab_bar_row_layout->addWidget(m_maximize_window_button);
    tab_bar_row_layout->addWidget(m_close_window_button);

    m_tab_bar_row = new QWidget(this);
    m_tab_bar_row->setObjectName("LadybirdTabStrip");
    m_tab_bar_row->setMinimumHeight(40);
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
    m_stacked_widget->addWidget(widget);
    m_tab_bar->addTab(label);

    update_tab_layout();
}

void TabWidget::remove_tab(int index)
{
    auto* widget = m_stacked_widget->widget(index);
    m_stacked_widget->removeWidget(widget);

    m_tab_bar->removeTab(index);

    if (m_tab_bar->count() > 0 && m_tab_bar->currentIndex() >= 0)
        m_stacked_widget->setCurrentIndex(m_tab_bar->currentIndex());

    update_tab_layout();
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

bool TabWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_tab_bar_row) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton) {
                toggle_window_maximized();
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() == Qt::LeftButton && start_window_move())
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

void TabWidget::update_tab_layout()
{
    auto button_width = m_new_tab_button->sizeHint().width();
    auto available_for_tabs = width() - button_width;

    m_tab_bar->set_available_width(available_for_tabs);
}

void TabWidget::recreate_icons()
{
    m_new_tab_button->setIcon(create_tvg_icon_with_theme_colors("new_tab", palette()));
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
    auto* top_level_window = window();
    auto* button_style = top_level_window->style();
    m_minimize_window_button->setIcon(button_style->standardIcon(QStyle::SP_TitleBarMinButton));
    m_maximize_window_button->setIcon(button_style->standardIcon(top_level_window->isMaximized() ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton));
    m_maximize_window_button->setToolTip(top_level_window->isMaximized() ? "Restore" : "Maximize");
    m_close_window_button->setIcon(button_style->standardIcon(QStyle::SP_TitleBarCloseButton));
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
    setFixedSize({ 24, 24 });
    setIconSize({ 14, 14 });
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
