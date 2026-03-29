/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// On macOS, Qt's accessibility bridge (QMacAccessibilityElement) does not
// implement AXUIElementsForSearchPredicate or support AXWebArea role,
// landmark subroles, or custom role descriptions. This file swizzles
// QMacAccessibilityElement methods at runtime to add these capabilities,
// and handles initial VoiceOver focus via makeFirstResponder.

#include "WebContentViewAccessibilityMac.h"
#include "WebContentView.h"

#include <LibWebView/AccessibilityTreeManager.h>

#import <AppKit/AppKit.h>
#import <QAccessibleInterface>
#import <QVariant>
#import <QWidget>
#import <objc/runtime.h>

// ARC is enabled via CMake compile options

#import <objc/message.h>

// Swizzle Qt's QMacAccessibilityElement to support AXWebArea role,
// custom subroles, and custom role descriptions via dynamic properties.

static IMP s_original_subrole = nullptr;
static IMP s_original_role_description = nullptr;
static IMP s_original_role = nullptr;

static QAccessibleInterface* get_qt_interface(id element)
{
    SEL sel = NSSelectorFromString(@"qtInterface");
    if ([element respondsToSelector:sel]) {
        typedef QAccessibleInterface* (*QtIfaceGetter)(id, SEL);
        return ((QtIfaceGetter)objc_msgSend)(element, sel);
    }
    return nullptr;
}

static NSString* swizzled_subrole(id self, SEL _cmd)
{
    QAccessibleInterface* iface = get_qt_interface(self);
    if (iface) {
        if (QObject const* obj = iface->object()) {
            QVariant v = obj->property("_qt_mac_subrole");
            if (v.isValid() && v.typeId() == QMetaType::QString) {
                NSString* sr = v.toString().toNSString();
                if ([sr length] > 0)
                    return sr;
            }
        }
    }
    return reinterpret_cast<NSString* (*)(id, SEL)>(s_original_subrole)(self, _cmd);
}

static NSString* swizzled_role_description(id self, SEL _cmd)
{
    QAccessibleInterface* iface = get_qt_interface(self);
    if (iface) {
        if (QObject const* obj = iface->object()) {
            QVariant v = obj->property("_qt_mac_roleDescription");
            if (v.isValid() && v.typeId() == QMetaType::QString) {
                NSString* desc = v.toString().toNSString();
                if ([desc length] > 0)
                    return desc;
            }
        }
    }
    return reinterpret_cast<NSString* (*)(id, SEL)>(s_original_role_description)(self, _cmd);
}

static NSString* swizzled_role(id self, SEL _cmd)
{
    QAccessibleInterface* iface = get_qt_interface(self);
    if (iface) {
        switch (iface->role()) {
        case QAccessible::WebDocument:
            return @"AXWebArea";
        case QAccessible::ListItem:
            // Qt maps ListItem to NSAccessibilityStaticTextRole,
            // which prevents VoiceOver from counting list items.
            return NSAccessibilityGroupRole;
        case QAccessible::List:
            return NSAccessibilityListRole;
        default:
            break;
        }
    }
    return reinterpret_cast<NSString* (*)(id, SEL)>(s_original_role)(self, _cmd);
}

// Swizzle accessibilityIsIgnored to make ListItem transparent.
// macOS promotes ignored elements’ children to the parent level,
// so VoiceOver navigates directly to the link/button inside.
static IMP s_original_is_ignored = nullptr;

static BOOL swizzled_is_ignored(id self, SEL _cmd)
{
    QAccessibleInterface* iface = get_qt_interface(self);
    if (iface && iface->role() == QAccessible::ListItem)
        return YES;
    return reinterpret_cast<BOOL (*)(id, SEL)>(s_original_is_ignored)(self, _cmd);
}

extern "C" void NSAccessibilityHandleFocusChanged();

namespace Ladybird {

// Navigation leaf: children are not individually navigable
static bool is_leaf_role(QAccessible::Role r)
{
    return r == QAccessible::Link || r == QAccessible::Button
        || r == QAccessible::Heading || r == QAccessible::MenuItem
        || r == QAccessible::PageTab || r == QAccessible::RadioButton
        || r == QAccessible::CheckBox || r == QAccessible::Graphic
        || r == QAccessible::StaticText;
}

// Container-only roles are descended into but not navigation stops.
// VoiceOver should navigate to the content inside, not the container.
static bool is_container_only_role(QAccessible::Role r)
{
    return r == QAccessible::ListItem;
}

// Swizzled accessibilityParameterizedAttributeNames: add search predicates
static IMP s_original_param_names = nullptr;

static NSArray* swizzled_param_names(id self, SEL _cmd)
{
    NSArray* original = reinterpret_cast<NSArray* (*)(id, SEL)>(s_original_param_names)(self, _cmd);
    QAccessibleInterface* iface = get_qt_interface(self);
    if (!iface)
        return original;

    // Add search predicate attributes to all elements
    NSMutableArray* extended = [NSMutableArray arrayWithArray:(original ?: @[])];
    if (![extended containsObject:@"AXUIElementsForSearchPredicate"])
        [extended addObject:@"AXUIElementsForSearchPredicate"];
    if (![extended containsObject:@"AXUIElementCountForSearchPredicate"])
        [extended addObject:@"AXUIElementCountForSearchPredicate"];
    return extended;
}

// Swizzled accessibilityAttributeValue:forParameter: — handle search predicates
static IMP s_original_param_value = nullptr;

static id swizzled_param_value(id self, SEL _cmd, NSString* attribute, id parameter)
{
    if (![attribute isEqualToString:@"AXUIElementsForSearchPredicate"]
        && ![attribute isEqualToString:@"AXUIElementCountForSearchPredicate"]) {
        if (s_original_param_value)
            return reinterpret_cast<id (*)(id, SEL, NSString*, id)>(s_original_param_value)(self, _cmd, attribute, parameter);
        return nil;
    }

    QAccessibleInterface* iface = get_qt_interface(self);
    if (!iface || ![parameter isKindOfClass:[NSDictionary class]])
        return [attribute hasSuffix:@"Count"] ? @0 : @[];

    NSDictionary* pred = (NSDictionary*)parameter;
    NSString* directionStr = pred[@"AXDirection"];
    id startEl = pred[@"AXStartElement"];
    NSNumber* limitNum = pred[@"AXResultsLimit"];
    int limit = limitNum ? [limitNum intValue] : -1;
    bool forward = ![directionStr isEqualToString:@"AXDirectionPrevious"];

    // Build flat DFS list of non-ignored elements
    NSMutableArray* allElements = [NSMutableArray array];

    // DFS stack
    struct StackEntry {
        QAccessibleInterface* iface;
    };
    QList<StackEntry> stack;
    for (int i = iface->childCount() - 1; i >= 0; --i) {
        if (auto* c = iface->child(i))
            stack.append({ c });
    }

    while (!stack.isEmpty()) {
        auto entry = stack.takeLast();
        auto* node = entry.iface;
        if (!node || !node->isValid())
            continue;

        QAccessible::Role r = node->role();
        bool is_leaf = is_leaf_role(r);

        // Skip container-only roles (e.g., ListItem) — navigate
        // directly to the content inside them.
        if (r != QAccessible::NoRole && !is_container_only_role(r)) {
            QAccessible::Id nid = QAccessible::uniqueId(node);
            if (nid) {
                Class macElementClass = NSClassFromString(@"QMacAccessibilityElement");
                if (macElementClass) {
                    id element = ((id (*)(Class, SEL, QAccessible::Id))objc_msgSend)(
                        macElementClass, NSSelectorFromString(@"elementWithId:"), nid);
                    if (element)
                        [allElements addObject:element];
                }
            }
        }

        // Don't descend into leaf-like roles
        if (!is_leaf) {
            for (int i = node->childCount() - 1; i >= 0; --i) {
                if (auto* c = node->child(i))
                    stack.append({ c });
            }
        }
    }

    // Find start position
    NSInteger startIdx = -1;
    if (startEl) {
        for (NSInteger i = 0; i < (NSInteger)[allElements count]; i++) {
            if (allElements[i] == startEl) {
                startIdx = i;
                break;
            }
        }
    }

    // Collect results
    NSMutableArray* results = [NSMutableArray array];
    if (forward) {
        for (NSInteger i = startIdx + 1;
            i < (NSInteger)[allElements count] && (limit < 0 || (int)[results count] < limit);
            i++)
            [results addObject:allElements[i]];
    } else {
        for (NSInteger i = startIdx - 1;
            i >= 0 && (limit < 0 || (int)[results count] < limit);
            i--)
            [results addObject:allElements[i]];
    }

    if ([attribute hasSuffix:@"Count"])
        return @([results count]);
    return results;
}

// Swizzled accessibilityFocusedUIElement: for AXWebArea, return the
// first meaningful child so VoiceOver starts in the web content
static IMP s_original_focused = nullptr;

static id swizzled_focused_element(id self, SEL _cmd)
{
    QAccessibleInterface* iface = get_qt_interface(self);
    if (!iface)
        return reinterpret_cast<id (*)(id, SEL)>(s_original_focused)(self, _cmd);

    // For both the WebDocument and its parent container, find the
    // first leaf element in the web content tree.
    QAccessibleInterface* webDoc = nullptr;
    if (iface->role() == QAccessible::WebDocument) {
        webDoc = iface;
    } else {
        // Check if any child is a WebDocument
        for (int i = 0; i < iface->childCount(); ++i) {
            if (auto* c = iface->child(i)) {
                if (c->role() == QAccessible::WebDocument) {
                    webDoc = c;
                    break;
                }
            }
        }
    }

    if (webDoc) {
        // DFS to find first non-container child
        QList<QAccessibleInterface*> stack;
        for (int i = webDoc->childCount() - 1; i >= 0; --i) {
            if (auto* c = webDoc->child(i))
                stack.append(c);
        }
        while (!stack.isEmpty()) {
            auto* node = stack.takeLast();
            if (!node || !node->isValid())
                continue;
            QAccessible::Role r = node->role();
            // Return the first leaf-like or meaningful element
            if (is_leaf_role(r)) {
                QAccessible::Id nid = QAccessible::uniqueId(node);
                if (nid) {
                    Class cls = NSClassFromString(@"QMacAccessibilityElement");
                    if (cls) {
                        id el = ((id (*)(Class, SEL, QAccessible::Id))objc_msgSend)(
                            cls, NSSelectorFromString(@"elementWithId:"), nid);
                        if (el)
                            return el;
                    }
                }
            }
            for (int i = node->childCount() - 1; i >= 0; --i) {
                if (auto* c = node->child(i))
                    stack.append(c);
            }
        }
    }
    return reinterpret_cast<id (*)(id, SEL)>(s_original_focused)(self, _cmd);
}

static void install_cocoa_swizzles()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    Class cls = NSClassFromString(@"QMacAccessibilityElement");
    if (!cls)
        return;

    Method m;

    m = class_getInstanceMethod(cls, @selector(accessibilitySubRole));
    if (m) {
        s_original_subrole = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_subrole);
    }

    m = class_getInstanceMethod(cls, @selector(accessibilityRoleDescription));
    if (m) {
        s_original_role_description = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_role_description);
    }

    m = class_getInstanceMethod(cls, @selector(accessibilityRole));
    if (m) {
        s_original_role = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_role);
    }

    m = class_getInstanceMethod(cls, @selector(accessibilityFocusedUIElement));
    if (m) {
        s_original_focused = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_focused_element);
    }

    m = class_getInstanceMethod(cls, @selector(accessibilityParameterizedAttributeNames));
    if (m) {
        s_original_param_names = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_param_names);
    }

    m = class_getInstanceMethod(cls, @selector(accessibilityAttributeValue:forParameter:));
    if (m) {
        s_original_param_value = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_param_value);
    }

    m = class_getInstanceMethod(cls, @selector(accessibilityIsIgnored));
    if (m) {
        s_original_is_ignored = method_getImplementation(m);
        method_setImplementation(m, (IMP)swizzled_is_ignored);
    }
}

void install_native_accessibility(QWidget*, WebView::AccessibilityTreeManager*)
{
    install_cocoa_swizzles();
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

    // Post announcement on the application — VoiceOver picks it up
    // regardless of which element is focused.
    NSAccessibilityPostNotificationWithUserInfo(
        [NSApp mainWindow],
        NSAccessibilityAnnouncementRequestedNotification,
        @{
            NSAccessibilityAnnouncementKey : announcement,
            NSAccessibilityPriorityKey : @(priority),
        });
}

void post_accessibility_focus_changed(QWidget* widget, i64 node_id)
{
    auto* view = static_cast<WebContentView*>(widget);

    auto* iface = view->accessibility_interface_for_node(node_id);
    if (!iface || !iface->isValid())
        return;

    QAccessible::Id axid = QAccessible::uniqueId(iface);
    if (!axid)
        return;

    // Ensure the QMacAccessibilityElement exists for this interface.
    // Without this, Qt's bridge can't find the element and logs
    // "Invalid child in QAccessibleEvent".
    Class macElementClass = NSClassFromString(@"QMacAccessibilityElement");
    if (!macElementClass)
        return;

    id element = ((id (*)(Class, SEL, QAccessible::Id))objc_msgSend)(
        macElementClass, NSSelectorFromString(@"elementWithId:"), axid);
    if (!element)
        return;

    NSAccessibilityPostNotification(element, NSAccessibilityFocusedUIElementChangedNotification);
}

void notify_accessibility_tree_loaded(QWidget* widget, WebView::AccessibilityTreeManager* manager)
{
    if (!manager || manager->is_empty())
        return;

    NSView* nsView = (__bridge NSView*)reinterpret_cast<void*>(widget->winId());
    if (!nsView || !nsView.window)
        return;

    // Make the web content view the Cocoa first responder.
    // VoiceOver follows the first responder chain for initial focus.
    [nsView.window makeFirstResponder:nsView];

    auto const* root = manager->root();
    if (!root)
        return;

    auto* view = static_cast<WebContentView*>(widget);
    auto* iface = view->accessibility_interface_for_node(root->id);
    if (!iface)
        return;

    QAccessible::Id axid = QAccessible::uniqueId(iface);
    if (!axid)
        return;

    Class macElementClass = NSClassFromString(@"QMacAccessibilityElement");
    if (!macElementClass)
        return;

    id rootElement = ((id (*)(Class, SEL, QAccessible::Id))objc_msgSend)(
        macElementClass, NSSelectorFromString(@"elementWithId:"), axid);
    if (!rootElement)
        return;

    NSAccessibilityPostNotification(rootElement, @"AXLoadComplete");
    NSAccessibilityHandleFocusChanged();
}

}
