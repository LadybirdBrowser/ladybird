/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/ScreenshotViewer.h>

#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegion>
#include <QWheelEvent>

namespace Ladybird {

static constexpr int SIDE_MARGIN = 32;
static constexpr int CHROME_MARGIN = 64;
static constexpr int CHROME_INSET = 16;
static constexpr qreal CORNER_RADIUS = 6;
static constexpr int SHADOW_SIZE = 16;
static constexpr int BACKDROP_ALPHA = 200;
static constexpr qreal DRAG_THRESHOLD = 4;

// Shows a screenshot over the whole tab: at its actual size, one pixel of the screenshot to one pixel of the screen,
// when the tab has room for it, and otherwise fitted to the tab, with a click to look at it in its actual size.
ScreenshotViewer::ScreenshotViewer(QImage screenshot, QWidget* parent)
    : QWidget(parent)
    , m_screenshot(AK::move(screenshot))
{
    setObjectName("LadybirdScreenshotViewer");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setStyleSheet(ChromeStyle::screenshot_viewer_style_sheet());

    m_close_button = new QPushButton(tr("Close"), this);
    m_close_button->setObjectName("ScreenshotViewerCloseButton");
    m_close_button->setFocusPolicy(Qt::NoFocus);
    m_close_button->setCursor(Qt::ArrowCursor);
    QObject::connect(m_close_button, &QPushButton::clicked, this, [this] { close_viewer(); });

    m_caption = new QLabel(this);
    m_caption->setObjectName("ScreenshotViewerCaption");
    m_caption->setAlignment(Qt::AlignCenter);
    m_caption->setAttribute(Qt::WA_TransparentForMouseEvents);

    if (parent)
        setGeometry(parent->rect());
    update_caption();
}

// The screenshot is in device pixels, so its actual size on this screen is that many device pixels.
QSizeF ScreenshotViewer::actual_size() const
{
    return QSizeF(m_screenshot.size()) / devicePixelRatioF();
}

QRectF ScreenshotViewer::available_rect() const
{
    return QRectF(rect()).adjusted(SIDE_MARGIN, CHROME_MARGIN, -SIDE_MARGIN, -CHROME_MARGIN);
}

QRectF ScreenshotViewer::centered_rect(QSizeF size) const
{
    auto center = available_rect().center();
    return QRectF(center.x() - size.width() / 2, center.y() - size.height() / 2, size.width(), size.height());
}

bool ScreenshotViewer::fits_at_actual_size() const
{
    auto available = available_rect().size();
    auto size = actual_size();
    return size.width() <= available.width() && size.height() <= available.height();
}

QRectF ScreenshotViewer::image_rect() const
{
    if (m_screenshot.isNull())
        return {};
    if (m_is_at_actual_size || fits_at_actual_size())
        return centered_rect(actual_size()).translated(m_offset);
    return centered_rect(actual_size().scaled(available_rect().size(), Qt::KeepAspectRatio));
}

// Looking at the screenshot in its actual size keeps the point under the cursor where it is, so a click zooms into
// what the user clicked on.
void ScreenshotViewer::set_actual_size(bool actual_size, Optional<QPointF> anchor)
{
    if (actual_size && fits_at_actual_size())
        actual_size = false;
    if (actual_size == m_is_at_actual_size)
        return;

    auto fitted = image_rect();
    m_is_at_actual_size = actual_size;
    m_offset = {};

    if (actual_size && anchor.has_value() && !fitted.isEmpty()) {
        QPointF relative { (anchor->x() - fitted.left()) / fitted.width(), (anchor->y() - fitted.top()) / fitted.height() };
        auto size = this->actual_size();
        auto centered = centered_rect(size);
        QPointF top_left { anchor->x() - relative.x() * size.width(), anchor->y() - relative.y() * size.height() };
        move_image_by(top_left - centered.topLeft());
    }

    update_caption();
    update_cursor(anchor.value_or(QPointF {}));
    update();
}

// The image always covers the tab along a direction in which it is larger than the tab, and stays centered along one
// in which it is not.
void ScreenshotViewer::move_image_by(QPointF delta)
{
    m_offset += delta;
    auto centered = centered_rect(actual_size());
    auto clamp_axis = [](qreal offset, qreal start, qreal length, qreal extent) {
        if (length <= extent)
            return 0.;
        auto minimum = extent - length - start;
        auto maximum = -start;
        return clamp(offset, minimum, maximum);
    };
    m_offset.setX(clamp_axis(m_offset.x(), centered.left(), centered.width(), width()));
    m_offset.setY(clamp_axis(m_offset.y(), centered.top(), centered.height(), height()));
}

void ScreenshotViewer::close_viewer()
{
    hide();
    if (on_closed)
        on_closed();
}

QPixmap const& ScreenshotViewer::pixmap_for(QSizeF size)
{
    auto device_size = (size * devicePixelRatioF()).toSize();
    if (m_pixmap.isNull() || m_pixmap.size() != device_size) {
        auto image = device_size == m_screenshot.size() ? m_screenshot : m_screenshot.scaled(device_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        m_pixmap = QPixmap::fromImage(image);
        m_pixmap.setDevicePixelRatio(devicePixelRatioF());
    }
    return m_pixmap;
}

void ScreenshotViewer::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 0, 0, BACKDROP_ALPHA));

    auto target = image_rect();
    if (target.isEmpty())
        return;

    // A soft shadow lifts the screenshot off the page it was taken from. It is only painted around the screenshot, which
    // covers the rest of it.
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 6));
    if (auto around = QRegion(rect()).subtracted(target.toAlignedRect()); !around.isEmpty()) {
        painter.save();
        painter.setClipRegion(around);
        for (int step = SHADOW_SIZE; step > 0; --step)
            painter.drawRoundedRect(target.adjusted(-step, -step + 4, step, step + 4), CORNER_RADIUS + step, CORNER_RADIUS + step);
        painter.restore();
    }

    QPainterPath corners;
    corners.addRoundedRect(target, CORNER_RADIUS, CORNER_RADIUS);
    painter.save();
    painter.setClipPath(corners);
    painter.drawPixmap(target.topLeft(), pixmap_for(target.size()));
    painter.restore();

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 48), 1));
    painter.drawRoundedRect(target.adjusted(0.5, 0.5, -0.5, -0.5), CORNER_RADIUS, CORNER_RADIUS);
}

void ScreenshotViewer::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_press_position = event->position();
    m_press_offset = m_offset;
    m_is_dragging = false;
    update_cursor(event->position());
}

void ScreenshotViewer::mouseMoveEvent(QMouseEvent* event)
{
    if (m_press_position.has_value() && m_is_at_actual_size) {
        auto delta = event->position() - *m_press_position;
        if (delta.manhattanLength() > DRAG_THRESHOLD)
            m_is_dragging = true;
        if (m_is_dragging) {
            m_offset = m_press_offset;
            move_image_by(delta);
            update();
        }
    }
    update_cursor(event->position());
}

// A click on the screenshot switches between seeing all of it and seeing it in its actual size, and a click beside it
// closes the viewer.
void ScreenshotViewer::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_press_position.has_value())
        return;
    m_press_position.clear();
    if (exchange(m_is_dragging, false)) {
        update_cursor(event->position());
        return;
    }

    if (!image_rect().contains(event->position())) {
        close_viewer();
        return;
    }
    set_actual_size(!m_is_at_actual_size, event->position());
}

void ScreenshotViewer::wheelEvent(QWheelEvent* event)
{
    if (!m_is_at_actual_size)
        return;
    auto delta = event->pixelDelta().isNull() ? QPointF(event->angleDelta()) / 2 : QPointF(event->pixelDelta());
    move_image_by(delta);
    update();
}

void ScreenshotViewer::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        close_viewer();
        return;
    case Qt::Key_Space:
        set_actual_size(!m_is_at_actual_size);
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

void ScreenshotViewer::resizeEvent(QResizeEvent*)
{
    if (m_is_at_actual_size && fits_at_actual_size())
        m_is_at_actual_size = false;
    move_image_by({});

    auto close_size = m_close_button->sizeHint();
    m_close_button->setGeometry(width() - CHROME_INSET - close_size.width(), CHROME_INSET, close_size.width(), close_size.height());
    update_caption();
}

void ScreenshotViewer::update_cursor(QPointF position)
{
    auto cursor = Qt::ArrowCursor;
    if (image_rect().contains(position)) {
        if (m_is_at_actual_size)
            cursor = m_is_dragging ? Qt::ClosedHandCursor : Qt::OpenHandCursor;
        else if (!fits_at_actual_size())
            cursor = Qt::PointingHandCursor;
    }
    setCursor(cursor);
}

void ScreenshotViewer::update_caption()
{
    if (m_is_at_actual_size)
        m_caption->setText(tr("Actual size. Drag to look around, click to see all of it."));
    else if (!fits_at_actual_size())
        m_caption->setText(tr("Click to see it in actual size."));
    else
        m_caption->setText(tr("Actual size"));

    auto caption_size = m_caption->sizeHint().boundedTo(QSize(width() - CHROME_INSET * 2, height()));
    m_caption->setGeometry((width() - caption_size.width()) / 2, height() - CHROME_INSET - caption_size.height(), caption_size.width(), caption_size.height());
}

}
