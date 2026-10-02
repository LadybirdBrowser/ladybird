/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/PictureInPictureWindow.h>
#include <UI/Qt/WebContentView.h>
#include <UI/Qt/WindowScreenObserver.h>

#include <QApplication>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif

namespace Ladybird {

static constexpr int screen_edge_margin = 16;
static constexpr int resize_margin = 6;

static Qt::WindowFlags window_flags()
{
#if defined(AK_OS_MACOS)
    // A tool window is a panel on macOS, which never becomes the main window, so activating Ladybird brings a browser
    // window forward rather than this one.
    return Qt::Tool | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint;
#else
    return Qt::Window | Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint;
#endif
}

// A frameless window, placed in the bottom-right corner of the screen it is opened from, that floats above other windows.
// It is moved by dragging anywhere on its page, and resized from its edges.
class PictureInPictureWindow final
    : public QWidget
    , public WebView::PictureInPictureWindow {
public:
    AK_ALLOC_WITH_KMALLOC;

    PictureInPictureWindow(WebView::CanonicalTraversable& traversable, WebView::ViewImplementation const& owner_view, Gfx::IntSize video_size)
        : QWidget(nullptr, window_flags())
    {
        // The window's native window and view are created for the screen of the browser window it is opened from.
        auto* screen = static_cast<WebContentView const&>(owner_view).screen();
        setScreen(screen);

        setAttribute(Qt::WA_ShowWithoutActivating);
#if defined(AK_OS_MACOS)
        // A tool window otherwise hides whenever Ladybird is inactive. Qt's WA_MacAlwaysShowToolWindow would also make
        // it order every window to the front whenever Ladybird is activated, bringing browser windows forward too.
        keep_appkit_window_visible_while_inactive(*this);
#endif
        setWindowTitle("Picture-in-Picture");

        auto available = screen->availableGeometry();

        m_screen_observer = new WindowScreenObserver(*this);
        auto view_initial_state = WebContentViewInitialState {
            .is_private = owner_view.is_private(),
            .maximum_frames_per_second = m_screen_observer->refresh_rate(),
            .display_id = m_screen_observer->display_id(),
            .owner_view = &owner_view,
        };
        m_view = new WebContentView(this, traversable, view_initial_state);
        m_screen_observer->on_device_pixel_ratio_change = [this] {
            m_view->set_device_pixel_ratio(m_screen_observer->device_pixel_ratio());
        };
        m_screen_observer->on_display_metadata_change = [this] {
            m_view->set_display_metadata(m_screen_observer->display_id(), m_screen_observer->refresh_rate());
        };
        report_page_close_of(*m_view);
        m_view->installEventFilter(this);
        m_view->on_finish_handling_mouse_event = [this](Web::MouseEvent const& event, Web::EventResult result) {
            if (event.type == Web::MouseEvent::Type::MouseDown)
                page_did_handle_press(result);
        };

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(m_view);

        auto size = initial_size(video_size, { available.width(), available.height() });
        resize(size.width(), size.height());
        move(available.right() - size.width() - screen_edge_margin, available.bottom() - size.height() - screen_edge_margin);
        show();

#if defined(AK_OS_MACOS)
        // A frameless window cannot be resized from its edges on macOS unless AppKit is told it is resizable.
        make_appkit_window_resizable(*this);
#endif
    }

    virtual Gfx::IntSize size() const override { return { width(), height() }; }
    virtual String handle() const override { return m_view->handle(); }
    virtual void hide() override { QWidget::hide(); }

private:
    // A drag moves the window, unless the page claims the press that starts it, as the media controls do to scrub. Every
    // press reaches the page, and a drag waits until the page has handled its press rather than guess while it is busy.
    virtual bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object != m_view)
            return false;

        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if (mouse_event->button() != Qt::LeftButton)
                return false;
#if !defined(AK_OS_MACOS)
            if (auto edges = resize_edges_for(mouse_event->position().toPoint()); edges != Qt::Edges {}) {
                if (windowHandle()->startSystemResize(edges))
                    return true;
            }
#endif
            m_press = Press { .position = mouse_event->globalPosition().toPoint() };
            return false;
        }
        case QEvent::MouseMove: {
            if (!m_press.has_value())
                return false;
            if (m_press->is_moving_window)
                return true;
            if (m_press->claim != PressClaim::Window)
                return false;
            auto* mouse_event = static_cast<QMouseEvent*>(event);
            if ((mouse_event->globalPosition().toPoint() - m_press->position).manhattanLength() < QApplication::startDragDistance())
                return false;
            m_press->is_moving_window = true;
            // The window takes the press over, so the page sees it cancelled.
            m_view->cancel_mouse_press();
#if defined(AK_OS_MACOS)
            if (start_appkit_window_drag(*this))
                return true;
#endif
            windowHandle()->startSystemMove();
            return true;
        }
        case QEvent::MouseButtonRelease: {
            // The page's press was cancelled once it moved the window, so the page does not see it end.
            auto press_moved_window = m_press.has_value() && m_press->is_moving_window;
            m_press.clear();
            return press_moved_window;
        }
        case QEvent::NativeGesture:
            // The window is sized to its video, so its page is never zoomed.
            return static_cast<QNativeGestureEvent*>(event)->gestureType() == Qt::ZoomNativeGesture;
        case QEvent::Wheel:
            return static_cast<QWheelEvent*>(event)->modifiers().testFlag(Qt::ControlModifier);
        default:
            return false;
        }
    }

    void page_did_handle_press(Web::EventResult result)
    {
        if (!m_press.has_value() || m_press->claim != PressClaim::Unknown)
            return;
        // A press that runs its default actions is handled too, so only one the page cancelled is claimed.
        m_press->claim = result == Web::EventResult::Cancelled ? PressClaim::Page : PressClaim::Window;
    }

    Qt::Edges resize_edges_for(QPoint position) const
    {
        Qt::Edges edges;
        if (position.x() < resize_margin)
            edges |= Qt::LeftEdge;
        if (position.x() >= width() - resize_margin)
            edges |= Qt::RightEdge;
        if (position.y() < resize_margin)
            edges |= Qt::TopEdge;
        if (position.y() >= height() - resize_margin)
            edges |= Qt::BottomEdge;
        return edges;
    }

    virtual void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        if (on_resize)
            on_resize(size());
    }

    virtual void closeEvent(QCloseEvent* event) override
    {
        // Closing only hides the window, which stays until its page has closed, as the manager asks for once the page's
        // video has exited.
        event->accept();
        if (on_close)
            on_close();
    }

    WebContentView* m_view { nullptr };
    WindowScreenObserver* m_screen_observer { nullptr };

    enum class PressClaim : u8 {
        Unknown,
        Page,
        Window,
    };
    struct Press {
        QPoint position;
        PressClaim claim { PressClaim::Unknown };
        bool is_moving_window { false };
    };
    Optional<Press> m_press;
};

NonnullOwnPtr<WebView::PictureInPictureWindow> create_picture_in_picture_window(WebView::CanonicalTraversable& traversable, WebView::ViewImplementation const& owner_view, Gfx::IntSize video_size)
{
    return make<PictureInPictureWindow>(traversable, owner_view, video_size);
}

}
