/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWebView/Menu.h>

#import <Interface/BookmarkFolder.h>
#import <Interface/BookmarksBar.h>
#import <Interface/Menu.h>
#import <Utilities/Conversions.h>
#import <objc/runtime.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const BOOKMARK_FOLDER_WIDTH = 200;
static constexpr CGFloat const BOOKMARK_FOLDER_MAX_HEIGHT = 360;
static constexpr CGFloat const BOOKMARK_FOLDER_ROW_HEIGHT = 26;

static constexpr CGFloat const BOOKMARK_FOLDER_ICON_SIZE = 16;
static constexpr CGFloat const BOOKMARK_FOLDER_CHEVRON_WIDTH = 12;

static constexpr CGFloat const BOOKMARK_FOLDER_INSET = 8;
static constexpr CGFloat const BOOKMARK_FOLDER_HORIZONTAL_PADDING = 4;
static constexpr CGFloat const BOOKMARK_FOLDER_VERTICAL_PADDING = 6;

static constexpr CGFloat const BOOKMARK_FOLDER_HORIZONTAL_OVERLAP = 24;
static constexpr CGFloat const BOOKMARK_FOLDER_SUBMENU_LEFT_SHIFT = 18;
static constexpr CGFloat const BOOKMARK_FOLDER_ROOT_VERTICAL_SHIFT = 10;

@interface BookmarkFolderItemView : NSView

@property (nonatomic, weak) BookmarksBar* bookmarks_bar;
@property (nonatomic, weak) BookmarkFolderPopover* parent_folder;

@property (nonatomic, strong) NSImageView* icon_view;
@property (nonatomic, strong) NSTextField* title_label;
@property (nonatomic, strong) NSImageView* chevron_view;
@property (nonatomic, strong) NSTrackingArea* tracking_area;

@end

@implementation BookmarkFolderItemView
{
    WeakPtr<WebView::Action> m_action;
    WeakPtr<WebView::Menu> m_menu;
    BOOL m_hovered;
}

- (instancetype)initForBookmark:(WebView::Action&)action
                   bookmarksBar:(BookmarksBar*)bookmarks_bar
                   parentFolder:(BookmarkFolderPopover*)parent_folder
{
    if (self = [super initWithFrame:NSZeroRect]) {
        self.bookmarks_bar = bookmarks_bar;
        self.parent_folder = parent_folder;

        m_action = action.make_weak_ptr();
        m_hovered = NO;

        Ladybird::add_control_properties(self, action);
        [self setToolTip:Ladybird::string_to_ns_string(action.tooltip())];

        self.icon_view = Ladybird::create_application_icon(action);
        [self addSubview:self.icon_view];

        self.title_label = [NSTextField labelWithString:Ladybird::string_to_ns_string(action.text())];
        [self.title_label setFont:[NSFont menuFontOfSize:0]];
        [[self.title_label cell] setLineBreakMode:NSLineBreakByTruncatingTail];
        [self addSubview:self.title_label];
    }

    return self;
}

- (instancetype)initForSubfolder:(WebView::Menu&)menu
                    bookmarksBar:(BookmarksBar*)bookmarks_bar
                    parentFolder:(BookmarkFolderPopover*)parent_folder
{
    if (self = [super initWithFrame:NSZeroRect]) {
        self.bookmarks_bar = bookmarks_bar;
        self.parent_folder = parent_folder;

        m_menu = menu.make_weak_ptr();
        m_hovered = NO;

        Ladybird::add_control_properties(self, menu);

        self.icon_view = [[NSImageView alloc] initWithFrame:NSZeroRect];
        [self.icon_view setImage:[NSImage imageWithSystemSymbolName:@"folder" accessibilityDescription:@""]];
        [self addSubview:self.icon_view];

        self.title_label = [NSTextField labelWithString:Ladybird::string_to_ns_string(menu.title())];
        [self.title_label setFont:[NSFont menuFontOfSize:0]];
        [[self.title_label cell] setLineBreakMode:NSLineBreakByTruncatingTail];
        [self addSubview:self.title_label];

        self.chevron_view = [[NSImageView alloc] initWithFrame:NSZeroRect];
        [self.chevron_view setImage:[NSImage imageWithSystemSymbolName:@"chevron.right" accessibilityDescription:@""]];
        [self.chevron_view setAutoresizingMask:NSViewMinXMargin];
        [self addSubview:self.chevron_view];
    }

    return self;
}

- (void)setHovered:(BOOL)hovered
{
    if (m_hovered == hovered)
        return;

    m_hovered = hovered;

    auto* text_color = m_hovered ? [NSColor alternateSelectedControlTextColor] : [NSColor controlTextColor];
    [self.title_label setTextColor:text_color];

    [self setNeedsDisplay:YES];
}

#pragma mark - NSView

- (void)mouseEntered:(NSEvent*)event
{
    [self setHovered:YES];

    if (auto submenu = m_menu.strong_ref())
        [self.parent_folder openChildFolder:*submenu relativeToView:self];
    else
        [self.parent_folder closeChildFolder];
}

- (void)mouseExited:(NSEvent*)event
{
    [self setHovered:NO];
}

- (void)mouseDown:(NSEvent*)event
{
    if ([event modifierFlags] & NSEventModifierFlagControl) {
        [self.bookmarks_bar showContextMenu:self event:event];
        return;
    }

    if (auto submenu = m_menu.strong_ref(); submenu && submenu->size() > 0) {
        [self.parent_folder openChildFolder:*submenu relativeToView:self];
    } else if (auto action = m_action.strong_ref()) {
        [self.bookmarks_bar closeBookmarkFolders];
        action->activate();
    }
}

- (void)rightMouseDown:(NSEvent*)event
{
    [self.bookmarks_bar showContextMenu:self event:event];
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];

    auto mouse_location = [self convertPoint:[[self window] mouseLocationOutsideOfEventStream] fromView:nil];
    [self setHovered:CGRectContainsPoint([self bounds], mouse_location)];

    if (self.tracking_area)
        [self removeTrackingArea:self.tracking_area];

    self.tracking_area = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                      options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                        owner:self
                                                     userInfo:nil];
    [self addTrackingArea:self.tracking_area];
}

- (void)drawRect:(NSRect)dirtyRect
{
    [super drawRect:dirtyRect];

    if (!m_hovered)
        return;

    auto* selection_path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 4, 1) xRadius:8 yRadius:8];
    [[NSColor selectedContentBackgroundColor] setFill];
    [selection_path fill];
}

- (void)layout
{
    [super layout];

    auto [frame_width, frame_height] = [self bounds].size;

    auto icon_size = AK::min(BOOKMARK_FOLDER_ICON_SIZE, frame_height);
    auto icon_y = AK::floor((frame_height - icon_size) / 2.0);

    auto label_height = AK::min([self.title_label intrinsicContentSize].height, frame_height);
    auto label_y = AK::floor((frame_height - label_height) / 2.0);

    auto icon_frame = NSMakeRect(BOOKMARK_FOLDER_INSET, icon_y, icon_size, icon_size);
    [self.icon_view setFrame:icon_frame];

    auto trailing_width = self.chevron_view
        ? BOOKMARK_FOLDER_HORIZONTAL_PADDING + BOOKMARK_FOLDER_CHEVRON_WIDTH + BOOKMARK_FOLDER_INSET
        : 0;

    auto title_frame = NSMakeRect(
        BOOKMARK_FOLDER_INSET + icon_size + BOOKMARK_FOLDER_HORIZONTAL_PADDING,
        label_y,
        frame_width - (BOOKMARK_FOLDER_HORIZONTAL_PADDING * 2) - icon_size - trailing_width,
        label_height);
    [self.title_label setFrame:title_frame];

    if (self.chevron_view) {
        auto chevron_frame = NSMakeRect(
            frame_width - BOOKMARK_FOLDER_INSET - BOOKMARK_FOLDER_CHEVRON_WIDTH,
            icon_y,
            BOOKMARK_FOLDER_CHEVRON_WIDTH,
            icon_size);
        [self.chevron_view setFrame:chevron_frame];
    }
}

@end

@interface BookmarkFolderPopover () <NSPopoverDelegate>

@property (nonatomic, strong) BookmarkFolderPopover* child_folder;
@property (nonatomic, weak) BookmarkFolderPopover* parent_folder;
@property (nonatomic, weak) BookmarksBar* bookmarks_bar;

@end

@implementation BookmarkFolderPopover
{
    WeakPtr<WebView::Menu> m_menu;
}

- (instancetype)init:(WebView::Menu&)menu
        bookmarksBar:(BookmarksBar*)bookmarks_bar
        parentFolder:(BookmarkFolderPopover*)parent_folder
{
    if (self = [super init]) {
        self.bookmarks_bar = bookmarks_bar;
        self.parent_folder = parent_folder;
        self.delegate = self;

        m_menu = menu.make_weak_ptr();

        [self setAnimates:NO];
        [self setBehavior:NSPopoverBehaviorTransient];
        [self setValue:[NSNumber numberWithBool:YES] forKeyPath:@"shouldHideAnchor"];

        auto* content_view = [[NSView alloc] initWithFrame:NSZeroRect];
        auto* scroll_view = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        [scroll_view setHasVerticalScroller:YES];
        [scroll_view setDrawsBackground:NO];
        [scroll_view setBorderType:NSNoBorder];

        auto width = BOOKMARK_FOLDER_WIDTH;
        auto height = (BOOKMARK_FOLDER_VERTICAL_PADDING * 2) + (BOOKMARK_FOLDER_ROW_HEIGHT * menu.size());

        auto* items_view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
        auto y = height - BOOKMARK_FOLDER_VERTICAL_PADDING;

        for (auto const& item : menu.items()) {
            auto* item_view = item.visit(
                [&](NonnullRefPtr<WebView::Action> const& action) -> NSView* {
                    return [[BookmarkFolderItemView alloc] initForBookmark:*action
                                                              bookmarksBar:self.bookmarks_bar
                                                              parentFolder:self];
                },
                [&](NonnullRefPtr<WebView::Menu> const& submenu) -> NSView* {
                    return [[BookmarkFolderItemView alloc] initForSubfolder:*submenu
                                                               bookmarksBar:self.bookmarks_bar
                                                               parentFolder:self];
                },
                [&](WebView::Separator) -> NSView* {
                    VERIFY_NOT_REACHED();
                });

            y -= BOOKMARK_FOLDER_ROW_HEIGHT;

            [item_view setFrame:NSMakeRect(0, y, width, BOOKMARK_FOLDER_ROW_HEIGHT)];
            [items_view addSubview:item_view];
        }

        auto visible_height = AK::min(height, BOOKMARK_FOLDER_MAX_HEIGHT);
        [content_view setFrame:NSMakeRect(0, 0, width, visible_height)];
        [scroll_view setFrame:NSMakeRect(0, 0, width, visible_height)];
        [scroll_view setHasVerticalScroller:(height > visible_height)];
        [scroll_view setDocumentView:items_view];

        [content_view addSubview:scroll_view];

        auto* controller = [[NSViewController alloc] init];
        [controller setView:content_view];

        [self setContentViewController:controller];
        [self setContentSize:NSMakeSize(width, visible_height)];
    }

    return self;
}

- (void)showRelativeToView:(NSView*)view preferredEdge:(NSRectEdge)preferred_edge
{
    auto rect = [view bounds];

    if (preferred_edge == NSRectEdgeMaxX) {
        rect = NSMakeRect(
            NSWidth(rect) - BOOKMARK_FOLDER_HORIZONTAL_OVERLAP - BOOKMARK_FOLDER_SUBMENU_LEFT_SHIFT,
            0,
            BOOKMARK_FOLDER_HORIZONTAL_OVERLAP,
            NSHeight(rect));
    }

    [self showRelativeToRect:rect ofView:view preferredEdge:preferred_edge];

    if (preferred_edge == NSRectEdgeMaxY) {
        if (auto* window = [self.contentViewController.view window]) {
            auto origin = [window frame].origin;
            origin.y += BOOKMARK_FOLDER_ROOT_VERTICAL_SHIFT;
            [window setFrameOrigin:origin];
        }
    }
}

- (void)openChildFolder:(WebView::Menu&)menu relativeToView:(NSView*)view
{
    if (menu.size() == 0) {
        [self closeChildFolder];
        return;
    }

    [self.child_folder close];

    self.child_folder = [[BookmarkFolderPopover alloc] init:menu bookmarksBar:self.bookmarks_bar parentFolder:self];
    [self.child_folder showRelativeToView:view preferredEdge:NSRectEdgeMaxX];
}

- (void)closeChildFolder
{
    [self.child_folder close];
    self.child_folder = nil;
}

- (void)close
{
    [self.child_folder close];
    self.child_folder = nil;

    [super close];
}

#pragma mark - NSPopoverDelegate

- (void)popoverDidClose:(NSNotification*)notification
{
    [self.child_folder close];
    self.child_folder = nil;

    if (self.parent_folder)
        self.parent_folder.child_folder = nil;
    else
        [self.bookmarks_bar bookmarkFolderDidClose:self];
}

@end
