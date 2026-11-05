/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/TypeCasts.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/PlainText.h>
#include <UI/Qt/TabBar.h>
#include <UI/Qt/Theme.h>

#include <QAbstractItemView>
#include <QBasicTimer>
#include <QEvent>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QStyle>
#include <QStyleHintReturnMask>
#include <QStyleOption>
#include <QTextLayout>
#include <QTimerEvent>
#include <QToolTip>
#include <QWidget>
#include <QtMath>

// NB: These are only there for the platforms that link Qt::GuiPrivate.
#if __has_include(<qpa/qplatformcursor.h>)
#    define LADYBIRD_QT_HAS_PLATFORM_CURSOR
#    include <QtGui/private/qhighdpiscaling_p.h>
#    include <qpa/qplatformcursor.h>
#    include <qpa/qplatformscreen.h>
#endif

namespace Ladybird {

// A page's tooltip can be as long as it likes, so this is what's laid out of it, whatever its length.
static constexpr qsizetype MAXIMUM_TEXT_LENGTH = 2048;
static constexpr int MAXIMUM_LINE_COUNT = 24;
static constexpr int MAXIMUM_TEXT_WIDTH = 480;

static constexpr int HORIZONTAL_TEXT_MARGIN = 6;
static constexpr int VERTICAL_TEXT_MARGIN = 3;

static constexpr int HIDE_DELAY_MS = 300;

static constexpr QChar ELLIPSIS { 0x2026 };

// How far from the point of interest a tooltip starts, to clear the mouse cursor, where its size isn't known.
static constexpr QPoint FALLBACK_CURSOR_OFFSET { 2, 24 };

static QString with_length_at_most(QString const& text, qsizetype length)
{
    if (length > 0 && length < text.length() && text[length].isLowSurrogate())
        --length;
    return text.left(length);
}

static QString tooltip_text_for_layout(QString const& text)
{
    auto result = text.length() > MAXIMUM_TEXT_LENGTH ? with_length_at_most(text, MAXIMUM_TEXT_LENGTH) + ELLIPSIS : text;
    result.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    result.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    // NB: A QTextLayout is a single paragraph, so that's what its line breaks have to be.
    result.replace(QLatin1Char('\n'), QChar::LineSeparator);
    return result;
}

static QScreen* screen_at(QPoint global_position, QWidget const& widget)
{
    auto* guess = widget.screen();
    auto* exact = guess->virtualSiblingAt(global_position);
    return exact ? exact : guess;
}

static QPoint cursor_offset(QScreen const& screen, int tooltip_height)
{
#if defined(LADYBIRD_QT_HAS_PLATFORM_CURSOR)
    if (auto const* platform_screen = screen.handle()) {
        auto const* cursor = platform_screen->cursor();
        auto cursor_size = QHighDpi::fromNativePixels(cursor ? cursor->size() : QSize(16, 16), platform_screen);
        // NB: A very large cursor would push the tooltip far away, so it moves to the side of it instead.
        if (cursor_size.height() > 2 * tooltip_height)
            return { cursor_size.width() / 2, 0 };
        return { 2, cursor_size.height() };
    }
#else
    (void)screen;
    (void)tooltip_height;
#endif
    return FALLBACK_CURSOR_OFFSET;
}

class PlainTextToolTipWindow final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    // NB: A window with a parent on Windows would raise that parent as it shows.
#if defined(Q_OS_WINDOWS)
    explicit PlainTextToolTipWindow(QWidget&)
        : QWidget(nullptr, Qt::ToolTip | Qt::BypassGraphicsProxyWidget)
#else
    explicit PlainTextToolTipWindow(QWidget& parent)
        : QWidget(&parent, Qt::ToolTip | Qt::BypassGraphicsProxyWidget)
#endif
    {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFont(QToolTip::font());
        setMouseTracking(true);
        ensurePolished();
        setWindowOpacity(style()->styleHint(QStyle::SH_ToolTipLabel_Opacity, nullptr, this) / 255.0);
    }

    QWidget const* widget() const { return m_widget; }

    bool shows(QString const& text, QPoint local_position) const
    {
        return m_source_text == text && (m_rect.isNull() || m_rect.contains(local_position));
    }

    void set_content(QString const& text, QPoint global_position, QWidget& widget, QRect rect, int display_time_ms)
    {
        m_source_text = text;
        m_widget = &widget;
        m_rect = rect;

        auto* screen = screen_at(global_position, widget);
        setScreen(screen);
        auto size = lay_out_text(tooltip_text_for_layout(text), *screen);
        setAccessibleName(m_layout.text());

        auto display_time = display_time_ms > 0 ? display_time_ms : 10000 + 40 * max(0, static_cast<int>(m_layout.text().length()) - 100);
        m_expire_timer.start(display_time, this);
        m_hide_timer.stop();

        // NB: This window can already be on screen with other text, so it changes in one step, and repaints even if
        //     its size stays the same.
        setGeometry({ position_for(global_position, size, *screen), size });
        update();
    }

    void hide_soon()
    {
        if (!m_hide_timer.isActive())
            m_hide_timer.start(HIDE_DELAY_MS, this);
    }

    void cancel_hide()
    {
        m_hide_timer.stop();
    }

    void hide_now()
    {
        close();
        deleteLater();
    }

    void handle_application_event(QObject* object, QEvent* event)
    {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            // Anything but keys such as the modifiers and caps lock.
            auto key = static_cast<QKeyEvent*>(event)->key();
            if (key < Qt::Key_Shift || key > Qt::Key_ScrollLock)
                hide_now();
            break;
        }
        case QEvent::Leave:
            hide_soon();
            break;
        case QEvent::WindowActivate:
        case QEvent::WindowDeactivate:
        case QEvent::FocusIn:
        case QEvent::FocusOut:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
            hide_now();
            break;
        case QEvent::MouseMove:
            if (object == m_widget && !m_rect.isNull() && !m_rect.contains(static_cast<QMouseEvent*>(event)->position().toPoint()))
                hide_soon();
            break;
        default:
            break;
        }
    }

private:
    int perform_layout(QString const& text, int text_width)
    {
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

        m_layout.setFont(font());
        m_layout.setText(text);
        m_layout.setTextOption(option);
        m_layout.setCacheEnabled(true);

        qreal height = 0;
        qreal natural_width = 0;
        m_layout.beginLayout();
        while (m_layout.lineCount() < MAXIMUM_LINE_COUNT) {
            auto line = m_layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(text_width);
            line.setPosition({ 0, height });
            height += line.height();
            natural_width = max(natural_width, line.naturalTextWidth());
        }
        m_layout.endLayout();

        m_text_size = { qCeil(natural_width), qCeil(height) };
        auto last_line = m_layout.lineAt(m_layout.lineCount() - 1);
        return last_line.textStart() + last_line.textLength();
    }

    // Text that takes more lines than a tooltip has ends with an ellipsis on the last of them. An ellipsis can be wider
    // than what it takes the place of, and would then wrap to a line that isn't there, so this cuts more until it fits.
    void lay_out_with_ellipsis(QString const& text, int laid_out_length, int last_line_start, int text_width)
    {
        static constexpr int MAXIMUM_ATTEMPTS = 6;

        auto length_to_cut = 2;
        for (auto attempt = 0; attempt < MAXIMUM_ATTEMPTS; ++attempt) {
            auto shortened_text = with_length_at_most(text, max(laid_out_length - length_to_cut, last_line_start)) + ELLIPSIS;
            if (perform_layout(shortened_text, text_width) >= shortened_text.length())
                return;
            length_to_cut *= 2;
        }

        // NB: The lines before the last stay as they are, so an ellipsis fits on the last line when it's alone there.
        perform_layout(with_length_at_most(text, last_line_start) + ELLIPSIS, text_width);
    }

    QSize lay_out_text(QString const& text, QScreen const& screen)
    {
        auto text_width = min(MAXIMUM_TEXT_WIDTH, screen.geometry().width() - 4 * HORIZONTAL_TEXT_MARGIN);
        auto laid_out_length = perform_layout(text, text_width);
        if (laid_out_length < text.length())
            lay_out_with_ellipsis(text, laid_out_length, m_layout.lineAt(m_layout.lineCount() - 1).textStart(), text_width);

        return m_text_size + QSize(2 * HORIZONTAL_TEXT_MARGIN, 2 * VERTICAL_TEXT_MARGIN);
    }

    static QPoint position_for(QPoint global_position, QSize size, QScreen const& screen)
    {
        auto position = global_position + cursor_offset(screen, size.height());

        auto screen_rect = screen.geometry();
        if (position.x() + size.width() > screen_rect.x() + screen_rect.width())
            position.rx() -= 4 + size.width();
        if (position.y() + size.height() > screen_rect.y() + screen_rect.height())
            position.ry() -= 24 + size.height();
        position.setY(clamp(position.y(), screen_rect.y(), max(screen_rect.y(), screen_rect.y() + screen_rect.height() - size.height())));
        position.setX(clamp(position.x(), screen_rect.x(), max(screen_rect.x(), screen_rect.x() + screen_rect.width() - size.width())));
        return position;
    }

    virtual void paintEvent(QPaintEvent*) override
    {
        // NB: The palette of QToolTip doesn't follow the appearance on all platforms. On macOS it has black text on a
        //     translucent fill in dark mode, so this window looks like our other panels instead.
        auto const& panel = Theme::panel();
        QPainter painter(this);
        painter.fillRect(rect(), panel.surface);
        painter.setPen(ChromeStyle::opaque_panel_edge());
        painter.drawRect(rect().adjusted(0, 0, -1, -1));

        painter.setPen(panel.text);
        m_layout.draw(&painter, QPointF(HORIZONTAL_TEXT_MARGIN, VERTICAL_TEXT_MARGIN));
    }

    virtual void resizeEvent(QResizeEvent*) override
    {
        QStyleHintReturnMask frame_mask;
        QStyleOption option;
        option.initFrom(this);
        if (style()->styleHint(QStyle::SH_ToolTip_Mask, &option, this, &frame_mask))
            setMask(frame_mask.region);
    }

    virtual void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_rect.isNull()) {
            auto position = event->globalPosition();
            if (m_widget)
                position = m_widget->mapFromGlobal(position);
            if (!m_rect.contains(position.toPoint()))
                hide_soon();
        }
    }

    virtual void timerEvent(QTimerEvent* event) override
    {
        if (event->timerId() == m_hide_timer.timerId() || event->timerId() == m_expire_timer.timerId()) {
            m_hide_timer.stop();
            m_expire_timer.stop();
            hide_now();
        }
    }

    QString m_source_text;
    QPointer<QWidget> m_widget;
    QRect m_rect;
    QTextLayout m_layout;
    QSize m_text_size;
    QBasicTimer m_hide_timer;
    QBasicTimer m_expire_timer;
};

static QPointer<PlainTextToolTipWindow> s_tooltip_window;

void show_tooltip(QPoint global_position, QString const& text, QWidget& widget, QRect rect, int display_time_ms)
{
    if (text.isEmpty()) {
        hide_tooltip();
        return;
    }

    // NB: A window that was closed is only waiting to be deleted, and it isn't visible, so it's never used again.
    if (s_tooltip_window && s_tooltip_window->isVisible() && s_tooltip_window->widget() == &widget) {
        if (s_tooltip_window->shows(text, widget.mapFromGlobal(global_position)))
            s_tooltip_window->cancel_hide();
        else
            s_tooltip_window->set_content(text, global_position, widget, rect, display_time_ms);
        return;
    }

    if (s_tooltip_window)
        s_tooltip_window->hide_now();

    s_tooltip_window = new PlainTextToolTipWindow(widget);
    s_tooltip_window->set_content(text, global_position, widget, rect, display_time_ms);
    s_tooltip_window->show();
}

void hide_tooltip()
{
    if (s_tooltip_window)
        s_tooltip_window->hide_soon();
}

static Optional<ToolTipContent> tooltip_at(QWidget& widget, QPoint position)
{
    if (auto* tab_bar = as_if<TabBar>(&widget)) {
        if (auto content = tab_bar->tooltip_at(position); content.has_value())
            return content;
    }

    // NB: Item views handle tooltips for their viewport, whose events go to the view by way of an event filter.
    if (auto* view = as_if<QAbstractItemView>(widget.parentWidget()); view && view->viewport() == &widget) {
        auto index = view->indexAt(position);
        if (auto text = index.data(Qt::ToolTipRole).toString(); !text.isEmpty())
            return ToolTipContent { .text = move(text), .rect = view->visualRect(index) };
    }

    // NB: A button with a default action has that action's tooltip here, or else the text of the action.
    if (auto text = widget.toolTip(); !text.isEmpty())
        return ToolTipContent { .text = move(text), .rect = {} };

    return {};
}

bool PlainTextEventFilter::eventFilter(QObject* object, QEvent* event)
{
    // NB: QLabel parses its text when it is first laid out or painted, and both polish the label first. This also
    //     covers the labels inside a QMessageBox, and the label of QToolTip, were anything to still reach it.
    if (event->type() == QEvent::Polish) {
        if (auto* label = as_if<QLabel>(object); label && label->textFormat() == Qt::AutoText)
            label->setTextFormat(Qt::PlainText);
        return false;
    }

    if (event->type() == QEvent::ToolTip) {
        auto* widget = as_if<QWidget>(object);
        if (!widget)
            return false;

        // NB: QApplication offers an ignored event to the parent of the widget, which also comes through here. So
        //     this takes every one of these events, and no widget can pass its tooltip to QToolTip.
        auto const& help_event = *static_cast<QHelpEvent*>(event);
        if (auto content = tooltip_at(*widget, help_event.pos()); content.has_value()) {
            if (!content->text.isEmpty())
                show_tooltip(help_event.globalPos(), content->text, *widget, content->rect, widget->toolTipDuration());
            event->accept();
        } else {
            event->ignore();
        }
        return true;
    }

    if (s_tooltip_window)
        s_tooltip_window->handle_application_event(object, event);
    return false;
}

}
