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

#include <ApplicationServices/ApplicationServices.h>

// Text marker: wraps a node ID for VoiceOver web content navigation.
static AXTextMarkerRef createTextMarker(i64 node_id)
{
    return AXTextMarkerCreate(kCFAllocatorDefault, (UInt8 const*)&node_id, sizeof(node_id));
}

static i64 nodeIDFromTextMarker(AXTextMarkerRef marker)
{
    if (!marker || AXTextMarkerGetLength(marker) != sizeof(i64))
        return -1;
    i64 node_id;
    memcpy(&node_id, AXTextMarkerGetBytePtr(marker), sizeof(node_id));
    return node_id;
}

static bool is_ignored_role(StringView role, AK::String const& name)
{
    return (role == "generic"sv && name.is_empty())
        || (role == "paragraph"sv && name.is_empty());
}

static bool is_navigation_leaf_role(StringView role)
{
    return role == "link"sv || role == "button"sv
        || role == "heading"sv || role == "menuitem"sv
        || role == "tab"sv || role == "radio"sv
        || role == "checkbox"sv || role == "img"sv
        || role == "image"sv || role == "text leaf"sv;
}

static NSAccessibilityRole aria_role_to_ns_role(StringView role)
{
    if (role == "button"sv)
        return NSAccessibilityButtonRole;
    if (role == "link"sv)
        return NSAccessibilityLinkRole;
    if (role == "heading"sv)
        return @"AXHeading";
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
        return @"AXGroup";
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

- (BOOL)shouldGroupAccessibilityChildren
{
    return NO;
}

- (BOOL)accessibilityIsIgnored
{
    auto const* data = [self nodeData];
    if (!data)
        return YES;

    auto role = data->role.bytes_as_string_view();

    // Generic elements (div, span, body) are transparent to assistive technology
    // unless they have a name. Their children get promoted to the parent.
    if (role == "generic"sv && data->name.is_empty())
        return YES;

    // Paragraph containers are transparent — VoiceOver navigates to
    // the text content inside them directly.
    if (role == "paragraph"sv && data->name.is_empty())
        return YES;

    return NO;
}

- (NSArray*)accessibilityAttributeNames
{
    static NSArray* baseAttributes = @[
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
        @"AXElementBusy",
        @"AXSelected",
        @"AXVisited",
        @"AXBlockQuoteLevel",
        @"AXChildrenInNavigationOrder",
        NSAccessibilityURLAttribute,
    ];

    // Text markers and loading attributes belong only on the
    // document root element, not on structural containers.
    static NSArray* documentAttributes = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        documentAttributes = [baseAttributes arrayByAddingObjectsFromArray:@[
            @"AXStartTextMarker",
            @"AXEndTextMarker",
            @"AXLoaded",
            @"AXLoadingProgress",
        ]];
    });

    auto const* data = [self nodeData];
    if (data && data->role.bytes_as_string_view() == "document"sv)
        return documentAttributes;
    return baseAttributes;
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
        if (role_sv == "heading"sv)
            return @"heading";
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
            return _webView;

        // Walk up past ignored ancestors to find the nearest visible parent.
        i64 parent_id = data->parent_id;
        while (parent_id != -1) {
            auto const* parent_data = _manager->node(parent_id);
            if (!parent_data)
                return _webView;

            if (!is_ignored_role(parent_data->role.bytes_as_string_view(), parent_data->name))
                return [_webView accessibilityElementForNodeID:parent_id];

            parent_id = parent_data->parent_id;
        }
        return _webView;
    }

    if ([attribute isEqualToString:NSAccessibilityChildrenAttribute]) {
        if (!data)
            return @[];
        NSMutableArray* children = [NSMutableArray array];
        [self collectUnignoredChildren:children];
        return children;
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

    if ([attribute isEqualToString:@"AXChildrenInNavigationOrder"])
        return [self accessibilityAttributeValue:NSAccessibilityChildrenAttribute];

    if ([attribute isEqualToString:NSAccessibilityURLAttribute])
        return nil;

    if ([attribute isEqualToString:@"AXStartTextMarker"]) {
        if (!data)
            return nil;
        auto const* node = data;
        while (!node->child_ids.is_empty()) {
            auto const* child = _manager->node(node->child_ids[0]);
            if (!child)
                break;
            node = child;
            if (!is_ignored_role(node->role.bytes_as_string_view(), node->name))
                break;
        }
        return CFBridgingRelease(createTextMarker(node->id));
    }
    if ([attribute isEqualToString:@"AXEndTextMarker"]) {
        if (!data)
            return nil;
        auto const* node = data;
        while (!node->child_ids.is_empty()) {
            auto const* child = _manager->node(node->child_ids.last());
            if (!child)
                break;
            node = child;
            if (!is_ignored_role(node->role.bytes_as_string_view(), node->name))
                break;
        }
        return CFBridgingRelease(createTextMarker(node->id));
    }
    if ([attribute isEqualToString:@"AXElementBusy"])
        return @NO;
    if ([attribute isEqualToString:@"AXSelected"])
        return @NO;
    if ([attribute isEqualToString:@"AXVisited"])
        return @NO;
    if ([attribute isEqualToString:@"AXBlockQuoteLevel"])
        return @0;

    if ([attribute isEqualToString:@"AXLoaded"]) {
        if (data && data->role.bytes_as_string_view() == "document"sv)
            return @YES;
        return nil;
    }

    if ([attribute isEqualToString:@"AXLoadingProgress"]) {
        if (data && data->role.bytes_as_string_view() == "document"sv)
            return @1.0;
        return nil;
    }

    if ([attribute isEqualToString:NSAccessibilityEnabledAttribute]) {
        if (!data)
            return @YES;
        return data->is_disabled ? @NO : @YES;
    }

    return nil;
}

- (void)collectUnignoredChildren:(NSMutableArray*)result
{
    auto const* data = [self nodeData];
    if (!data)
        return;

    for (auto child_id : data->child_ids) {
        auto const* child_data = _manager->node(child_id);
        if (!child_data) {
            continue;
        }

        if (is_ignored_role(child_data->role.bytes_as_string_view(), child_data->name)) {
            // Promote this element's children directly.
            id child_element = [_webView accessibilityElementForNodeID:child_id];
            if ([child_element isKindOfClass:[LadybirdAccessibilityElement class]])
                [(LadybirdAccessibilityElement*)child_element collectUnignoredChildren:result];
        } else {
            id child_element = [_webView accessibilityElementForNodeID:child_id];
            if (child_element)
                [result addObject:child_element];
        }
    }
}

- (NSArray*)accessibilityParameterizedAttributeNames
{
    static NSArray* baseParamAttributes = @[
        @"AXUIElementsForSearchPredicate",
        @"AXUIElementCountForSearchPredicate",
        @"AXIndexForChildUIElement",
    ];

    // Text marker parameterized attributes belong only on the
    // document root element.
    static NSArray* documentParamAttributes = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        documentParamAttributes = [baseParamAttributes arrayByAddingObjectsFromArray:@[
            @"AXNextTextMarkerForTextMarker",
            @"AXPreviousTextMarkerForTextMarker",
            @"AXUIElementForTextMarker",
            @"AXTextMarkerRangeForUIElement",
            @"AXLengthForTextMarkerRange",
            @"AXStringForTextMarkerRange",
            @"AXAttributedStringForTextMarkerRange",
            @"AXTextMarkerForPosition",
        ]];
    });

    auto const* data = [self nodeData];
    if (data && data->role.bytes_as_string_view() == "document"sv)
        return documentParamAttributes;
    return baseParamAttributes;
}

- (id)accessibilityAttributeValue:(NSString*)attribute forParameter:(id)parameter
{
    if ([attribute isEqualToString:@"AXUIElementsForSearchPredicate"] ||
        [attribute isEqualToString:@"AXUIElementCountForSearchPredicate"]) {
        if (![parameter isKindOfClass:[NSDictionary class]])
            return [attribute hasSuffix:@"Count"] ? @0 : @[];

        NSDictionary* pred = (NSDictionary*)parameter;
        NSString* directionStr = pred[@"AXDirection"];
        id startEl = pred[@"AXStartElement"];
        NSNumber* limitNum = pred[@"AXResultsLimit"];
        int limit = limitNum ? [limitNum intValue] : -1;
        bool forward = ![directionStr isEqualToString:@"AXDirectionPrevious"];

        // Build flat depth-first list of all non-ignored elements
        // within this element's subtree
        NSMutableArray* allElements = [NSMutableArray array];
        auto const* selfData = [self nodeData];
        if (!selfData)
            return [attribute hasSuffix:@"Count"] ? @0 : @[];

        // Depth-first traversal collecting non-ignored elements
        Vector<i64> stack;
        // Push children in reverse so first child is processed first
        for (int i = (int)selfData->child_ids.size() - 1; i >= 0; --i)
            stack.append(selfData->child_ids[i]);

        while (!stack.is_empty()) {
            i64 nid = stack.take_last();
            auto const* node = _manager->node(nid);
            if (!node)
                continue;
            bool ignored = is_ignored_role(node->role.bytes_as_string_view(), node->name);
            if (!ignored)
                [allElements addObject:[_webView accessibilityElementForNodeID:nid]];
            // Only descend into children of container elements.
            // Leaf-like roles (links, buttons, headings) have their
            // text content in their accessible name already.
            auto node_role = node->role.bytes_as_string_view();
            if (ignored || !is_navigation_leaf_role(node_role)) {
                for (int i = (int)node->child_ids.size() - 1; i >= 0; --i)
                    stack.append(node->child_ids[i]);
            }
        }

        // Find start position
        NSInteger startIdx = -1;
        if (startEl && [startEl isKindOfClass:[LadybirdAccessibilityElement class]]) {
            i64 startID = ((LadybirdAccessibilityElement*)startEl).nodeID;
            for (NSInteger i = 0; i < (NSInteger)[allElements count]; i++) {
                id el = allElements[i];
                if ([el isKindOfClass:[LadybirdAccessibilityElement class]]
                    && ((LadybirdAccessibilityElement*)el).nodeID == startID) {
                    startIdx = i;
                    break;
                }
            }
        }

        // When navigating forward past a container element,
        // skip its descendants to avoid re-traversing them.
        // In pre-order DFS, descendants form a contiguous block
        // right after their ancestor.
        NSInteger effectiveStart = startIdx;
        if (forward && startIdx >= 0
            && startEl && [startEl isKindOfClass:[LadybirdAccessibilityElement class]]) {
            i64 startID = ((LadybirdAccessibilityElement*)startEl).nodeID;
            for (NSInteger i = startIdx + 1; i < (NSInteger)[allElements count]; i++) {
                id el = allElements[i];
                if (![el isKindOfClass:[LadybirdAccessibilityElement class]])
                    break;
                i64 elID = ((LadybirdAccessibilityElement*)el).nodeID;
                // Walk up the parent chain to check if this is a descendant
                bool descendant = false;
                auto const* n = _manager->node(elID);
                while (n && n->parent_id != -1) {
                    if (n->parent_id == startID) {
                        descendant = true;
                        break;
                    }
                    n = _manager->node(n->parent_id);
                }
                if (!descendant)
                    break;
                effectiveStart = i;
            }
        }

        // Collect results
        NSMutableArray* results = [NSMutableArray array];
        if (forward) {
            for (NSInteger i = effectiveStart + 1;
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

    if ([attribute isEqualToString:@"AXIndexForChildUIElement"]) {
        NSArray* children = [self accessibilityAttributeValue:NSAccessibilityChildrenAttribute];
        NSUInteger idx = [children indexOfObjectIdenticalTo:parameter];
        if (idx != NSNotFound)
            return @(idx);
        return nil;
    }

    if ([attribute isEqualToString:@"AXUIElementForTextMarker"]) {
        i64 nid = nodeIDFromTextMarker((__bridge AXTextMarkerRef)parameter);
        return nid != -1 ? [_webView accessibilityElementForNodeID:nid] : nil;
    }

    if ([attribute isEqualToString:@"AXNextTextMarkerForTextMarker"]
        || [attribute isEqualToString:@"AXPreviousTextMarkerForTextMarker"])
        return nil;

    if ([attribute isEqualToString:@"AXTextMarkerRangeForUIElement"]) {
        auto const* data = [self nodeData];
        if (!data)
            return nil;
        auto start = createTextMarker(data->id);
        auto end = createTextMarker(data->id);
        auto range = AXTextMarkerRangeCreate(kCFAllocatorDefault, start, end);
        CFRelease(start);
        CFRelease(end);
        return (__bridge id)range;
    }

    if ([attribute isEqualToString:@"AXLengthForTextMarkerRange"])
        return @0;
    if ([attribute isEqualToString:@"AXStringForTextMarkerRange"])
        return @"";
    if ([attribute isEqualToString:@"AXAttributedStringForTextMarkerRange"])
        return [[NSAttributedString alloc] initWithString:@""];
    if ([attribute isEqualToString:@"AXTextMarkerForPosition"])
        return nil;

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
    if ([attribute isEqualToString:NSAccessibilityFocusedAttribute] && [value boolValue])
        [_webView performAccessibilityAction:@"focus" forNodeID:_nodeID];
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
    if ([action isEqualToString:NSAccessibilityPressAction])
        [_webView performAccessibilityAction:@"press" forNodeID:_nodeID];
}

- (id)accessibilityHitTest:(NSPoint)screenPoint
{
    NSRect viewRect = [_webView accessibilityViewRectForScreenPoint:screenPoint];
    auto point = Gfx::IntPoint { static_cast<int>(viewRect.origin.x), static_cast<int>(viewRect.origin.y) };

    auto const* hit = _manager->hit_test(point);
    if (!hit)
        return self;

    // Walk up past ignored elements to return a non-ignored one.
    while (hit && is_ignored_role(hit->role.bytes_as_string_view(), hit->name)) {
        if (hit->parent_id == -1)
            return self;
        hit = _manager->node(hit->parent_id);
    }

    if (hit)
        return [_webView accessibilityElementForNodeID:hit->id];
    return self;
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

- (NSArray*)accessibilityChildrenInNavigationOrder
{
    return [self accessibilityChildren];
}

- (BOOL)isAccessibilityElement
{
    return ![self accessibilityIsIgnored];
}

- (id)accessibilityWindow
{
    return [_webView window];
}

- (id)accessibilityTopLevelUIElement
{
    return [_webView window];
}

@end

#pragma clang diagnostic pop
