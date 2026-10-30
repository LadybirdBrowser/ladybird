/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/ByteString.h>
#include <AK/Math.h>
#include <AK/Vector.h>
#include <LibGfx/Size.h>
#include <UI/Qt/X11Window.h>

#include <QBitmap>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QPointer>
#include <QRegion>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <xcb/xcb_icccm.h>

namespace Ladybird {

static xcb_connection_t* x11_connection()
{
    auto* x11_application = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11_application)
        return nullptr;
    return x11_application->connection();
}

void set_x11_window_aspect_ratio(QWidget& widget, Gfx::IntSize aspect_ratio)
{
    auto* connection = x11_connection();
    if (!connection)
        return;

    auto window = static_cast<xcb_window_t>(widget.winId());

    // Qt writes all of the window's size hints whenever it changes the window's geometry, so the aspect ratio joins the
    // hints it last wrote, and has to be set again after any such change.
    xcb_size_hints_t hints {};
    xcb_icccm_get_wm_normal_hints_reply(connection, xcb_icccm_get_wm_normal_hints(connection, window), &hints, nullptr);
    xcb_icccm_size_hints_set_aspect(&hints, aspect_ratio.width(), aspect_ratio.height(), aspect_ratio.width(), aspect_ratio.height());
    xcb_icccm_set_wm_normal_hints(connection, window, &hints);
    xcb_flush(connection);
}

// A compositing manager owns the _NET_WM_CM_S<n> selection of each screen it composites.
static Vector<xcb_atom_t> const& compositing_manager_selections(xcb_connection_t* connection)
{
    static auto const selections = [connection] {
        Vector<xcb_intern_atom_cookie_t> cookies;
        auto screen_count = xcb_setup_roots_length(xcb_get_setup(connection));
        for (int screen = 0; screen < screen_count; ++screen) {
            auto name = ByteString::formatted("_NET_WM_CM_S{}", screen);
            cookies.append(xcb_intern_atom(connection, false, name.length(), name.characters()));
        }

        Vector<xcb_atom_t> atoms;
        for (auto cookie : cookies) {
            auto* reply = xcb_intern_atom_reply(connection, cookie, nullptr);
            if (!reply)
                continue;
            atoms.append(reply->atom);
            free(reply);
        }
        return atoms;
    }();
    return selections;
}

bool windows_blend_with_desktop()
{
    auto* connection = x11_connection();
    if (!connection)
        return true;

    // NB: A compositing manager can start or stop at any time, so this asks the display each time.
    Vector<xcb_get_selection_owner_cookie_t> cookies;
    for (auto selection : compositing_manager_selections(connection))
        cookies.append(xcb_get_selection_owner(connection, selection));

    bool has_compositing_manager = false;
    for (auto cookie : cookies) {
        auto* reply = xcb_get_selection_owner_reply(connection, cookie, nullptr);
        if (!reply)
            continue;
        if (reply->owner != XCB_WINDOW_NONE)
            has_compositing_manager = true;
        free(reply);
    }
    return has_compositing_manager;
}

static constexpr char const* CLIPPED_TO_ITS_OPAQUE_PIXELS = "ladybird_clipped_to_its_opaque_pixels";

bool is_clipped_to_its_opaque_pixels(QWidget const& window)
{
    return window.property(CLIPPED_TO_ITS_OPAQUE_PIXELS).toBool();
}

// NB: A compositing manager blends top-level windows only. A window embedded in another one, such as the suggestions
//     under the address field, never blends with what lies beneath it.
static bool window_blends_with_what_lies_beneath(QWidget& window)
{
    if (window.windowHandle() && window.windowHandle()->parent())
        return false;
    return windows_blend_with_desktop();
}

static void clip_translucent_window(QWidget& window)
{
    if (window_blends_with_what_lies_beneath(window)) {
        if (is_clipped_to_its_opaque_pixels(window)) {
            window.clearMask();
            window.setProperty(CLIPPED_TO_ITS_OPAQUE_PIXELS, false);
            window.update();
        }
        return;
    }

    if (window.size().isEmpty())
        return;

    // NB: The window paints differently once clipped, such as with opaque edges, so it is marked before its picture
    //     is taken.
    if (!is_clipped_to_its_opaque_pixels(window)) {
        window.setProperty(CLIPPED_TO_ITS_OPAQUE_PIXELS, true);
        window.update();
    }

    // NB: The window paints no background of its own, so whatever stays clear in the picture is clear in the window. A
    //     picture of a window leaves out what lies outside its mask, so the mask it has grown out of goes first.
    window.clearMask();
    auto device_pixel_ratio = window.devicePixelRatioF();
    QImage picture(window.size() * device_pixel_ratio, QImage::Format_ARGB32_Premultiplied);
    picture.setDevicePixelRatio(device_pixel_ratio);
    picture.fill(Qt::transparent);
    window.render(&picture, {}, {}, QWidget::DrawChildren);

    // NB: A pixel that is partly clear shows darkened, so only the opaque pixels stay. That leaves out shadows and the
    //     antialiased edges of rounded corners. The mask is in logical pixels, so one stays only if all of the device
    //     pixels it covers are opaque.
    struct DevicePixels {
        int start { 0 };
        int end { 0 };
    };
    auto device_pixels_covered = [&](int logical_pixel, int device_size) {
        return DevicePixels {
            .start = static_cast<int>(AK::floor(logical_pixel * device_pixel_ratio)),
            .end = min(static_cast<int>(AK::ceil((logical_pixel + 1) * device_pixel_ratio)), device_size),
        };
    };
    QImage opaque_pixels(window.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < window.height(); ++y) {
        auto rows = device_pixels_covered(y, picture.height());
        auto* mask_pixels = reinterpret_cast<QRgb*>(opaque_pixels.scanLine(y));
        for (int x = 0; x < window.width(); ++x) {
            auto columns = device_pixels_covered(x, picture.width());
            bool is_opaque = rows.start < rows.end && columns.start < columns.end;
            for (int device_y = rows.start; is_opaque && device_y < rows.end; ++device_y) {
                auto const* pixels = reinterpret_cast<QRgb const*>(picture.constScanLine(device_y));
                for (int device_x = columns.start; is_opaque && device_x < columns.end; ++device_x)
                    is_opaque = qAlpha(pixels[device_x]) == 255;
            }
            mask_pixels[x] = is_opaque ? 0xff000000 : 0;
        }
    }
    QRegion opaque_region { QBitmap::fromImage(opaque_pixels.createAlphaMask()) };

    // NB: QWidget takes an empty mask to mean no mask at all. A window that paints nothing opaque, such as one that is
    //     only a shadow, gets a mask that lies wholly outside it instead.
    if (opaque_region.isEmpty())
        opaque_region = QRect { window.width(), window.height(), 1, 1 };

    window.setMask(opaque_region);
}

class TranslucentWindowClipper final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit TranslucentWindowClipper(QObject* parent)
        : QObject(parent)
    {
    }

private:
    virtual bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() != QEvent::Show && event->type() != QEvent::Resize)
            return false;
        if (!object->isWidgetType())
            return false;

        auto& widget = static_cast<QWidget&>(*object);
        if (!widget.isWindow() || !widget.testAttribute(Qt::WA_TranslucentBackground))
            return false;

        // NB: A window is clipped as it shows, so that it never appears unclipped, and again once its contents have
        //     been laid out for its new size.
        if (event->type() == QEvent::Show)
            clip_translucent_window(widget);
        if (any_of(m_windows_to_clip, [&](auto const& window) { return window == &widget; }))
            return false;
        m_windows_to_clip.append(&widget);
        if (m_windows_to_clip.size() == 1) {
            QTimer::singleShot(0, this, [this] {
                auto windows = move(m_windows_to_clip);
                for (auto& window : windows) {
                    if (window && window->isVisible())
                        clip_translucent_window(*window);
                }
            });
        }
        return false;
    }

    Vector<QPointer<QWidget>> m_windows_to_clip;
};

void clip_translucent_windows_that_cannot_blend()
{
    if (!x11_connection())
        return;
    qApp->installEventFilter(new TranslucentWindowClipper(qApp));
}

}
