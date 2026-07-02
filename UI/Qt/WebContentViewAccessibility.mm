/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebContentViewAccessibility.h"
#include "WebContentView.h"

#include <LibWebView/AccessibilityTreeManager.h>

#import <AppKit/AppKit.h>
#import <objc/message.h>
#import <objc/runtime.h>

#import <Interface/LadybirdAccessibilityElement.h>
#import <Interface/LadybirdAccessibilityViewProtocol.h>

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
    NSRect window_rect = [self convertRect:viewRect toView:nil];
    return [self.window convertRectToScreen:window_rect];
}

- (NSRect)accessibilityViewRectForScreenPoint:(NSPoint)screenPoint
{
    NSRect screen_rect = NSMakeRect(screenPoint.x, screenPoint.y, 0, 0);
    NSRect window_rect = [self.window convertRectFromScreen:screen_rect];
    NSPoint view_point = [self convertPoint:window_rect.origin fromView:nil];
    return NSMakeRect(view_point.x, view_point.y, 0, 0);
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

    Vector<i64> stack;
    for (int i = static_cast<int>(root->child_ids.size()) - 1; i >= 0; --i)
        stack.append(root->child_ids[i]);

    while (!stack.is_empty()) {
        auto id = stack.take_last();
        auto const* node = _manager->node(id);
        if (!node)
            continue;
        auto role = node->role.bytes_as_string_view();
        bool ignored = (role == "generic"sv && node->name.is_empty())
            || (role == "paragraph"sv && node->name.is_empty());
        if (!ignored && role != "document"sv)
            return [self accessibilityElementForNodeID:id];
        for (int i = static_cast<int>(node->child_ids.size()) - 1; i >= 0; --i)
            stack.append(node->child_ids[i]);
    }

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

static void install_swizzles()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

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

    auto* overlay = [[WebContentAccessibilityView alloc]
         initWithFrame:ns_view.bounds
               manager:view->m_accessibility_manager.ptr()
        webContentView:view];
    [ns_view addSubview:overlay];
}

void update_accessibility_tree(WebContentView* view)
{
    auto* overlay = get_overlay(view);
    if (!overlay)
        return;

    NSView* ns_view = (__bridge NSView*)reinterpret_cast<void*>(view->winId());

    [overlay.elements removeAllObjects];

    if (ns_view && ns_view.window)
        [ns_view.window makeFirstResponder:ns_view];

    NSAccessibilityPostNotification(ns_view ?: (NSView*)overlay,
        NSAccessibilityLayoutChangedNotification);
    NSAccessibilityPostNotification(
        NSAccessibilityUnignoredAncestor(ns_view ?: (NSView*)overlay),
        @"AXLoadComplete");
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
