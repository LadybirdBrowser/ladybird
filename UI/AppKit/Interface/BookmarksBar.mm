/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/Menu.h>

#import <Interface/BookmarkFolder.h>
#import <Interface/BookmarksBar.h>
#import <Interface/Menu.h>
#import <Utilities/Conversions.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const BOOKMARK_BUTTON_MAX_WIDTH = 150;
static constexpr CGFloat const BOOKMARK_ITEM_SPACING = 2;
static constexpr CGFloat const BOOKMARK_LEADING_INSET = 8;
static constexpr CGFloat const OVERFLOW_TRAILING_INSET = 4;

static Optional<WebView::Menu&> find_bookmark_folder_by_id(WebView::Menu& menu, StringView id)
{
    for (auto& item : menu.items()) {
        auto* submenu_ptr = item.get_pointer<NonnullRefPtr<WebView::Menu>>();
        if (!submenu_ptr)
            continue;

        auto& submenu = **submenu_ptr;

        if (auto submenu_id = submenu.properties().get("id"sv); submenu_id.has_value() && *submenu_id == id)
            return submenu;

        if (auto descendant = find_bookmark_folder_by_id(submenu, id); descendant.has_value())
            return descendant;
    }

    return {};
}

@interface BookmarksBar ()

@property (nonatomic, strong) NSStackView* bookmark_items;
@property (nonatomic, strong) BookmarkFolderPopover* bookmark_folder_popover;
@property (nonatomic, weak) NSButton* active_bookmark_folder_button;

@property (nonatomic, strong) NSButton* overflow_button;
@property (nonatomic, strong) NSMenu* overflow_menu;

@property (nonatomic, strong) NSMenu* bookmarks_bar_context_menu;
@property (nonatomic, strong) NSMenu* bookmark_context_menu;
@property (nonatomic, strong) NSMenu* bookmark_folder_context_menu;

@property (nonatomic, strong, readwrite) NSString* selected_bookmark_menu_item_id;
@property (nonatomic, strong, readwrite) NSString* selected_bookmark_menu_target_folder_id;

@end

@implementation BookmarksBar

@synthesize overflow_menu = _overflow_menu;
@synthesize bookmarks_bar_context_menu = _bookmarks_bar_context_menu;
@synthesize bookmark_context_menu = _bookmark_context_menu;
@synthesize bookmark_folder_context_menu = _bookmark_folder_context_menu;

- (instancetype)init
{
    if (self = [super init]) {
        self.bookmark_items = [[NSStackView alloc] init];
        [self.bookmark_items setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
        [self.bookmark_items setSpacing:BOOKMARK_ITEM_SPACING];
        [self.bookmark_items setEdgeInsets:NSEdgeInsets { 0, BOOKMARK_LEADING_INSET, 0, 0 }];
        [self.bookmark_items setAlignment:NSLayoutAttributeCenterY];
        [self.bookmark_items setTranslatesAutoresizingMaskIntoConstraints:NO];

        [self.bookmark_items setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                                      forOrientation:NSLayoutConstraintOrientationHorizontal];
        [self.bookmark_items setClippingResistancePriority:NSLayoutPriorityDefaultLow
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];

        self.overflow_button = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"chevron.right" accessibilityDescription:@""]
                                                  target:self
                                                  action:@selector(openOverflowMenu:)];
        [self.overflow_button setBezelStyle:NSBezelStyleAccessoryBarAction];
        [self.overflow_button setShowsBorderOnlyWhileMouseInside:YES];
        [self.overflow_button setTranslatesAutoresizingMaskIntoConstraints:NO];
        [self.overflow_button setHidden:YES];

        [self.overflow_button setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                                       forOrientation:NSLayoutConstraintOrientationHorizontal];

        [self addSubview:self.bookmark_items];
        [self addSubview:self.overflow_button];

        [NSLayoutConstraint activateConstraints:@[
            [[self.bookmark_items leadingAnchor] constraintEqualToAnchor:[self leadingAnchor]],
            [[self.bookmark_items topAnchor] constraintEqualToAnchor:[self topAnchor]],
            [[self.bookmark_items bottomAnchor] constraintEqualToAnchor:[self bottomAnchor]],

            [[self.overflow_button trailingAnchor] constraintEqualToAnchor:[self trailingAnchor]
                                                                  constant:-OVERFLOW_TRAILING_INSET],
            [[self.overflow_button centerYAnchor] constraintEqualToAnchor:[self centerYAnchor]],

            [[self.bookmark_items trailingAnchor] constraintLessThanOrEqualToAnchor:[self.overflow_button leadingAnchor]
                                                                           constant:-BOOKMARK_ITEM_SPACING],
        ]];

        [self setClipsToBounds:YES];

        [self rebuild];
    }

    return self;
}

- (void)rebuild
{
    [self closeBookmarkFolders];
    [self.bookmark_items setSubviews:@[]];

    auto set_button_properties = [](NSButton* button, StringView title) {
        [button setTitle:Ladybird::string_to_ns_string(title)];
        [button setImagePosition:NSImageLeading];

        [button setBezelStyle:NSBezelStyleAccessoryBarAction];
        [button setShowsBorderOnlyWhileMouseInside:YES];

        [button setFont:[NSFont systemFontOfSize:12]];
        [button setControlSize:NSControlSizeRegular];

        [[button cell] setLineBreakMode:NSLineBreakByTruncatingTail];
        [[button widthAnchor] constraintLessThanOrEqualToConstant:BOOKMARK_BUTTON_MAX_WIDTH].active = YES;
    };

    for (auto const& item : WebView::Application::the().bookmarks_menu().items()) {
        auto* button = item.visit(
            [&](NonnullRefPtr<WebView::Action> const& bookmark) -> NSButton* {
                if (bookmark->id() != WebView::ActionID::BookmarkItem)
                    return nil;

                auto* button = Ladybird::create_application_button(bookmark);
                set_button_properties(button, bookmark->text());

                return button;
            },
            [&](NonnullRefPtr<WebView::Menu> const& folder) -> NSButton* {
                auto* button = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"folder" accessibilityDescription:@""]
                                                  target:self
                                                  action:@selector(openFolder:)];
                set_button_properties(button, folder->title());

                Ladybird::add_control_properties(button, *folder);
                return button;
            },
            [](WebView::Separator) -> NSButton* {
                return nil;
            });

        if (button) {
            [self.bookmark_items addView:button inGravity:NSStackViewGravityLeading];
        }
    }

    [self setNeedsLayout:YES];
}

- (NSMenu*)overflow_menu
{
    if (!_overflow_menu) {
        _overflow_menu = [[NSMenu alloc] init];

        NSArray<NSView*>* buttons = [self.bookmark_items views];
        size_t button_index = 0;

        for (auto const& item : WebView::Application::the().bookmarks_menu().items()) {
            auto is_bookmark_item = item.visit(
                [](NonnullRefPtr<WebView::Action> const& action) { return action->id() == WebView::ActionID::BookmarkItem; },
                [](NonnullRefPtr<WebView::Menu> const&) { return true; },
                [](WebView::Separator) { return false; });

            if (!is_bookmark_item)
                continue;

            if (button_index < [buttons count] && [buttons[button_index] isHidden]) {
                item.visit(
                    [&](NonnullRefPtr<WebView::Action> const& action) {
                        [_overflow_menu addItem:Ladybird::create_application_menu_item(action)];
                    },
                    [&](NonnullRefPtr<WebView::Menu> const& folder) {
                        auto* folder_item = [[NSMenuItem alloc] initWithTitle:Ladybird::string_to_ns_string(folder->title())
                                                                       action:nil
                                                                keyEquivalent:@""];

                        auto* submenu = Ladybird::create_application_menu(folder);
                        [folder_item setSubmenu:submenu];

                        [_overflow_menu addItem:folder_item];
                    },
                    [](WebView::Separator) {});
            }

            ++button_index;
        }
    }

    return _overflow_menu;
}

- (void)openOverflowMenu:(NSButton*)sender
{
    if ([self.overflow_menu numberOfItems] > 0) {
        [self.overflow_menu popUpMenuPositioningItem:nil
                                          atLocation:NSMakePoint(0, [sender bounds].size.height)
                                              inView:sender];
    }
}

- (void)openFolder:(NSButton*)sender
{
    auto* item_id = Ladybird::get_control_property(sender, @"id");
    if (!item_id)
        return;

    auto id = Ladybird::ns_string_to_string(item_id);
    auto folder = find_bookmark_folder_by_id(WebView::Application::the().bookmarks_menu(), id);
    if (!folder.has_value())
        return;

    [self openFolderMenu:*folder anchoredToView:sender preferredEdge:NSRectEdgeMaxY];
}

- (void)openFolderMenu:(WebView::Menu&)menu
        anchoredToView:(NSView*)view
         preferredEdge:(NSRectEdge)preferredEdge
{
    if (menu.size() == 0)
        return;

    [self closeBookmarkFolders];

    if ([view isKindOfClass:[NSButton class]]) {
        self.active_bookmark_folder_button = (NSButton*)view;
        [self.active_bookmark_folder_button setShowsBorderOnlyWhileMouseInside:NO];
        [self.active_bookmark_folder_button highlight:YES];
    }

    self.bookmark_folder_popover = [[BookmarkFolderPopover alloc] init:menu bookmarksBar:self parentFolder:nil];
    [self.bookmark_folder_popover showRelativeToView:view preferredEdge:preferredEdge];
}

- (void)closeBookmarkFolders
{
    [self.bookmark_folder_popover close];
    self.bookmark_folder_popover = nil;

    [self clearActiveBookmarkFolder];
}

- (void)bookmarkFolderDidClose:(BookmarkFolderPopover*)folder
{
    if (self.bookmark_folder_popover == folder)
        self.bookmark_folder_popover = nil;

    [self clearActiveBookmarkFolder];
}

- (void)clearActiveBookmarkFolder
{
    if (!self.active_bookmark_folder_button)
        return;

    [self.active_bookmark_folder_button highlight:NO];
    [self.active_bookmark_folder_button setShowsBorderOnlyWhileMouseInside:YES];
    self.active_bookmark_folder_button = nil;
}

- (void)showContextMenu:(id)control event:(NSEvent*)event
{
    self.selected_bookmark_menu_item_id = Ladybird::get_control_property(control, @"id");
    self.selected_bookmark_menu_target_folder_id = Ladybird::get_control_property(control, @"target_folder_id");

    if (auto* type = Ladybird::get_control_property(control, @"type"); [type isEqualToString:@"bookmark"])
        [NSMenu popUpContextMenu:self.bookmark_context_menu withEvent:event forView:control];
    else if ([type isEqualToString:@"folder"])
        [NSMenu popUpContextMenu:self.bookmark_folder_context_menu withEvent:event forView:control];
}

- (void)showContextMenuForEvent:(NSEvent*)event
{
    if (auto* button = [self bookmarkButtonForEvent:event]) {
        [self showContextMenu:button event:event];
        return;
    }

    self.selected_bookmark_menu_item_id = @"";
    self.selected_bookmark_menu_target_folder_id = nil;

    [NSMenu popUpContextMenu:self.bookmarks_bar_context_menu withEvent:event forView:self];
}

- (NSView*)bookmarkButtonForEvent:(NSEvent*)event
{
    auto location = [event locationInWindow];

    for (NSView* button in [self.bookmark_items views]) {
        if ([button isHidden])
            continue;

        auto point = [button convertPoint:location fromView:nil];
        if (!NSPointInRect(point, [button bounds]))
            continue;

        return button;
    }

    return nil;
}

- (void)rightMouseDown:(NSEvent*)event
{
    [self showContextMenuForEvent:event];
}

- (void)mouseDown:(NSEvent*)event
{
    if ([event modifierFlags] & NSEventModifierFlagControl) {
        [self showContextMenuForEvent:event];
        return;
    }

    if ([event modifierFlags] & NSEventModifierFlagCommand) {
        if (auto* button = [self bookmarkButtonForEvent:event]) {
            if (auto* type = Ladybird::get_control_property(button, @"type"); [type isEqualToString:@"bookmark"]) {
                auto bookmark_id = Ladybird::ns_string_to_string(Ladybird::get_control_property(button, @"id"));
                auto activate_tab = ([event modifierFlags] & NSEventModifierFlagShift) ? Web::HTML::ActivateTab::No : Web::HTML::ActivateTab::Yes;

                WebView::Application::the().open_bookmark_in_new_tab(bookmark_id, activate_tab);
            }
        }

        return;
    }

    [super mouseDown:event];
}

- (nullable NSView*)hitTest:(NSPoint)point
{
    auto* hit = [super hitTest:point];
    if (!hit)
        return nil;

    // Route cmd and ctrl+left clicks to the BookmarksBar so we can handle the action appropriately, rather than letting
    // NSButton swallow the event for its own click tracking.
    auto* event = [NSApp currentEvent];
    if ([event type] == NSEventTypeLeftMouseDown && ([event modifierFlags] & (NSEventModifierFlagControl | NSEventModifierFlagCommand)))
        return self;

    return hit;
}

- (NSMenu*)bookmarks_bar_context_menu
{
    if (!_bookmarks_bar_context_menu)
        _bookmarks_bar_context_menu = Ladybird::create_application_menu(WebView::Application::the().bookmarks_bar_context_menu());
    return _bookmarks_bar_context_menu;
}

- (NSMenu*)bookmark_context_menu
{
    if (!_bookmark_context_menu)
        _bookmark_context_menu = Ladybird::create_application_menu(WebView::Application::the().bookmark_context_menu());
    return _bookmark_context_menu;
}

- (NSMenu*)bookmark_folder_context_menu
{
    if (!_bookmark_folder_context_menu)
        _bookmark_folder_context_menu = Ladybird::create_application_menu(WebView::Application::the().bookmark_folder_context_menu());
    return _bookmark_folder_context_menu;
}

- (void)layout
{
    [super layout];

    auto overflow_width = [self.overflow_button fittingSize].width + OVERFLOW_TRAILING_INSET + BOOKMARK_ITEM_SPACING;
    auto total_width = [self bounds].size.width;

    // First pass: check if any buttons overflow without reserving space for the overflow button.
    auto used_width = BOOKMARK_LEADING_INSET;
    for (NSView* button in [self.bookmark_items views]) {
        used_width += [button fittingSize].width + BOOKMARK_ITEM_SPACING;
    }

    auto has_overflow = used_width > total_width;
    auto available_width = has_overflow ? total_width - overflow_width : total_width;

    // Second pass: hide buttons that don't fully fit with overflow button.
    used_width = BOOKMARK_LEADING_INSET;
    for (NSView* button in [self.bookmark_items views]) {
        used_width += [button fittingSize].width + BOOKMARK_ITEM_SPACING;
        [button setHidden:(used_width > available_width)];
    }

    [self.overflow_button setHidden:!has_overflow];
    self.overflow_menu = nil;
}

@end
