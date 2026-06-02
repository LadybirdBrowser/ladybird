/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/MacWindow.h>

#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QCoreApplication>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QWidget>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
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

class LadybirdAppKitEventCaptureFilter final : public QAbstractNativeEventFilter {
public:
    virtual bool nativeEventFilter(QByteArray const& event_type, void* message, qintptr*) override
    {
        if (!is_appkit_event_type(event_type))
            return false;

        auto* event = static_cast<NSEvent*>(message);
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

}
