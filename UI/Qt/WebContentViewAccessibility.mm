/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebContentViewAccessibility.h"
#include "WebContentView.h"

#include <LibWebView/AccessibilityTreeManager.h>

#import <AppKit/AppKit.h>
#import <objc/message.h>
#import <objc/runtime.h>

#import "LadybirdAccessibilityElement.h"
#import "LadybirdAccessibilityViewProtocol.h"

#import <QAccessibleInterface>
#import <QAccessibleWidget>
#import <QWidget>

using namespace Qt::StringLiterals;

// ARC is enabled via CMake compile options

extern "C" void NSAccessibilityHandleFocusChanged();

@interface WebContentAccessibilityView : NSView <LadybirdAccessibilityView>

@property (nonatomic, assign) WebView::AccessibilityTreeManager* manager;
@property (nonatomic, assign) Ladybird::WebContentView* webContentView;
@property (nonatomic, strong) NSMutableDictionary<NSNumber*, LadybirdAccessibilityElement*>* elements;

@end

@implementation WebContentAccessibilityView

- (instancetype)initWithFrame:(NSRect)frame
                      manager:(WebView::AccessibilityTreeManager*)manager
               webContentView:(Ladybird::WebContentView*)webContentView
{
    self = [super initWithFrame:frame];
    if (self) {
        _manager = manager;
        _webContentView = webContentView;
        _elements = [NSMutableDictionary dictionary];
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    return self;
}

- (NSView*)hitTest:(NSPoint)point
{
    return nil;
}

- (BOOL)acceptsFirstResponder
{
    return NO;
}

- (BOOL)isFlipped
{
    return YES;
}

// LadybirdAccessibilityView protocol

- (id)accessibilityElementForNodeID:(int64_t)nodeID
{
    NSNumber* key = @(nodeID);
    LadybirdAccessibilityElement* existing = _elements[key];
    if (existing)
        return existing;

    auto const* data = _manager->node(nodeID);
    if (!data)
        return nil;

    auto* element = [[LadybirdAccessibilityElement alloc] initWithNodeID:nodeID
                                                                 manager:_manager
                                                                    view:self];
    _elements[key] = element;
    return element;
}

- (NSRect)accessibilityScreenRectForViewRect:(NSRect)viewRect
{
    // The bounds from WebContent are in CSS pixels, and the view shows the page at its zoom level: WebContent folds
    // zoom into its device scale, the way Gecko does (nsPresContext::SetFullZoom() refreshes the app-units-per-device-
    // pixel ratio that LocalAccessible::Bounds() converts with), so a CSS pixel is zoom_level() points. No device-
    // pixel-ratio scaling on top of that — convertRect works in points. (Blink and WebKit zoom in layout instead, so
    // their AX bounds come out zoomed already.)
    auto zoom = _webContentView->zoom_level();
    NSRect zoomed_rect = NSMakeRect(viewRect.origin.x * zoom, viewRect.origin.y * zoom, viewRect.size.width * zoom, viewRect.size.height * zoom);
    NSRect window_rect = [self convertRect:zoomed_rect toView:nil];
    return [self.window convertRectToScreen:window_rect];
}

- (NSRect)accessibilityViewRectForScreenPoint:(NSPoint)screenPoint
{
    // The inverse: a screen point to CSS pixels, so points divided by the zoom level.
    NSRect screen_rect = NSMakeRect(screenPoint.x, screenPoint.y, 0, 0);
    NSRect window_rect = [self.window convertRectFromScreen:screen_rect];
    NSPoint view_point = [self convertPoint:window_rect.origin fromView:nil];
    auto zoom = _webContentView->zoom_level();
    return NSMakeRect(view_point.x / zoom, view_point.y / zoom, 0, 0);
}

- (void)performAccessibilityAction:(NSString*)action forNodeID:(int64_t)nodeID
{
    auto action_string = MUST(String::from_utf8(StringView { [action UTF8String], strlen([action UTF8String]) }));
    _webContentView->perform_accessibility_action(nodeID, AK::move(action_string));
}

- (NSURL*)accessibilityPageURL
{
    if (!_webContentView)
        return nil;
    auto const& url = _webContentView->url();
    if (url.scheme().is_empty())
        return nil;
    auto serialized = url.serialize();
    auto* ns_string = [[NSString alloc] initWithBytes:serialized.bytes().data()
                                               length:serialized.bytes().size()
                                             encoding:NSUTF8StringEncoding];
    if (ns_string == nil)
        return nil;
    return [NSURL URLWithString:ns_string];
}

- (BOOL)accessibilityViewIsFirstResponder
{
    return [[self window] firstResponder] == self;
}

// NSAccessibility: scroll area containing the web content root

- (BOOL)isAccessibilityElement
{
    return YES;
}

- (NSAccessibilityRole)accessibilityRole
{
    return NSAccessibilityScrollAreaRole;
}

- (NSArray*)accessibilityChildren
{
    if (!_manager || _manager->is_empty())
        return @[];

    auto const* root = _manager->root();
    if (!root)
        return @[];

    id root_element = [self accessibilityElementForNodeID:root->id];
    if (!root_element)
        return @[];

    return @[ root_element ];
}

- (NSArray*)accessibilityChildrenInNavigationOrder
{
    return [self accessibilityChildren];
}

- (id)accessibilityFocusedUIElement
{
    if (!_manager || _manager->is_empty())
        return self;

    auto const* root = _manager->root();
    if (!root)
        return self;

    // The focused DOM element, if there is one (an input the user clicked into, e.g.).
    if (auto focused_id = _manager->focused_node_id(); focused_id.has_value()) {
        if (id element = [self accessibilityElementForNodeID:*focused_id])
            return element;
    }

    // No DOM focus: the AXWebArea. On page load, VoiceOver expects the focused element to be the web area, so it can
    // move its cursor into the document and read from the top; a leaf instead makes VoiceOver think the user already
    // navigated to that element, and it skips that read.
    if (id element = [self accessibilityElementForNodeID:root->id])
        return element;
    return self;
}

- (id)accessibilityHitTest:(NSPoint)point
{
    if (!_manager || _manager->is_empty())
        return self;

    NSRect view_rect = [self accessibilityViewRectForScreenPoint:point];
    auto content_point = Gfx::IntPoint {
        static_cast<int>(view_rect.origin.x),
        static_cast<int>(view_rect.origin.y)
    };

    auto const* hit = _manager->hit_test(content_point);
    if (!hit)
        return self;

    while (hit) {
        auto role = hit->role.bytes_as_string_view();
        bool ignored = (role == "generic"sv && hit->name.is_empty())
            || (role == "paragraph"sv && hit->name.is_empty());
        if (!ignored)
            break;
        if (hit->parent_id == -1)
            return self;
        hit = _manager->node(hit->parent_id);
    }

    if (hit)
        return [self accessibilityElementForNodeID:hit->id];
    return self;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

- (NSArray*)accessibilityParameterizedAttributeNames
{
    return @[
        @"AXUIElementsForSearchPredicate",
        @"AXUIElementCountForSearchPredicate",
        @"AXIndexForChildUIElement",
    ];
}

- (id)accessibilityAttributeValue:(NSString*)attribute forParameter:(id)parameter
{
    if ([attribute isEqualToString:@"AXIndexForChildUIElement"]) {
        NSArray* children = [self accessibilityChildren];
        NSUInteger idx = [children indexOfObjectIdenticalTo:parameter];
        if (idx != NSNotFound)
            return @(idx);
        return nil;
    }

    auto const* root = _manager ? _manager->root() : nullptr;
    if (!root)
        return nil;

    id root_element = [self accessibilityElementForNodeID:root->id];
    if ([root_element respondsToSelector:@selector(accessibilityAttributeValue:forParameter:)])
        return [root_element accessibilityAttributeValue:attribute forParameter:parameter];

    return nil;
}

#pragma clang diagnostic pop

@end

static QAccessibleInterface* get_qt_interface(id element)
{
    SEL sel = NSSelectorFromString(@"qtInterface");
    if ([element respondsToSelector:sel]) {
        using QtIfaceGetter = QAccessibleInterface* (*)(id, SEL);
        return ((QtIfaceGetter)objc_msgSend)(element, sel);
    }
    return nullptr;
}

static WebContentAccessibilityView* find_overlay_for_element(id element)
{
    auto* iface = get_qt_interface(element);
    if (!iface)
        return nil;
    auto* widget = qobject_cast<Ladybird::WebContentView*>(iface->object());
    if (!widget)
        return nil;
    NSView* ns_view = (__bridge NSView*)reinterpret_cast<void*>(widget->winId());
    if (!ns_view)
        return nil;
    for (NSView* subview in ns_view.subviews) {
        if ([subview isKindOfClass:[WebContentAccessibilityView class]])
            return (WebContentAccessibilityView*)subview;
    }
    return nil;
}

static IMP s_original_role = nullptr;
static IMP s_original_children = nullptr;
static IMP s_original_focused = nullptr;
static IMP s_original_view_focused = nullptr;

static NSString* swizzled_role(id self, SEL _cmd)
{
    if (find_overlay_for_element(self))
        return @"AXWebArea";
    return reinterpret_cast<NSString* (*)(id, SEL)>(s_original_role)(self, _cmd);
}

static NSArray* swizzled_children(id self, SEL _cmd)
{
    auto* overlay = find_overlay_for_element(self);
    if (overlay)
        return @[ overlay ];
    return reinterpret_cast<NSArray* (*)(id, SEL)>(s_original_children)(self, _cmd);
}

static id swizzled_focused(id self, SEL _cmd)
{
    auto* overlay = find_overlay_for_element(self);
    if (overlay)
        return [overlay accessibilityFocusedUIElement];
    return reinterpret_cast<id (*)(id, SEL)>(s_original_focused)(self, _cmd);
}

// AppKit resolves the application's focused UI element through the key window's first responder, which for a Qt window
// is its QNSView — and Qt answers with the element of the widget that has the keyboard focus, never looking inside it. So
// when that widget is a web view, answer with what its overlay would: the focused DOM element, or the AXWebArea. That's
// the answer Chrome's RenderWidgetHostViewCocoa (also the first responder) gives from accessibilityFocusedUIElement.
static id into_web_content(id focused)
{
    if (auto* overlay = find_overlay_for_element(focused))
        return [overlay accessibilityFocusedUIElement];
    return focused;
}

static id swizzled_view_focused(id self, SEL _cmd)
{
    return into_web_content(reinterpret_cast<id (*)(id, SEL)>(s_original_view_focused)(self, _cmd));
}

static void install_swizzles()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    if (Class view_class = NSClassFromString(@"QNSView")) {
        // Override on QNSView only: if Qt implements the method, wrap Qt's; if QNSView inherits NSView's, add an
        // override that falls through to it, rather than rewriting NSView's for every view in the process.
        SEL selector = @selector(accessibilityFocusedUIElement);
        Method own = class_getInstanceMethod(view_class, selector);
        Method inherited = class_getInstanceMethod([NSView class], selector);
        if (own && own != inherited) {
            s_original_view_focused = method_getImplementation(own);
            method_setImplementation(own, (IMP)swizzled_view_focused);
        } else if (inherited) {
            s_original_view_focused = method_getImplementation(inherited);
            class_addMethod(view_class, selector, (IMP)swizzled_view_focused, method_getTypeEncoding(inherited));
        }
    }

    Class cls = NSClassFromString(@"QMacAccessibilityElement");
    if (!cls)
        return;

    Method m;

    m = class_getInstanceMethod(cls, @selector(accessibilityRole));
    if (m) {
        s_original_role = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_role);
    }

    m = class_getInstanceMethod(cls, @selector(accessibilityChildren));
    if (m) {
        s_original_children = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_children);
    }

    m = class_getInstanceMethod(cls, @selector(accessibilityFocusedUIElement));
    if (m) {
        s_original_focused = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_focused);
    }
}

static WebContentAccessibilityView* get_overlay(QWidget* widget)
{
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(widget->winId());
    if (!view)
        return nil;
    for (NSView* subview in view.subviews) {
        if ([subview isKindOfClass:[WebContentAccessibilityView class]])
            return (WebContentAccessibilityView*)subview;
    }
    return nil;
}

namespace Ladybird {

// Minimal QAccessibleInterface – so Qt's bridge creates a QMacAccessibilityElement for the WebContentView widget.
class WebContentViewAccessible : public QAccessibleWidget {
public:
    explicit WebContentViewAccessible(QWidget* widget)
        : QAccessibleWidget(widget, QAccessible::Grouping)
    {
    }
    int childCount() const override { return 0; }
    QAccessibleInterface* child(int) const override { return nullptr; }
};

static QAccessibleInterface* accessibility_factory(QString const& class_name, QObject* object)
{
    if (class_name == "Ladybird::WebContentView"_L1) {
        if (auto* widget = qobject_cast<QWidget*>(object))
            return new WebContentViewAccessible(widget);
    }
    return nullptr;
}

void install_accessibility(WebContentView* view)
{
    static bool factory_installed = false;
    if (!factory_installed) {
        QAccessible::installFactory(accessibility_factory);
        factory_installed = true;
    }

    install_swizzles();

    NSView* ns_view = (__bridge NSView*)reinterpret_cast<void*>(view->winId());
    if (!ns_view)
        return;

    // The overlay is a subview of the widget's NSView, and goes away with it when QWidget::destroy() drops that view
    // (prepare_for_window_move(), as a tab moves to another window) — so finish_window_move() calls this again for the
    // view create() made. A view that still has its overlay keeps it.
    if (get_overlay(view))
        return;

    auto* overlay = [[WebContentAccessibilityView alloc]
         initWithFrame:ns_view.bounds
               manager:view->m_accessibility_manager.ptr()
        webContentView:view];
    [ns_view addSubview:overlay];
}

void update_accessibility_tree(WebContentView* view, bool take_initial_focus, bool report_focus)
{
    auto* overlay = get_overlay(view);
    if (!overlay)
        return;

    NSView* ns_view = (__bridge NSView*)reinterpret_cast<void*>(view->winId());

    // Keep the element of every node that is still in the tree, so a tree update only costs the elements of the nodes it
    // removed, rather than discarding and re-creating one for every node on every DOM mutation — and so the handles an
    // assistive technology holds stay good across a mutation. Elements of nodes that are gone get invalidated before
    // they leave the cache: they hold a raw pointer into the manager, and invalidating them makes sure nothing that
    // still references one can reach it through them.
    NSMutableArray<NSNumber*>* stale_keys = [NSMutableArray array];
    for (NSNumber* key in overlay.elements) {
        if (!overlay.manager->node([key longLongValue]))
            [stale_keys addObject:key];
    }
    for (NSNumber* key in stale_keys) {
        [overlay.elements[key] invalidate];
        [overlay.elements removeObjectForKey:key];
    }
    // On the overlay, not on Qt's NSView: that one is ignored for accessibility, and a notification posted on an ignored
    // element reaches no observer.
    NSAccessibilityPostNotification(overlay, NSAccessibilityLayoutChangedNotification);

    // Only a page's first tree takes the window's first responder and announces the load — the gate WebContentView
    // puts on its setFocus(), for the same reason: WebContent rebuilds the tree on DOM mutations too, and doing this on
    // each of those would yank keyboard focus out of the address bar mid-keystroke, and VoiceOver's cursor back to the
    // top of the page while the user is reading it.
    if (take_initial_focus) {
        if (ns_view && ns_view.window)
            [ns_view.window makeFirstResponder:ns_view];
        NSAccessibilityPostNotification(NSAccessibilityUnignoredAncestor(ns_view ?: (NSView*)overlay),
            @"AXLoadComplete");
        NSAccessibilityHandleFocusChanged();
        return;
    }

    // An assistive technology that arrived mid-page got no focused element from its first queries: Have AppKit look the
    // focus up again now that there's a tree to find it in — as a focus change does. But only while the view holds the
    // keyboard focus in the key window, which is when Chrome fires focus events too (RenderWidgetHostViewMac::
    // AccessibilityHasFocus()); otherwise, AppKit would re-post the focus of whatever else holds it. Qt's focus is the
    // one to ask: the window's first responder is the top-level QNSView, whichever widget has the focus.
    NSWindow* window = ns_view ? ns_view.window : overlay.window;
    if (report_focus && window && window.isKeyWindow && view->hasFocus())
        NSAccessibilityHandleFocusChanged();
}

void post_accessibility_focus_changed(WebContentView* view, i64 node_id)
{
    auto* overlay = get_overlay(view);
    if (!overlay)
        return;

    id element = [overlay accessibilityElementForNodeID:node_id];
    if (!element)
        return;

    NSAccessibilityPostNotification(element,
        NSAccessibilityFocusedUIElementChangedNotification);
    NSAccessibilityHandleFocusChanged();
}

void post_accessibility_announcement(String const& text, String const& live_value)
{
    if (text.is_empty())
        return;

    NSString* announcement = [[NSString alloc]
        initWithBytes:text.bytes().data()
               length:text.bytes().size()
             encoding:NSUTF8StringEncoding];
    if (!announcement || [announcement length] == 0)
        return;

    NSAccessibilityPriorityLevel priority = (live_value == "assertive"sv)
        ? NSAccessibilityPriorityHigh
        : NSAccessibilityPriorityMedium;

    NSAccessibilityPostNotificationWithUserInfo(
        [NSApp mainWindow],
        NSAccessibilityAnnouncementRequestedNotification,
        @{
            NSAccessibilityAnnouncementKey : announcement,
            NSAccessibilityPriorityKey : @(priority),
        });
}

}
