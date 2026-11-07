/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Motion.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WindowControlsReveal.h>

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVariantAnimation>
#include <QWindow>

namespace Ladybird {

// The pointer has to come this close to the top of the page, so that what a page has in its corner stays in reach.
static constexpr int REVEAL_ZONE_HEIGHT = 10;
// The window's outline is around the page, and a pointer thrown into the corner of the window ends up on it.
static constexpr int WINDOW_EDGE_ROOM = 2;
// The pointer can stray this far from the controls for a moment without them leaving.
static constexpr int CONCEAL_MARGIN = 16;
static constexpr int CONCEAL_DELAY_MS = 300;

WindowControlsReveal::WindowControlsReveal(QWidget& parent, QWidget& page)
    : QWidget(&parent)
    , m_page(page)
{
    setObjectName("LadybirdWindowControlsReveal");
    // OPTIMIZATION: A native child that Qt creates as it is shown stacks above whatever presents the page, so the page
    //               doesn't have to step aside, and be painted by Qt, each time the pointer passes through the corner.
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
    hide();

    m_conceal_timer = new QTimer(this);
    m_conceal_timer->setSingleShot(true);
    m_conceal_timer->setInterval(CONCEAL_DELAY_MS);
    connect(m_conceal_timer, &QTimer::timeout, this, [this] {
        // NB: The controls can slide in under a pointer at rest, which only tells the window that the pointer left it.
        if (!underMouse())
            set_revealed(false);
    });

    m_animation = new QVariantAnimation(this);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        // NB: Setting the next run's values reports them as of where the last run ended.
        if (m_animation->state() != QAbstractAnimation::Running)
            return;
        m_progress = value.toReal();
        place();
    });
}

void WindowControlsReveal::hold(QWidget& window_controls)
{
    if (m_window_controls == &window_controls)
        return;
    m_window_controls = &window_controls;
    window_controls.setParent(this);
    m_page.installEventFilter(this);
    qApp->installEventFilter(this);
}

void WindowControlsReveal::release()
{
    if (!m_window_controls)
        return;
    m_window_controls = nullptr;
    m_page.removeEventFilter(this);
    qApp->removeEventFilter(this);
    m_conceal_timer->stop();
    m_animation->stop();
    m_revealed = false;
    m_progress = 0;
    hide();
}

bool WindowControlsReveal::event_is_for_this_window(QObject* watched) const
{
    // NB: The pointer's events go to the windows first, which see it wherever the widgets under it don't track it.
    auto* event_window = qobject_cast<QWindow*>(watched);
    if (!event_window)
        return false;
    while (event_window->parent())
        event_window = event_window->parent();
    return event_window == window()->windowHandle();
}

bool WindowControlsReveal::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
        if (watched == &m_page)
            place();
        break;
    case QEvent::MouseMove:
        if (event_is_for_this_window(watched)) {
            auto const& mouse_event = *static_cast<QMouseEvent*>(event);
            pointer_moved(mouse_event.globalPosition(), mouse_event.buttons() != Qt::NoButton);
        }
        break;
    case QEvent::Leave:
        if (m_revealed && event_is_for_this_window(watched))
            m_conceal_timer->start();
        break;
    default:
        break;
    }
    return false;
}

QRect WindowControlsReveal::rect_when_revealed() const
{
    auto size = m_window_controls->sizeHint();
    auto top_right = m_page.mapTo(parentWidget(), QPoint(m_page.width(), 0));
    return { top_right - QPoint(size.width(), 0), size };
}

void WindowControlsReveal::pointer_moved(QPointF global_position, bool button_is_down)
{
    auto position = parentWidget()->mapFromGlobal(global_position).toPoint();
    auto revealed_rect = rect_when_revealed();

    if (m_revealed) {
        if (revealed_rect.adjusted(-CONCEAL_MARGIN, -WINDOW_EDGE_ROOM, WINDOW_EDGE_ROOM, CONCEAL_MARGIN).contains(position))
            m_conceal_timer->stop();
        else if (!m_conceal_timer->isActive())
            m_conceal_timer->start();
        return;
    }

    // NB: A drag that ends up in the corner, such as a text selection, isn't headed for the window controls.
    auto reveal_zone = QRect(revealed_rect.topLeft(), QSize(revealed_rect.width(), REVEAL_ZONE_HEIGHT)).adjusted(0, -WINDOW_EDGE_ROOM, WINDOW_EDGE_ROOM, 0);
    if (!button_is_down && reveal_zone.contains(position))
        set_revealed(true);
}

void WindowControlsReveal::set_revealed(bool revealed)
{
    if (m_revealed == revealed)
        return;
    m_revealed = revealed;
    m_conceal_timer->stop();
    m_animation->stop();

    if (SystemAppearance::prefers_reduced_motion()) {
        m_progress = revealed ? 1 : 0;
        place();
        return;
    }
    m_animation->setStartValue(m_progress);
    m_animation->setEndValue(revealed ? 1.0 : 0.0);
    m_animation->setDuration(revealed ? Motion::FAST : Motion::EXIT);
    m_animation->setEasingCurve(revealed ? Motion::enter_easing() : Motion::exit_easing());
    m_animation->start();
}

// The controls slide out from under the top of the window: only their bottom shows at first, at their full size.
void WindowControlsReveal::place()
{
    if (!m_window_controls)
        return;
    auto height = qRound(rect_when_revealed().height() * m_progress);
    if (height == 0) {
        hide();
        return;
    }

    auto rect = rect_when_revealed();
    setGeometry(rect.x(), rect.y(), rect.width(), height);
    m_window_controls->setGeometry(0, height - rect.height(), rect.width(), rect.height());
    m_window_controls->show();
    if (isHidden()) {
        show();
        raise();
    }
    update();
}

// NB: The corner toward the page stays square, as a native surface can't show the page through a rounded one.
void WindowControlsReveal::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    auto const& chrome = Theme::chrome(*this);
    painter.fillRect(rect(), chrome.sidebar);
    painter.fillRect(0, 0, 1, height(), chrome.divider);
    painter.fillRect(0, height() - 1, width(), 1, chrome.divider);
}

}
