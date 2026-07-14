/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/MacWindow.h>

#include <LibGfx/Color.h>
#include <LibGfx/Point.h>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QWidget>
#include <UI/Qt/WebContentView.h>

#import <Cocoa/Cocoa.h>
#import <UI/AppKit/Utilities/DictionaryLookup.h>
#import <objc/runtime.h>

@interface LadybirdWindowControlOffsetObserver : NSObject
{
    int m_x_offset;
    int m_y_offset;
    NSButton* m_buttons[3];
    NSPoint m_target_origins[3];
    bool m_update_scheduled;
    bool m_is_applying_offset;
}

- (instancetype)initWithWindow:(NSWindow*)window xOffset:(int)x_offset yOffset:(int)y_offset;
- (void)applyWindowControlOffsets;
- (void)scheduleWindowControlOffsetUpdate:(NSButton*)button;
- (void)windowControlFrameDidChange:(NSNotification*)notification;
- (void)windowDidResize:(NSNotification*)notification;

@end

@implementation LadybirdWindowControlOffsetObserver

- (instancetype)initWithWindow:(NSWindow*)window xOffset:(int)x_offset yOffset:(int)y_offset
{
    self = [super init];
    if (!self)
        return nil;

    m_x_offset = x_offset;
    m_y_offset = y_offset;

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(windowDidResize:) name:NSWindowDidResizeNotification object:window];

    constexpr NSWindowButton button_types[] = { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton };
    for (size_t i = 0; i < std::size(button_types); ++i) {
        auto* button = [window standardWindowButton:button_types[i]];
        m_buttons[i] = [button retain];
        button.postsFrameChangedNotifications = YES;
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(windowControlFrameDidChange:) name:NSViewFrameDidChangeNotification object:button];
        [self scheduleWindowControlOffsetUpdate:button];
    }

    return self;
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
    for (auto* button : m_buttons)
        [button release];
    [super dealloc];
}

- (void)applyWindowControlOffsets
{
    m_is_applying_offset = true;
    for (size_t i = 0; i < std::size(m_buttons); ++i)
        [m_buttons[i] setFrameOrigin:m_target_origins[i]];
    m_is_applying_offset = false;
    m_update_scheduled = false;
}

- (void)scheduleWindowControlOffsetUpdate:(NSButton*)button
{
    for (size_t i = 0; i < std::size(m_buttons); ++i) {
        if (m_buttons[i] != button)
            continue;

        auto origin = button.frame.origin;
        origin.x += m_x_offset;
        origin.y -= m_y_offset;
        m_target_origins[i] = origin;
        break;
    }

    if (m_update_scheduled)
        return;

    // NB: AppKit ignores frame changes made while its titlebar layout is in progress.
    //     Apply the offset after that layout pass returns to the main event loop.
    m_update_scheduled = true;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self applyWindowControlOffsets];
    });
}

- (void)windowControlFrameDidChange:(NSNotification*)notification
{
    if (m_is_applying_offset)
        return;
    [self scheduleWindowControlOffsetUpdate:notification.object];
}

- (void)windowDidResize:(NSNotification*)notification
{
    [self applyWindowControlOffsets];
}

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

void hide_appkit_window_title(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;

    window.titleVisibility = NSWindowTitleHidden;
}

void offset_appkit_window_controls(QWidget& widget, int x_offset, int y_offset)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;

    static char observer_key;
    auto* observer = [[LadybirdWindowControlOffsetObserver alloc] initWithWindow:window xOffset:x_offset yOffset:y_offset];
    objc_setAssociatedObject(window, &observer_key, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [observer release];
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
