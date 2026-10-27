/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/ScreenshotSelectionOverlay.h>

#include <QApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QtMath>

namespace Ladybird {

static constexpr int INSTRUCTIONS_MARGIN = 16;
static constexpr int INSTRUCTIONS_TEXT_INSET = 16;
static constexpr int INSTRUCTIONS_BUTTON_INSET = 12;
static constexpr int INSTRUCTIONS_BUTTON_SPACING = 8;
static constexpr int INSTRUCTIONS_TEXT_SPACING = 24;
static constexpr int DIMMING_ALPHA = 110;
static constexpr int SCROLL_SETTLE_TIME_MS = 100;
static constexpr int MAXIMUM_SCROLL_ANIMATION_TIME_MS = 300;

// The instructions stay on one line while the tab has room for it, and wrap rather than being cut off when it does not.
class InstructionsLabel final : public QLabel {
public:
    AK_ALLOC_WITH_KMALLOC;

    InstructionsLabel(QString const& text, QWidget* parent)
        : QLabel(text, parent)
    {
        setTextFormat(Qt::PlainText);
        setWordWrap(true);
    }

    virtual QSize sizeHint() const override
    {
        auto margins = contentsMargins();
        auto width = qCeil(QFontMetricsF(font()).horizontalAdvance(text())) + margins.left() + margins.right() + 1;
        return { width, heightForWidth(width) };
    }
};

// Shows the page as it was when the selection started or the user last scrolled it, so the user selects what they saw,
// not what the page has changed to since. The rest of the frame dims around the selection.
ScreenshotSelectionOverlay::ScreenshotSelectionOverlay(QImage frame, QWidget* parent)
    : QWidget(parent)
    , m_frame(AK::move(frame))
{
    setObjectName("LadybirdScreenshotSelection");
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);

    m_scroll_settle_timer = new QTimer(this);
    m_scroll_settle_timer->setSingleShot(true);
    m_scroll_settle_timer->setInterval(SCROLL_SETTLE_TIME_MS);
    QObject::connect(m_scroll_settle_timer, &QTimer::timeout, this, [this] { cover_page(); });

    auto* instructions = new QFrame(this);
    instructions->setObjectName("ScreenshotSelectionInstructions");
    instructions->setAttribute(Qt::WA_StyledBackground);
    instructions->setCursor(Qt::OpenHandCursor);
    instructions->installEventFilter(this);
    cast_popover_shadow(*instructions);
    // The buttons keep the same distance from the top, bottom and end of the card, and the text starts a little further
    // in. It gives up its width before the buttons do in a narrow tab.
    auto* layout = new QHBoxLayout(instructions);
    layout->setContentsMargins(INSTRUCTIONS_TEXT_INSET, INSTRUCTIONS_BUTTON_INSET, INSTRUCTIONS_BUTTON_INSET, INSTRUCTIONS_BUTTON_INSET);
    layout->setSpacing(INSTRUCTIONS_BUTTON_SPACING);
    layout->addWidget(new InstructionsLabel(tr("Drag to select the part of the page that shows the problem."), instructions), 1);
    layout->addSpacing(INSTRUCTIONS_TEXT_SPACING - INSTRUCTIONS_BUTTON_SPACING);

    auto* whole_frame_button = new QPushButton(tr("Use Visible Page"), instructions);
    whole_frame_button->setObjectName("ScreenshotSelectionWholeFrameButton");
    whole_frame_button->setFocusPolicy(Qt::TabFocus);
    QObject::connect(whole_frame_button, &QPushButton::clicked, this, [this] { use_whole_frame(); });
    layout->addWidget(whole_frame_button);

    auto* skip_button = new QPushButton(tr("No Screenshot"), instructions);
    skip_button->setObjectName("ScreenshotSelectionSkipButton");
    skip_button->setFocusPolicy(Qt::TabFocus);
    QObject::connect(skip_button, &QPushButton::clicked, this, [this] { skip(); });
    layout->addWidget(skip_button);

    auto* cancel_button = new QPushButton(tr("Cancel"), instructions);
    cancel_button->setObjectName("ScreenshotSelectionCancelButton");
    cancel_button->setFocusPolicy(Qt::TabFocus);
    QObject::connect(cancel_button, &QPushButton::clicked, this, [this] { dismiss(); });
    layout->addWidget(cancel_button);

    m_buttons = { whole_frame_button, skip_button, cancel_button };

    // The macOS style lays out a button by the bezel it would draw, which sits higher in the button than this style
    // sheet's border does. Laying the buttons out by their whole rect keeps them centered in the card.
    for (auto* button : m_buttons) {
        button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
        button->setCursor(Qt::ArrowCursor);
    }

    m_instructions = instructions;
    update_chrome_style();

    if (parent)
        setGeometry(parent->rect());
}

// The card is as large as its contents, however a restyle changes them, but no wider than the overlay. It sits centered
// at the top until the user moves it, and stays within the overlay either way.
void ScreenshotSelectionOverlay::place_instructions()
{
    auto available = rect().adjusted(INSTRUCTIONS_MARGIN, INSTRUCTIONS_MARGIN, -INSTRUCTIONS_MARGIN, -INSTRUCTIONS_MARGIN);
    auto width = qMax(m_instructions->minimumSizeHint().width(), qMin(m_instructions->sizeHint().width(), available.width()));
    auto height = m_instructions->hasHeightForWidth() ? m_instructions->heightForWidth(width) : m_instructions->sizeHint().height();

    auto position = m_instructions_position.value_or(QPoint { available.left() + (available.width() - width) / 2, available.top() });
    position.setX(qMax(available.left(), qMin(position.x(), available.right() + 1 - width)));
    position.setY(qMax(available.top(), qMin(position.y(), available.bottom() + 1 - height)));
    m_instructions->setGeometry({ position, QSize { width, height } });
}

// The selection in the coordinates of this widget, which the frame is stretched across.
QRect ScreenshotSelectionOverlay::selection() const
{
    if (!m_drag_start.has_value())
        return {};
    QPoint top_left { qMin(m_drag_start->x(), m_drag_end.x()), qMin(m_drag_start->y(), m_drag_end.y()) };
    QSize size { qAbs(m_drag_end.x() - m_drag_start->x()), qAbs(m_drag_end.y() - m_drag_start->y()) };
    return QRect(top_left, size).intersected(rect());
}

void ScreenshotSelectionOverlay::select(QRect area)
{
    area = area.normalized().intersected(rect());
    if (area.width() < minimum_selection_size || area.height() < minimum_selection_size || width() <= 0 || height() <= 0)
        return;

    auto scale_x = static_cast<qreal>(m_frame.width()) / width();
    auto scale_y = static_cast<qreal>(m_frame.height()) / height();
    QRect frame_area {
        qRound(area.x() * scale_x),
        qRound(area.y() * scale_y),
        qRound(area.width() * scale_x),
        qRound(area.height() * scale_y),
    };
    finish(m_frame.copy(frame_area.intersected(m_frame.rect())));
}

void ScreenshotSelectionOverlay::use_whole_frame()
{
    finish(m_frame);
}

void ScreenshotSelectionOverlay::skip()
{
    finish({});
}

// While the page scrolls underneath, the overlay steps aside for the page to present itself as it does without one,
// rather than copying each frame the page paints. It still takes the input, so that a press starts a selection rather
// than reaching the page. Once the scrolling settles, or the user goes on selecting, the overlay takes the page's frame
// and covers it again.
void ScreenshotSelectionOverlay::page_did_scroll()
{
    if (m_drag_start.has_value())
        return;

    m_scroll_settle_timer->start();
    m_follow_frames_until.setRemainingTime(MAXIMUM_SCROLL_ANIMATION_TIME_MS);
    if (m_reveals_page)
        return;

    m_reveals_page = true;
    m_instructions->hide();
    update();
    if (on_page_visibility_change)
        on_page_visibility_change();
}

// The scrolling settles once the page stops moving, which it may go on doing for a moment after the last input, such as
// to scroll smoothly. A page that always paints, such as to play an animation, settles soon after the input anyway.
void ScreenshotSelectionOverlay::page_did_paint()
{
    if (m_reveals_page && !m_follow_frames_until.hasExpired())
        m_scroll_settle_timer->start();
}

void ScreenshotSelectionOverlay::cover_page()
{
    if (!m_reveals_page)
        return;

    m_scroll_settle_timer->stop();
    if (auto frame = on_request_page_frame(); !frame.isNull())
        m_frame = AK::move(frame);
    m_reveals_page = false;
    m_instructions->show();
    update();
    if (on_page_visibility_change)
        on_page_visibility_change();
}

void ScreenshotSelectionOverlay::dismiss()
{
    m_scroll_settle_timer->stop();
    hide();
    if (on_close)
        on_close();
}

// The overlay closes before the screenshot is handed on, which may open something that takes the focus.
void ScreenshotSelectionOverlay::finish(QImage image)
{
    dismiss();
    if (on_finished)
        on_finished(AK::move(image));
}

void ScreenshotSelectionOverlay::paintEvent(QPaintEvent*)
{
    if (m_reveals_page)
        return;

    QPainter painter(this);
    painter.drawImage(rect(), m_frame);

    auto selected = selection();
    QRegion dimmed { rect() };
    if (!selected.isEmpty())
        dimmed = dimmed.subtracted(selected);
    painter.setClipRegion(dimmed);
    painter.fillRect(rect(), QColor(0, 0, 0, DIMMING_ALPHA));
    painter.setClipping(false);

    if (!selected.isEmpty()) {
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawRect(selected.adjusted(1, 1, -1, -1));
    }
}

void ScreenshotSelectionOverlay::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    cover_page();
    m_drag_start = event->position().toPoint();
    m_drag_end = *m_drag_start;
    update();
}

void ScreenshotSelectionOverlay::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_drag_start.has_value())
        return;

    // Only the area the selection leaves or enters changes, along with the outline around it.
    auto previous_selection = selection();
    m_drag_end = event->position().toPoint();
    update(previous_selection.united(selection()).adjusted(-2, -2, 2, 2));
}

// A click without a drag selects nothing, so the user can try again.
void ScreenshotSelectionOverlay::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_drag_start.has_value())
        return;
    m_drag_end = event->position().toPoint();
    auto selected = selection();
    m_drag_start.clear();
    update();
    select(selected);
}

// The user drags the card out of the way of the part of the page they want to select, rather than starting a selection.
bool ScreenshotSelectionOverlay::eventFilter(QObject* object, QEvent* event)
{
    if (object != m_instructions)
        return QWidget::eventFilter(object, event);

    if (event->type() == QEvent::LayoutRequest)
        place_instructions();
    if (!first_is_one_of(event->type(), QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease))
        return QWidget::eventFilter(object, event);

    auto& mouse_event = static_cast<QMouseEvent&>(*event);
    auto position = mouse_event.globalPosition().toPoint();
    if (event->type() == QEvent::MouseButtonPress && mouse_event.button() == Qt::LeftButton) {
        m_instructions_grab_offset = position - m_instructions->pos();
        m_instructions->setCursor(Qt::ClosedHandCursor);
    } else if (event->type() == QEvent::MouseMove && m_instructions_grab_offset.has_value()) {
        m_instructions_position = position - *m_instructions_grab_offset;
        place_instructions();
        m_instructions_position = m_instructions->pos();
    } else if (event->type() == QEvent::MouseButtonRelease && mouse_event.button() == Qt::LeftButton) {
        m_instructions_grab_offset.clear();
        m_instructions->setCursor(Qt::OpenHandCursor);
    }
    return true;
}

void ScreenshotSelectionOverlay::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    place_instructions();
}

void ScreenshotSelectionOverlay::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        cover_page();
        // A focused button does what it says. Otherwise, the whole frame is taken.
        if (auto* button = qobject_cast<QPushButton*>(QApplication::focusWidget()); button && m_buttons.contains_slow(button))
            button->click();
        else
            use_whole_frame();
        return;
    case Qt::Key_Escape:
        dismiss();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

// Tab moves between the selection and its buttons, and stays within the overlay rather than reaching the page or the
// review behind it.
bool ScreenshotSelectionOverlay::focusNextPrevChild(bool next)
{
    cover_page();

    Vector<QWidget*> stops { this };
    for (auto* button : m_buttons)
        stops.append(button);

    auto index = stops.find_first_index(QApplication::focusWidget()).value_or(0);
    index = next ? (index + 1) % stops.size() : (index + stops.size() - 1) % stops.size();
    stops[index]->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    return true;
}

void ScreenshotSelectionOverlay::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();
    QWidget::changeEvent(event);
}

void ScreenshotSelectionOverlay::update_chrome_style()
{
    m_instructions->setStyleSheet(ChromeStyle::screenshot_selection_style_sheet(palette()));
}

}
