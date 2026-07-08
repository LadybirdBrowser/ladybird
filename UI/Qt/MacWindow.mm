/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/MacWindow.h>

#include <LibGfx/Color.h>
#include <LibGfx/Point.h>
#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QCoreApplication>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QWidget>
#include <UI/Qt/WebContentView.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <UI/AppKit/Utilities/DictionaryLookup.h>
#import <objc/runtime.h>

@interface LadybirdWindowControlHoverTracker : NSObject
@property (nonatomic, retain) NSTrackingArea* trackingArea;
- (instancetype)initWithWidget:(QWidget*)widget callback:(void (*)(QWidget*))callback;
@end

@implementation LadybirdWindowControlHoverTracker
{
    void (*m_callback)(QWidget*);
    QPointer<QWidget> m_widget;
}

- (instancetype)initWithWidget:(QWidget*)widget callback:(void (*)(QWidget*))callback
{
    if ((self = [super init])) {
        m_callback = callback;
        m_widget = widget;
    }
    return self;
}

- (void)mouseEntered:(NSEvent*)event
{
    (void)event;
    if (m_callback && m_widget)
        m_callback(m_widget);
}

- (void)mouseExited:(NSEvent*)event
{
    (void)event;
    if (m_callback && m_widget)
        m_callback(m_widget);
}

#if !__has_feature(objc_arc)
- (void)dealloc
{
    [_trackingArea release];
    [super dealloc];
}
#endif

@end

namespace Ladybird {

static NSEvent* s_latest_window_drag_event;
static NSInteger s_last_pressure_stage;

static Gfx::Color ns_color_to_gfx_color(NSColor* color)
{
    auto* rgb_color = [color colorUsingColorSpace:NSColorSpace.genericRGBColorSpace];
    if (rgb_color != nil)
        return {
            static_cast<u8>([rgb_color redComponent] * 255),
            static_cast<u8>([rgb_color greenComponent] * 255),
            static_cast<u8>([rgb_color blueComponent] * 255),
            static_cast<u8>([rgb_color alphaComponent] * 255)
        };
    return {};
}

static bool is_window_drag_on_gesture_enabled()
{
    return [[NSUserDefaults standardUserDefaults] boolForKey:@"NSWindowShouldDragOnGesture"];
}

static bool is_appkit_event_type(QByteArray const& event_type)
{
    return event_type == QByteArrayLiteral("NSEvent")
        || event_type == QByteArrayLiteral("mac_generic_NSEvent");
}

static bool is_window_drag_event(NSEvent* event)
{
    if (!event)
        return false;

    switch (event.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseDragged:
        return true;
    default:
        return false;
    }
}

static bool should_start_window_drag_for_gesture(NSEvent* event)
{
    if (!event || event.type != NSEventTypeLeftMouseDown)
        return false;

    if (!is_window_drag_on_gesture_enabled())
        return false;

    auto modifier_flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if ((modifier_flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != (NSEventModifierFlagCommand | NSEventModifierFlagControl))
        return false;

    auto* window = event.window;
    if (!window || !window.isMovable || (window.styleMask & NSWindowStyleMaskFullScreen))
        return false;

    return true;
}

static NSPoint widget_position_to_ns_point(QWidget& widget, Gfx::IntPoint position)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return {};

    auto device_pixel_ratio = widget.devicePixelRatioF();
    auto point = NSMakePoint(position.x() / device_pixel_ratio, position.y() / device_pixel_ratio);
    if (![view isFlipped])
        point.y = NSHeight(view.bounds) - point.y;
    return point;
}

static Optional<Gfx::IntPoint> widget_position_for_appkit_event(QWidget& widget, NSEvent* event)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !event.window)
        return {};

    auto point = [view convertPoint:event.locationInWindow fromView:nil];
    if (![view isFlipped])
        point.y = NSHeight(view.bounds) - point.y;

    auto device_pixel_ratio = widget.devicePixelRatioF();
    return Gfx::IntPoint {
        static_cast<int>(point.x * device_pixel_ratio),
        static_cast<int>(point.y * device_pixel_ratio),
    };
}

static WebContentView* web_content_view_for_appkit_event(NSEvent* event)
{
    if (!event.window)
        return nullptr;

    auto* content_view = event.window.contentView;
    if (!content_view)
        return nullptr;

    auto point = [content_view convertPoint:event.locationInWindow fromView:nil];
    for (auto* view = [content_view hitTest:point]; view; view = view.superview) {
        auto* widget = QWidget::find(reinterpret_cast<WId>(view));
        if (!widget)
            continue;
        if (auto* web_content_view = qobject_cast<WebContentView*>(widget))
            return web_content_view;
    }

    return nullptr;
}

static bool should_perform_dictionary_lookup_for_event(NSEvent* event)
{
    if (!event)
        return false;

    switch (event.type) {
    case NSEventTypeQuickLook:
        return true;
    case NSEventTypePressure: {
        auto stage = event.stage;
        auto should_lookup = s_last_pressure_stage == 1
            && stage == 2
            && [[NSUserDefaults standardUserDefaults] integerForKey:@"com.apple.trackpad.forceClick"] == 1;
        s_last_pressure_stage = stage;
        return should_lookup;
    }
    default:
        return false;
    }
}

static bool perform_dictionary_lookup_for_event(NSEvent* event)
{
    if (!should_perform_dictionary_lookup_for_event(event))
        return false;

    auto* view = web_content_view_for_appkit_event(event);
    if (!view)
        return false;

    auto position = widget_position_for_appkit_event(*view, event);
    if (!position.has_value())
        return false;

    return view->look_up_selected_text_at(*position);
}

class LadybirdAppKitEventCaptureFilter final : public QAbstractNativeEventFilter {
public:
    virtual bool nativeEventFilter(QByteArray const& event_type, void* message, qintptr*) override
    {
        if (!is_appkit_event_type(event_type))
            return false;

        auto* event = static_cast<NSEvent*>(message);
        if (perform_dictionary_lookup_for_event(event))
            return true;

        if (should_start_window_drag_for_gesture(event)) {
            [event.window performWindowDragWithEvent:event];
            return true;
        }

        if (!is_window_drag_event(event))
            return false;

        [s_latest_window_drag_event release];
        s_latest_window_drag_event = [event copy];
        return false;
    }
};

static CGColorRef cg_color_from_qcolor(QColor const& color)
{
    return [NSColor colorWithSRGBRed:color.redF() green:color.greenF() blue:color.blueF() alpha:1.0].CGColor;
}

void install_appkit_event_capture()
{
    static auto* filter = new LadybirdAppKitEventCaptureFilter;
    static bool installed = false;
    if (installed)
        return;

    if (auto* application = QCoreApplication::instance()) {
        application->installNativeEventFilter(filter);
        installed = true;
    }
}

void set_rounded_window_corners(QWidget& widget, bool enabled, double radius, QColor const& background_color)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;

    auto* content_view = window.contentView;
    if (!content_view)
        return;

    content_view.wantsLayer = YES;
    content_view.layer.cornerRadius = enabled ? radius : 0.0;
    content_view.layer.masksToBounds = enabled ? YES : NO;
    content_view.layer.opaque = YES;
    content_view.layer.backgroundColor = cg_color_from_qcolor(background_color);
}

void set_appkit_window_resizable(QWidget& widget, bool enabled)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;

    if (enabled)
        window.styleMask |= NSWindowStyleMaskResizable;
    else
        window.styleMask &= ~NSWindowStyleMaskResizable;
}

void install_always_active_window_control_hover_tracking(QWidget& widget, void (*hover_changed)(QWidget*))
{
    static char tracker_key;

    auto* window_widget = widget.window();
    if (!window_widget)
        return;

    auto* view = reinterpret_cast<NSView*>(window_widget->winId());
    if (!view)
        return;

    if (auto* existing_tracker = static_cast<LadybirdWindowControlHoverTracker*>(objc_getAssociatedObject(view, &tracker_key))) {
        if (existing_tracker.trackingArea)
            [view removeTrackingArea:existing_tracker.trackingArea];
    }

    auto* tracker = [[LadybirdWindowControlHoverTracker alloc] initWithWidget:&widget callback:hover_changed];
    auto const widget_rect = QRect(widget.mapTo(window_widget, QPoint(0, 0)), widget.size());
    auto tracking_rect = NSMakeRect(widget_rect.x(), widget_rect.y(), widget_rect.width(), widget_rect.height());
    if (![view isFlipped])
        tracking_rect.origin.y = NSHeight(view.bounds) - NSMaxY(tracking_rect);

    auto options = NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways;
    auto* tracking_area = [[NSTrackingArea alloc] initWithRect:tracking_rect options:options owner:tracker userInfo:nil];
    tracker.trackingArea = tracking_area;
    [view addTrackingArea:tracking_area];
    objc_setAssociatedObject(view, &tracker_key, tracker, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
#if !__has_feature(objc_arc)
    [tracking_area release];
    [tracker release];
#endif
}

void make_appkit_window_first_responder(QWidget& widget)
{
    auto* window_widget = widget.window();
    if (!window_widget)
        return;

    auto* view = reinterpret_cast<NSView*>(window_widget->winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;

    [window makeFirstResponder:view];
}

bool start_appkit_window_drag(QWidget& widget)
{
    auto* window_widget = widget.window();
    if (!window_widget)
        return false;

    auto* view = reinterpret_cast<NSView*>(window_widget->winId());
    if (!view)
        return false;

    auto* window = view.window;
    if (!window)
        return false;

    auto* current_event = NSApp.currentEvent;
    auto current_event_is_valid = is_window_drag_event(current_event);
    auto current_event_window_matches = current_event_is_valid && current_event.window == window;

    auto* event = current_event_window_matches ? current_event : s_latest_window_drag_event;
    if (!is_window_drag_event(event) || event.window != window)
        return false;

    [window performWindowDragWithEvent:event];
    return true;
}

void show_appkit_dictionary_lookup(QWidget& widget, WebView::DictionaryLookup const& lookup, Gfx::IntPoint position)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    show_dictionary_lookup(view, lookup, widget_position_to_ns_point(widget, position));
}

Gfx::Color appkit_web_inactive_selection_color()
{
    return ns_color_to_gfx_color([NSColor unemphasizedSelectedTextBackgroundColor]);
}

Gfx::Color appkit_web_inactive_selection_text_color()
{
    return ns_color_to_gfx_color([NSColor unemphasizedSelectedTextColor]);
}

}
