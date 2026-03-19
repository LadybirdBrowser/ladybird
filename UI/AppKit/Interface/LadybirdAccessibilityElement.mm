/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#import <Interface/LadybirdAccessibilityElement.h>
#import <Interface/LadybirdWebView.h>

#include <LibWebView/AccessibilityNodeData.h>
#include <LibWebView/AccessibilityTreeManager.h>

static NSAccessibilityRole aria_role_to_ns_role(StringView role)
{
    if (role == "button"sv)
        return NSAccessibilityButtonRole;
    if (role == "link"sv)
        return NSAccessibilityLinkRole;
    if (role == "heading"sv)
        return NSAccessibilityGroupRole;
    if (role == "textbox"sv)
        return NSAccessibilityTextFieldRole;
    if (role == "checkbox"sv)
        return NSAccessibilityCheckBoxRole;
    if (role == "radio"sv)
        return NSAccessibilityRadioButtonRole;
    if (role == "combobox"sv)
        return NSAccessibilityComboBoxRole;
    if (role == "list"sv)
        return NSAccessibilityListRole;
    if (role == "listitem"sv)
        return NSAccessibilityGroupRole;
    if (role == "table"sv)
        return NSAccessibilityTableRole;
    if (role == "row"sv)
        return NSAccessibilityRowRole;
    if (role == "cell"sv || role == "gridcell"sv)
        return NSAccessibilityCellRole;
    if (role == "img"sv || role == "image"sv)
        return NSAccessibilityImageRole;
    if (role == "navigation"sv || role == "main"sv || role == "banner"sv
        || role == "complementary"sv || role == "contentinfo"sv || role == "search"sv
        || role == "form"sv || role == "region"sv)
        return NSAccessibilityGroupRole;
    if (role == "dialog"sv || role == "alertdialog"sv)
        return NSAccessibilityGroupRole;
    if (role == "progressbar"sv)
        return NSAccessibilityProgressIndicatorRole;
    if (role == "slider"sv)
        return NSAccessibilitySliderRole;
    if (role == "tab"sv)
        return NSAccessibilityRadioButtonRole;
    if (role == "tablist"sv)
        return NSAccessibilityTabGroupRole;
    if (role == "tabpanel"sv)
        return NSAccessibilityGroupRole;
    if (role == "menu"sv)
        return NSAccessibilityMenuRole;
    if (role == "menuitem"sv)
        return NSAccessibilityMenuItemRole;
    if (role == "menubar"sv)
        return NSAccessibilityMenuBarRole;
    if (role == "separator"sv)
        return NSAccessibilitySplitterRole;
    if (role == "alert"sv || role == "status"sv || role == "log"sv)
        return NSAccessibilityGroupRole;
    if (role == "text leaf"sv)
        return NSAccessibilityStaticTextRole;
    if (role == "document"sv)
        return @"AXWebArea";
    if (role == "generic"sv)
        return NSAccessibilityGroupRole;
    return NSAccessibilityGroupRole;
}

static NSAccessibilitySubrole aria_role_to_ns_subrole(StringView role)
{
    if (role == "dialog"sv || role == "alertdialog"sv)
        return NSAccessibilityDialogSubrole;
    if (role == "navigation"sv)
        return @"AXLandmarkNavigation";
    if (role == "main"sv)
        return @"AXLandmarkMain";
    if (role == "banner"sv)
        return @"AXLandmarkBanner";
    if (role == "complementary"sv)
        return @"AXLandmarkComplementary";
    if (role == "contentinfo"sv)
        return @"AXLandmarkContentInfo";
    if (role == "search"sv)
        return @"AXLandmarkSearch";
    if (role == "form"sv)
        return @"AXLandmarkForm";
    if (role == "region"sv)
        return @"AXLandmarkRegion";
    return nil;
}

static NSString* nsStringFromAK(AK::String const& string)
{
    if (string.is_empty())
        return nil;
    return [[NSString alloc] initWithBytes:string.bytes().data()
                                    length:string.bytes().size()
                                  encoding:NSUTF8StringEncoding];
}

@implementation LadybirdAccessibilityElement
{
    int64_t _nodeID;
    WebView::AccessibilityTreeManager const* _manager;
    __weak LadybirdWebView* _webView;
}

- (instancetype)initWithNodeID:(int64_t)nodeID
                       manager:(WebView::AccessibilityTreeManager const*)manager
                       webView:(LadybirdWebView*)webView
{
    self = [super init];
    if (self) {
        _nodeID = nodeID;
        _manager = manager;
        _webView = webView;
    }
    return self;
}

- (int64_t)nodeID
{
    return _nodeID;
}

- (WebView::AccessibilityNodeData const*)nodeData
{
    return _manager->node(_nodeID);
}

#pragma mark - NSAccessibility informal protocol

- (BOOL)accessibilityIsIgnored
{
    auto const* data = [self nodeData];
    if (!data)
        return YES;

    // Generic elements (div, span, body) are transparent to assistive technology
    // unless they have a name. Their children get promoted to the parent.
    if (data->role.bytes_as_string_view() == "generic"sv && data->name.is_empty())
        return YES;

    return NO;
}

- (NSArray*)accessibilityAttributeNames
{
    static NSArray* attributes = @[
        NSAccessibilityRoleAttribute,
        NSAccessibilitySubroleAttribute,
        NSAccessibilityRoleDescriptionAttribute,
        NSAccessibilityTitleAttribute,
        NSAccessibilityDescriptionAttribute,
        NSAccessibilityValueAttribute,
        NSAccessibilityHelpAttribute,
        NSAccessibilityParentAttribute,
        NSAccessibilityChildrenAttribute,
        NSAccessibilityWindowAttribute,
        NSAccessibilityTopLevelUIElementAttribute,
        NSAccessibilityPositionAttribute,
        NSAccessibilitySizeAttribute,
        NSAccessibilityFocusedAttribute,
        NSAccessibilityEnabledAttribute,
        @"AXLoaded",
    ];
    return attributes;
}

- (id)accessibilityAttributeValue:(NSString*)attribute
{
    auto const* data = [self nodeData];
    if ([attribute isEqualToString:NSAccessibilityRoleAttribute]) {
        if (!data)
            return NSAccessibilityGroupRole;
        return aria_role_to_ns_role(data->role.bytes_as_string_view());
    }

    if ([attribute isEqualToString:NSAccessibilitySubroleAttribute]) {
        if (!data)
            return nil;
        return aria_role_to_ns_subrole(data->role.bytes_as_string_view());
    }

    if ([attribute isEqualToString:NSAccessibilityRoleDescriptionAttribute]) {
        if (!data)
            return NSAccessibilityRoleDescription(NSAccessibilityGroupRole, nil);

        auto role_sv = data->role.bytes_as_string_view();
        // Landmarks need custom role descriptions because NSAccessibilityRoleDescription
        // doesn't recognize the AXLandmark* subrole strings.
        if (role_sv == "document"sv)
            return @"HTML content";
        if (role_sv == "banner"sv)
            return @"banner";
        if (role_sv == "navigation"sv)
            return @"navigation";
        if (role_sv == "main"sv)
            return @"main";
        if (role_sv == "complementary"sv)
            return @"complementary";
        if (role_sv == "contentinfo"sv)
            return @"content information";
        if (role_sv == "search"sv)
            return @"search";
        if (role_sv == "form"sv)
            return @"form";
        if (role_sv == "region"sv)
            return @"region";

        NSAccessibilityRole nsRole = [self accessibilityAttributeValue:NSAccessibilityRoleAttribute];
        NSAccessibilitySubrole nsSubrole = [self accessibilityAttributeValue:NSAccessibilitySubroleAttribute];
        return NSAccessibilityRoleDescription(nsRole, nsSubrole);
    }

    if ([attribute isEqualToString:NSAccessibilityTitleAttribute]) {
        if (!data)
            return nil;
        return nsStringFromAK(data->name);
    }

    if ([attribute isEqualToString:NSAccessibilityDescriptionAttribute]) {
        if (!data)
            return nil;
        return nsStringFromAK(data->name);
    }

    if ([attribute isEqualToString:NSAccessibilityHelpAttribute]) {
        if (!data)
            return nil;
        return nsStringFromAK(data->description);
    }

    if ([attribute isEqualToString:NSAccessibilityValueAttribute]) {
        if (!data)
            return nil;
        if (data->heading_level > 0)
            return @(data->heading_level);
        if (data->role.bytes_as_string_view() == "text leaf"sv)
            return nsStringFromAK(data->name);
        if (!data->value.is_empty())
            return nsStringFromAK(data->value);
        return nil;
    }

    if ([attribute isEqualToString:NSAccessibilityParentAttribute]) {
        if (!data || data->parent_id == -1)
            return NSAccessibilityUnignoredAncestor(_webView);
        id parent = [_webView accessibilityElementForNodeID:data->parent_id];
        return parent ? NSAccessibilityUnignoredAncestor(parent) : NSAccessibilityUnignoredAncestor(_webView);
    }

    if ([attribute isEqualToString:NSAccessibilityChildrenAttribute]) {
        if (!data)
            return @[];
        NSMutableArray* children = [NSMutableArray arrayWithCapacity:data->child_ids.size()];
        for (auto child_id : data->child_ids) {
            id child = [_webView accessibilityElementForNodeID:child_id];
            if (child)
                [children addObject:child];
        }
        return NSAccessibilityUnignoredChildren(children);
    }

    if ([attribute isEqualToString:NSAccessibilityWindowAttribute])
        return [_webView window];

    if ([attribute isEqualToString:NSAccessibilityTopLevelUIElementAttribute])
        return [_webView window];

    if ([attribute isEqualToString:NSAccessibilityPositionAttribute]) {
        if (!data)
            return [NSValue valueWithPoint:NSZeroPoint];
        NSRect viewRect = NSMakeRect(
            data->bounds.x(), data->bounds.y(),
            data->bounds.width(), data->bounds.height());
        NSRect screenRect = [_webView accessibilityScreenRectForViewRect:viewRect];
        return [NSValue valueWithPoint:screenRect.origin];
    }

    if ([attribute isEqualToString:NSAccessibilitySizeAttribute]) {
        if (!data)
            return [NSValue valueWithSize:NSZeroSize];
        NSRect viewRect = NSMakeRect(
            data->bounds.x(), data->bounds.y(),
            data->bounds.width(), data->bounds.height());
        NSRect screenRect = [_webView accessibilityScreenRectForViewRect:viewRect];
        return [NSValue valueWithSize:screenRect.size];
    }

    if ([attribute isEqualToString:NSAccessibilityFocusedAttribute]) {
        if (!data)
            return @NO;
        return data->is_focused ? @YES : @NO;
    }

    if ([attribute isEqualToString:@"AXLoaded"]) {
        if (data && data->role.bytes_as_string_view() == "document"sv)
            return @YES;
        return nil;
    }

    if ([attribute isEqualToString:NSAccessibilityEnabledAttribute]) {
        if (!data)
            return @YES;
        return data->is_disabled ? @NO : @YES;
    }

    return nil;
}

- (BOOL)accessibilityIsAttributeSettable:(NSString*)attribute
{
    if ([attribute isEqualToString:NSAccessibilityFocusedAttribute])
        return YES;
    return NO;
}

- (void)accessibilitySetValue:(id)value forAttribute:(NSString*)attribute
{
    // Phase 3: handle setting focus, values, etc.
}

- (NSArray*)accessibilityActionNames
{
    auto const* data = [self nodeData];
    if (!data)
        return @[];

    auto role = data->role.bytes_as_string_view();
    if (role == "button"sv || role == "link"sv || role == "checkbox"sv
        || role == "radio"sv || role == "menuitem"sv || role == "tab"sv)
        return @[ NSAccessibilityPressAction ];

    return @[];
}

- (NSString*)accessibilityActionDescription:(NSString*)action
{
    return NSAccessibilityActionDescription(action);
}

- (void)accessibilityPerformAction:(NSString*)action
{
    // Phase 3: handle press, increment, decrement, etc.
}

- (id)accessibilityHitTest:(NSPoint)screenPoint
{
    NSRect viewRect = [_webView accessibilityViewRectForScreenPoint:screenPoint];
    auto point = Gfx::IntPoint { static_cast<int>(viewRect.origin.x), static_cast<int>(viewRect.origin.y) };

    auto const* hit = _manager->hit_test(point);
    if (hit)
        return [_webView accessibilityElementForNodeID:hit->id];
    return self;
}

- (id)accessibilityFocusedUIElement
{
    auto const* root = _manager->root();
    if (!root)
        return nil;

    // BFS to find the focused node, or the first non-ignored leaf.
    id first_non_ignored = nil;
    Vector<i64> queue;
    queue.append(root->id);
    while (!queue.is_empty()) {
        auto id = queue.take_first();
        auto const* node = _manager->node(id);
        if (!node)
            continue;
        if (node->is_focused)
            return [_webView accessibilityElementForNodeID:id];
        if (!first_non_ignored) {
            auto role = node->role.bytes_as_string_view();
            if (role != "generic"sv && role != "document"sv)
                first_non_ignored = [_webView accessibilityElementForNodeID:id];
        }
        for (auto child_id : node->child_ids)
            queue.append(child_id);
    }
    return first_non_ignored ? first_non_ignored : self;
}

#pragma mark - Modern NSAccessibility property overrides

// The accessibility system may query via modern property API rather than
// the informal protocol. These delegate to accessibilityAttributeValue:.

- (NSAccessibilityRole)accessibilityRole
{
    return [self accessibilityAttributeValue:NSAccessibilityRoleAttribute];
}

- (NSAccessibilitySubrole)accessibilitySubrole
{
    return [self accessibilityAttributeValue:NSAccessibilitySubroleAttribute];
}

- (NSString*)accessibilityRoleDescription
{
    return [self accessibilityAttributeValue:NSAccessibilityRoleDescriptionAttribute];
}

- (NSString*)accessibilityLabel
{
    return [self accessibilityAttributeValue:NSAccessibilityDescriptionAttribute];
}

- (NSString*)accessibilityTitle
{
    return [self accessibilityAttributeValue:NSAccessibilityTitleAttribute];
}

- (id)accessibilityValue
{
    return [self accessibilityAttributeValue:NSAccessibilityValueAttribute];
}

- (NSString*)accessibilityHelp
{
    return [self accessibilityAttributeValue:NSAccessibilityHelpAttribute];
}

- (id)accessibilityParent
{
    return [self accessibilityAttributeValue:NSAccessibilityParentAttribute];
}

- (NSArray*)accessibilityChildren
{
    return [self accessibilityAttributeValue:NSAccessibilityChildrenAttribute];
}

- (NSRect)accessibilityFrame
{
    NSValue* pos = [self accessibilityAttributeValue:NSAccessibilityPositionAttribute];
    NSValue* size = [self accessibilityAttributeValue:NSAccessibilitySizeAttribute];
    if (!pos || !size)
        return NSZeroRect;
    return NSMakeRect([pos pointValue].x, [pos pointValue].y,
        [size sizeValue].width, [size sizeValue].height);
}

- (BOOL)isAccessibilityFocused
{
    return [[self accessibilityAttributeValue:NSAccessibilityFocusedAttribute] boolValue];
}

- (BOOL)isAccessibilityEnabled
{
    return [[self accessibilityAttributeValue:NSAccessibilityEnabledAttribute] boolValue];
}

- (BOOL)isAccessibilityElement
{
    return YES;
}

@end

#pragma clang diagnostic pop
