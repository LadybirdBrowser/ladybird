/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/MacWindow.h>

#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>
#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QCoreApplication>
#include <QDrag>
#include <QGuiApplication>
#include <QMargins>
#include <QMouseEvent>
#include <QPointer>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <UI/Qt/WebContentView.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <UI/Qt/DictionaryLookup.h>
#import <dlfcn.h>
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
- (void)updateTargetOriginForButton:(NSButton*)button;
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

    // NB: Lay out the title bar now, so the controls are in their default place to offset them from, and move them
    //     right away. Before a window is first shown, this keeps it from ever appearing with its controls unmoved.
    [window.contentView.superview layoutSubtreeIfNeeded];

    constexpr NSWindowButton button_types[] = { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton };
    for (size_t i = 0; i < std::size(button_types); ++i) {
        auto* button = [window standardWindowButton:button_types[i]];
        m_buttons[i] = [button retain];
        button.postsFrameChangedNotifications = YES;
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(windowControlFrameDidChange:) name:NSViewFrameDidChangeNotification object:button];
        [self updateTargetOriginForButton:button];
    }
    [self applyWindowControlOffsets];

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
    [m_buttons[0].superview updateTrackingAreas];
    m_is_applying_offset = false;
    m_update_scheduled = false;
}

- (void)updateTargetOriginForButton:(NSButton*)button
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
}

- (void)scheduleWindowControlOffsetUpdate:(NSButton*)button
{
    [self updateTargetOriginForButton:button];

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

@interface LadybirdShadowView : NSView
@end

@implementation LadybirdShadowView

- (NSView*)hitTest:(NSPoint)point
{
    return nil;
}

@end

namespace Ladybird {

static NSEvent* s_latest_window_drag_event;
static NSInteger s_last_pressure_stage;

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

static Optional<QPointF> widget_point_for_appkit_event(QWidget& widget, NSEvent* event)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !event.window)
        return {};

    auto point = [view convertPoint:event.locationInWindow fromView:nil];
    if (![view isFlipped])
        point.y = NSHeight(view.bounds) - point.y;
    return QPointF { point.x, point.y };
}

static Optional<Gfx::IntPoint> widget_position_for_appkit_event(QWidget& widget, NSEvent* event)
{
    auto point = widget_point_for_appkit_event(widget, event);
    if (!point.has_value())
        return {};

    auto device_pixel_ratio = widget.devicePixelRatioF();
    return Gfx::IntPoint {
        static_cast<int>(point->x() * device_pixel_ratio),
        static_cast<int>(point->y() * device_pixel_ratio),
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

static QPointer<WebContentView> s_view_following_mouse_while_inactive;

// Qt ignores mouse movement while Ladybird is inactive, so it is passed on here to a view that follows it anyway.
static void follow_mouse_while_inactive(NSEvent* event)
{
    if (!event || NSApp.active)
        return;

    if (event.type == NSEventTypeMouseExited) {
        auto view = s_view_following_mouse_while_inactive;
        if (!view || event.window != reinterpret_cast<NSView*>(view->winId()).window)
            return;
        s_view_following_mouse_while_inactive = nullptr;
        QEvent leave_event { QEvent::Leave };
        QCoreApplication::sendEvent(view, &leave_event);
        return;
    }

    if (event.type != NSEventTypeMouseMoved)
        return;

    auto* view = web_content_view_for_appkit_event(event);
    if (!view || !view->follows_mouse_while_inactive())
        return;
    auto point = widget_point_for_appkit_event(*view, event);
    if (!point.has_value())
        return;

    s_view_following_mouse_while_inactive = view;
    QMouseEvent move_event { QEvent::MouseMove, *point, view->mapToGlobal(*point), Qt::NoButton, Qt::NoButton, Qt::NoModifier };
    QCoreApplication::sendEvent(view, &move_event);
}

// AppKit reports no more than whether a scroll event ends the fingers' part of a gesture, but the HID event behind it
// says whether momentum follows. The functions that read it are private, so they are looked up rather than linked.
static bool appkit_scroll_momentum_will_begin(NSEvent* event)
{
    using CopyIOHIDEventFunction = CFTypeRef (*)(CGEventRef);
    using GetScrollMomentumFunction = u8 (*)(CFTypeRef);
    static auto copy_iohid_event = reinterpret_cast<CopyIOHIDEventFunction>(dlsym(RTLD_DEFAULT, "CGEventCopyIOHIDEvent"));
    static auto get_scroll_momentum = reinterpret_cast<GetScrollMomentumFunction>(dlsym(RTLD_DEFAULT, "IOHIDEventGetScrollMomentum"));
    static constexpr u8 scroll_momentum_will_begin = 1 << 3;

    if (!copy_iohid_event || !get_scroll_momentum || !event.CGEvent)
        return false;
    auto iohid_event = copy_iohid_event(event.CGEvent);
    if (!iohid_event)
        return false;
    ScopeGuard release_iohid_event = [&] { CFRelease(iohid_event); };
    return (get_scroll_momentum(iohid_event) & scroll_momentum_will_begin) != 0;
}

struct RecordedAppKitScrollMomentumTransition {
    u64 timestamp_in_milliseconds { 0 };
    AppKitScrollMomentumTransition transition;
};
static Vector<RecordedAppKitScrollMomentumTransition> s_recorded_appkit_scroll_momentum_transitions;

// Qt delivers a wheel event after AppKit dispatched the scroll event it came from, and stamps it with that event's
// time in milliseconds.
static u64 qt_timestamp_for_appkit_event(NSEvent* event)
{
    return static_cast<u64>(event.timestamp * 1000);
}

static void record_appkit_scroll_momentum_transition(NSEvent* event)
{
    if (event.type != NSEventTypeScrollWheel)
        return;

    Optional<AppKitScrollMomentumTransition> transition;
    if (event.momentumPhase == NSEventPhaseBegan)
        transition = AppKitScrollMomentumTransition::Began;
    else if (event.phase == NSEventPhaseEnded && event.momentumPhase == NSEventPhaseNone && appkit_scroll_momentum_will_begin(event))
        transition = AppKitScrollMomentumTransition::WillBegin;
    if (!transition.has_value())
        return;

    static constexpr size_t maximum_recorded_transitions = 8;
    if (s_recorded_appkit_scroll_momentum_transitions.size() == maximum_recorded_transitions)
        s_recorded_appkit_scroll_momentum_transitions.remove(0);
    s_recorded_appkit_scroll_momentum_transitions.append({
        .timestamp_in_milliseconds = qt_timestamp_for_appkit_event(event),
        .transition = transition.value(),
    });
}

// Qt makes a ScrollEnd of the lift, unless it saw the momentum coming, and a ScrollBegin of the momentum's first event,
// unless it did. The fingers' last step can share the lift's millisecond, and takes neither.
static bool qt_wheel_event_is_made_of(QWheelEvent const& wheel_event, AppKitScrollMomentumTransition transition)
{
    switch (transition) {
    case AppKitScrollMomentumTransition::WillBegin:
        return wheel_event.phase() == Qt::ScrollEnd;
    case AppKitScrollMomentumTransition::Began:
        return wheel_event.phase() == Qt::ScrollBegin;
    }
    VERIFY_NOT_REACHED();
}

Optional<AppKitScrollMomentumTransition> take_appkit_scroll_momentum_transition_for(QWheelEvent const& wheel_event)
{
    // The transitions recorded before this one went to other windows, or were dropped by Qt.
    auto timestamp = static_cast<u64>(wheel_event.timestamp());
    while (!s_recorded_appkit_scroll_momentum_transitions.is_empty() && s_recorded_appkit_scroll_momentum_transitions.first().timestamp_in_milliseconds < timestamp)
        s_recorded_appkit_scroll_momentum_transitions.remove(0);
    for (size_t index = 0; index < s_recorded_appkit_scroll_momentum_transitions.size(); index++) {
        auto const& recorded = s_recorded_appkit_scroll_momentum_transitions[index];
        if (recorded.timestamp_in_milliseconds > timestamp)
            break;
        if (!qt_wheel_event_is_made_of(wheel_event, recorded.transition))
            continue;
        auto transition = recorded.transition;
        s_recorded_appkit_scroll_momentum_transitions.remove(index);
        return transition;
    }
    return {};
}

class LadybirdAppKitEventCaptureFilter final : public QAbstractNativeEventFilter {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual bool nativeEventFilter(QByteArray const& event_type, void* message, qintptr*) override
    {
        if (!is_appkit_event_type(event_type))
            return false;

        auto* event = static_cast<NSEvent*>(message);
        follow_mouse_while_inactive(event);
        record_appkit_scroll_momentum_transition(event);

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

Optional<u64> appkit_display_id_for_window(QWidget& widget)
{
    NSScreen* screen = nil;
    // winId() would create the native window, so only a window that already has one is asked for its screen.
    if (auto* window_widget = widget.window(); window_widget && window_widget->windowHandle()) {
        if (auto* view = reinterpret_cast<NSView*>(window_widget->winId()))
            screen = view.window.screen;
    }
    if (screen == nil)
        screen = NSScreen.mainScreen;
    if (screen == nil)
        return {};

    NSNumber* screen_number = screen.deviceDescription[@"NSScreenNumber"];
    if (screen_number == nil)
        return {};
    return static_cast<u64>(screen_number.unsignedLongLongValue);
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

void make_appkit_window_resizable(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    view.window.styleMask |= NSWindowStyleMaskResizable;
}

void keep_appkit_window_visible_while_inactive(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    view.window.hidesOnDeactivate = NO;
}

void disable_appkit_window_animations(QWidget& widget)
{
    if (QGuiApplication::platformName() != "cocoa")
        return;
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    view.window.animationBehavior = NSWindowAnimationBehaviorNone;
}

void ensure_appkit_window_is_exposed(QWidget& widget)
{
    auto* window = widget.windowHandle();
    if (!widget.isVisible() || !window || window->isExposed())
        return;

    // Qt restores a native view's exposed state from its display callback. A view whose backing layer is already
    // clean can remain unexposed after being shown or raised, causing Qt to route hover events to the view below it.
    [reinterpret_cast<NSView*>(widget.winId()) setNeedsDisplay:YES];
}

void set_appkit_window_content_aspect_ratio(QWidget& widget, Gfx::IntSize aspect_ratio)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view || !view.window)
        return;
    view.window.contentAspectRatio = NSMakeSize(aspect_ratio.width(), aspect_ratio.height());
}

static char s_window_control_offset_observer_key;

void offset_appkit_window_controls(QWidget& widget, int x_offset, int y_offset)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;

    auto* window = view.window;
    if (!window)
        return;
    if (objc_getAssociatedObject(window, &s_window_control_offset_observer_key))
        return;

    auto* observer = [[LadybirdWindowControlOffsetObserver alloc] initWithWindow:window xOffset:x_offset yOffset:y_offset];
    objc_setAssociatedObject(window, &s_window_control_offset_observer_key, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [observer release];
}

void reapply_appkit_window_control_offsets(QWidget& widget)
{
    auto* view = reinterpret_cast<NSView*>(widget.internalWinId());
    if (!view || !view.window)
        return;

    LadybirdWindowControlOffsetObserver* observer = objc_getAssociatedObject(view.window, &s_window_control_offset_observer_key);
    [observer applyWindowControlOffsets];
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

Qt::DropAction execute_appkit_tab_drag(QWidget& widget, QDrag& drag)
{
    if (QGuiApplication::platformName() != "cocoa")
        return drag.exec(Qt::MoveAction, Qt::MoveAction);

    auto* view = reinterpret_cast<NSView*>(widget.window()->winId());
    auto* window = view.window;
    if (!window)
        return drag.exec(Qt::MoveAction, Qt::MoveAction);

    // Qt's Cocoa backend calls this synchronous AppKit API with slideBack:YES. Dropping outside a tab strip is
    // rejected by AppKit, so exec() waits for the slide-back animation before we can create the detached window.
    // Override only the source window's class, and restore it as soon as this tab drag finishes.
    auto window_class = object_getClass(window);
    auto* selector = @selector(dragImage:at:offset:event:pasteboard:source:slideBack:);
    auto* method = class_getInstanceMethod(window_class, selector);
    if (!method)
        return drag.exec(Qt::MoveAction, Qt::MoveAction);

    // Add an override if the method is inherited, so we don't replace NSWindow's implementation globally.
    class_addMethod(window_class, selector, method_getImplementation(method), method_getTypeEncoding(method));
    method = class_getInstanceMethod(window_class, selector);

    auto original_implementation = method_getImplementation(method);
    auto original_drag_image = reinterpret_cast<void (*)(id, SEL, NSImage*, NSPoint, NSSize, NSEvent*, NSPasteboard*, id, BOOL)>(original_implementation);
    auto implementation = imp_implementationWithBlock(^(NSWindow* receiver, NSImage* image, NSPoint location, NSSize offset, NSEvent* event, NSPasteboard* pasteboard, id source, BOOL slide_back) {
        original_drag_image(receiver, selector, image, location, offset, event, pasteboard, source, receiver == window ? NO : slide_back);
    });
    method_setImplementation(method, implementation);
    ScopeGuard restore_implementation = [&]() {
        method_setImplementation(method, original_implementation);
        imp_removeBlock(implementation);
    };

    return drag.exec(Qt::MoveAction, Qt::MoveAction);
}

void show_appkit_dictionary_lookup(QWidget& widget, WebView::DictionaryLookup const& lookup, Gfx::IntPoint position)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    show_dictionary_lookup(view, lookup, widget_position_to_ns_point(widget, position));
}

void round_appkit_view_corners(QWidget& widget, qreal radius, QColor const& border_color)
{
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;
    view.wantsLayer = YES;
    auto* layer = view.layer;
    layer.cornerRadius = radius;
    layer.cornerCurve = kCACornerCurveContinuous;
    layer.masksToBounds = YES;
    layer.borderWidth = 1;
    auto* color = CGColorCreateSRGB(border_color.redF(), border_color.greenF(), border_color.blueF(), border_color.alphaF());
    layer.borderColor = color;
    CGColorRelease(color);
}

void round_appkit_view_bottom_corners(QWidget& widget, qreal radius)
{
    if (QGuiApplication::platformName() != "cocoa")
        return;
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;
    view.wantsLayer = YES;
    auto* layer = view.layer;
    layer.cornerRadius = radius;
    layer.cornerCurve = kCACornerCurveContinuous;
    // NB: Qt's views are flipped, so the bottom of the view is at its layer's largest y.
    layer.maskedCorners = view.isFlipped ? (kCALayerMinXMaxYCorner | kCALayerMaxXMaxYCorner) : (kCALayerMinXMinYCorner | kCALayerMaxXMinYCorner);
    layer.masksToBounds = YES;
}

void cast_appkit_view_shadow(QWidget& widget, qreal corner_radius, qreal blur_radius, qreal y_offset, QColor const& color)
{
    if (QGuiApplication::platformName() != "cocoa")
        return;
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;
    view.wantsLayer = YES;
    auto* layer = view.layer;
    layer.masksToBounds = NO;
    layer.shadowOpacity = 1;
    layer.shadowRadius = blur_radius;
    // NB: The layer of a view is flipped with it, and Qt's views are, so a positive offset points down.
    layer.shadowOffset = CGSizeMake(0, y_offset);
    auto* shadow_color = CGColorCreateSRGB(color.redF(), color.greenF(), color.blueF(), color.alphaF());
    layer.shadowColor = shadow_color;
    CGColorRelease(shadow_color);
    auto* shadow_path = CGPathCreateWithRoundedRect(view.bounds, corner_radius, corner_radius, nullptr);
    layer.shadowPath = shadow_path;
    CGPathRelease(shadow_path);
}

AppKitViewShadow::AppKitViewShadow()
{
    auto* view = [[LadybirdShadowView alloc] initWithFrame:NSZeroRect];
    view.wantsLayer = YES;
    view.layer.masksToBounds = YES;
    view.hidden = YES;
    auto* caster = [[CALayer alloc] init];
    [view.layer addSublayer:caster];
    m_view = view;
    m_caster = caster;
}

AppKitViewShadow::~AppKitViewShadow()
{
    auto* view = static_cast<NSView*>(m_view);
    [view removeFromSuperview];
    [view release];
    [static_cast<CALayer*>(m_caster) release];
}

void AppKitViewShadow::follow(QWidget& widget, QMargins const& room, qreal corner_radius, qreal blur_radius, qreal y_offset, QColor const& shadow_color, QColor const& fill_color)
{
    if (QGuiApplication::platformName() != "cocoa")
        return;
    auto* surface = reinterpret_cast<NSView*>(widget.winId());
    if (!surface || !surface.superview)
        return;
    auto* view = static_cast<NSView*>(m_view);
    auto* caster = static_cast<CALayer*>(m_caster);

    [CATransaction begin];
    [CATransaction setDisableActions:YES];

    auto surface_frame = surface.frame;
    auto room_at_origin = surface.superview.isFlipped ? room.top() : room.bottom();
    view.frame = NSMakeRect(surface_frame.origin.x - room.left(), surface_frame.origin.y - room_at_origin,
        surface_frame.size.width + room.left() + room.right(), surface_frame.size.height + room.top() + room.bottom());
    [surface.superview addSubview:view positioned:NSWindowBelow relativeTo:surface];
    view.hidden = NO;

    // NB: This view is not flipped, unlike Qt's, so the bottom of the room comes first and a shadow falls to lower y.
    auto caster_frame = CGRectMake(room.left(), room.bottom(), surface_frame.size.width, surface_frame.size.height);
    caster.frame = caster_frame;
    caster.cornerRadius = corner_radius;
    auto* fill = CGColorCreateSRGB(fill_color.redF(), fill_color.greenF(), fill_color.blueF(), fill_color.alphaF());
    caster.backgroundColor = fill;
    CGColorRelease(fill);
    auto* shadow = CGColorCreateSRGB(shadow_color.redF(), shadow_color.greenF(), shadow_color.blueF(), shadow_color.alphaF());
    caster.shadowColor = shadow;
    CGColorRelease(shadow);
    caster.shadowOpacity = 1;
    caster.shadowRadius = blur_radius;
    caster.shadowOffset = CGSizeMake(0, -y_offset);
    auto* shadow_path = CGPathCreateWithRoundedRect(CGRectMake(0, 0, caster_frame.size.width, caster_frame.size.height), corner_radius, corner_radius, nullptr);
    caster.shadowPath = shadow_path;
    CGPathRelease(shadow_path);

    [CATransaction commit];
}

void AppKitViewShadow::hide()
{
    static_cast<NSView*>(m_view).hidden = YES;
}

void AppKitViewShadow::set_opacity(qreal opacity)
{
    static_cast<NSView*>(m_view).alphaValue = opacity;
}

void set_appkit_view_opacity(QWidget& widget, qreal opacity)
{
    if (QGuiApplication::platformName() != "cocoa")
        return;
    auto* view = reinterpret_cast<NSView*>(widget.winId());
    if (!view)
        return;
    view.alphaValue = opacity;
}

}
