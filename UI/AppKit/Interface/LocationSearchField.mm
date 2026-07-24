/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/LocationSearchField.h>
#import <Interface/Menu.h>
#import <Utilities/Conversions.h>

#include <LibWebView/Application.h>
#include <LibWebView/Omnibox.h>
#include <LibWebView/URL.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const BADGE_TRAILING_MARGIN = 6;
static constexpr CGFloat const BADGE_GAP = 4;
static constexpr CGFloat const BADGE_BUTTON_SIZE = 22;
static constexpr CGFloat const ZOOM_PILL_HEIGHT = 18;
static constexpr CGFloat const ZOOM_PILL_HORIZONTAL_PADDING = 12;

static NSImage* location_field_globe_icon()
{
    static NSImage* image;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        image = [NSImage imageWithSystemSymbolName:@"globe" accessibilityDescription:@"Page icon"];
        [image setSize:NSMakeSize(16, 16)];
    });
    return image;
}

@interface LocationSearchField () <NSTextViewDelegate, NSMenuItemValidation>

- (CGFloat)trailingBadgeInset;
- (void)badgeLayoutDidChange;
- (NSString*)copyLinkText;
- (NSMenu*)createLocationContextMenu;
- (NSMenuItem*)createPasteAndGoMenuItem;
- (void)copyLink:(NSMenuItem*)sender;
- (void)pasteAndGo:(NSMenuItem*)sender;

@end

@interface LocationFieldBadgeButton : NSButton
@end

@implementation LocationFieldBadgeButton

- (void)setBordered:(BOOL)bordered
{
    // ActionObserver toggles bordered alongside visibility for toolbar buttons; in-field badges are always borderless.
    [super setBordered:NO];
}

- (void)setHidden:(BOOL)hidden
{
    [super setHidden:hidden];
    auto* location_search_field = (LocationSearchField*)[self superview];
    if ([location_search_field isKindOfClass:[LocationSearchField class]])
        [location_search_field badgeLayoutDidChange];
}

- (void)setTitle:(NSString*)title
{
    [super setTitle:title];
    auto* location_search_field = (LocationSearchField*)[self superview];
    if ([location_search_field isKindOfClass:[LocationSearchField class]])
        [location_search_field badgeLayoutDidChange];
}

@end

@interface LocationSearchFieldCell : NSSearchFieldCell

- (NSRect)rectByApplyingTrailingBadgeInset:(NSRect)rect;

@end

@implementation LocationSearchFieldCell

- (NSRect)cancelButtonRectForBounds:(NSRect)rect
{
    return NSZeroRect;
}

- (NSRect)searchTextRectForBounds:(NSRect)rect
{
    return [self rectByApplyingTrailingBadgeInset:[super searchTextRectForBounds:rect]];
}

- (NSRect)titleRectForBounds:(NSRect)rect
{
    return [self rectByApplyingTrailingBadgeInset:[super titleRectForBounds:rect]];
}

- (NSRect)rectByApplyingTrailingBadgeInset:(NSRect)rect
{
    if ([[self controlView] isKindOfClass:[LocationSearchField class]]) {
        auto* location_search_field = (LocationSearchField*)[self controlView];
        auto inset = [location_search_field trailingBadgeInset];

        if (rect.size.width > inset)
            rect.size.width -= inset;
        else
            rect.size.width = 0;
    }

    return rect;
}

@end

@implementation LocationSearchField
{
    BOOL m_loading;
    BOOL m_shows_page_icon;
    NSImage* m_favicon;
    NSProgressIndicator* m_loading_indicator;
    NSButton* m_bookmark_button;
    NSButton* m_zoom_button;
}

+ (Class)cellClass
{
    return [LocationSearchFieldCell class];
}

- (instancetype)init
{
    if (self = [super init]) {
        auto* cell = (NSSearchFieldCell*)[self cell];
        [cell setCancelButtonCell:nil];
        [cell setScrollable:YES];
        [cell setUsesSingleLineMode:YES];
        [cell setLineBreakMode:NSLineBreakByClipping];

        m_loading_indicator = [[NSProgressIndicator alloc] init];
        [m_loading_indicator setStyle:NSProgressIndicatorStyleSpinning];
        [m_loading_indicator setControlSize:NSControlSizeSmall];
        [m_loading_indicator setDisplayedWhenStopped:NO];
        [m_loading_indicator setHidden:YES];
        [self addSubview:m_loading_indicator];

        [self updateLeadingIcon];
    }

    return self;
}

- (BOOL)becomeFirstResponder
{
    BOOL result = [super becomeFirstResponder];
    if (result) {
        if (self.willBeginEditing)
            self.willBeginEditing();
        [self performSelector:@selector(selectText:) withObject:self afterDelay:0];
    }
    return result;
}

- (void)mouseDown:(NSEvent*)event
{
    [super mouseDown:event];

    if (self.willBeginEditing)
        self.willBeginEditing();
}

- (NSMenu*)textView:(NSTextView*)text_view
               menu:(NSMenu*)menu
           forEvent:(NSEvent*)event
            atIndex:(NSUInteger)character_index
{
    if (text_view != [self currentEditor])
        return menu;

    auto existing_item_index = [menu indexOfItemWithTarget:self andAction:@selector(pasteAndGo:)];
    if (existing_item_index >= 0)
        [menu removeItemAtIndex:existing_item_index];

    auto* paste_and_go_item = [self createPasteAndGoMenuItem];

    NSInteger paste_item_index = -1;
    for (NSInteger index = 0; index < [menu numberOfItems]; ++index) {
        if ([[menu itemAtIndex:index] action] == @selector(paste:)) {
            paste_item_index = index;
            break;
        }
    }

    if (paste_item_index >= 0)
        [menu insertItem:paste_and_go_item atIndex:paste_item_index + 1];
    else
        [menu addItem:paste_and_go_item];

    return menu;
}

- (NSMenu*)createLocationContextMenu
{
    auto* menu = [[NSMenu alloc] init];

    auto* copy_link_item = [[NSMenuItem alloc]
        initWithTitle:@"Copy Link"
               action:@selector(copyLink:)
        keyEquivalent:@""];
    [copy_link_item setTarget:self];
    [copy_link_item setRepresentedObject:[[self copyLinkText] copy]];
    [copy_link_item setEnabled:[self validateMenuItem:copy_link_item]];
    [menu addItem:copy_link_item];

    [menu addItem:[self createPasteAndGoMenuItem]];

    return menu;
}

- (BOOL)handleContextMenuEvent:(NSEvent*)event
{
    auto* editor = [self currentEditor];
    if (editor != nil && [[self window] firstResponder] == editor)
        return NO;

    [NSMenu popUpContextMenu:[self createLocationContextMenu] withEvent:event forView:self];
    return YES;
}

- (NSMenuItem*)createPasteAndGoMenuItem
{
    auto* pasteboard_text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    auto clipboard_text = pasteboard_text == nil
        ? String {}
        : Ladybird::ns_string_to_string(pasteboard_text);
    auto has_search_engine_enabled = WebView::Application::settings().search_engine().has_value();
    auto clipboard_holds_url = !clipboard_text.is_empty() && WebView::location_looks_like_url(clipboard_text);
    auto action_will_search = !clipboard_holds_url && has_search_engine_enabled;

    auto* paste_and_go_item = [[NSMenuItem alloc]
        initWithTitle:Ladybird::string_to_ns_string(WebView::Omnibox::text_for_paste_and_go_action(clipboard_text, has_search_engine_enabled))
               action:@selector(pasteAndGo:)
        keyEquivalent:@""];
    [paste_and_go_item setTarget:self];
    [paste_and_go_item setRepresentedObject:[pasteboard_text copy]];
    [paste_and_go_item setEnabled:[self validateMenuItem:paste_and_go_item]];
    [paste_and_go_item setImage:action_will_search
            ? [NSImage imageWithSystemSymbolName:@"magnifyingglass" accessibilityDescription:@"Search"]
            : [NSImage imageNamed:NSImageNameGoRightTemplate]];

    return paste_and_go_item;
}

- (BOOL)validateMenuItem:(NSMenuItem*)menu_item
{
    if ([menu_item action] == @selector(copyLink:)) {
        id copy_link_text = [menu_item representedObject];
        return [copy_link_text isKindOfClass:[NSString class]]
            && [(NSString*)copy_link_text length] > 0;
    }

    if ([menu_item action] == @selector(pasteAndGo:)) {
        id clipboard_text = [menu_item representedObject];
        return [self isEditable]
            && [clipboard_text isKindOfClass:[NSString class]]
            && [(NSString*)clipboard_text length] > 0;
    }

    return YES;
}

- (NSString*)copyLinkText
{
    if (self.copyLinkTextProvider)
        return self.copyLinkTextProvider();

    return [self stringValue];
}

- (void)copyLink:(NSMenuItem*)sender
{
    id copy_link_text = [sender representedObject];
    if (![copy_link_text isKindOfClass:[NSString class]]
        || [(NSString*)copy_link_text length] == 0)
        return;

    auto* paste_board = [NSPasteboard generalPasteboard];
    [paste_board clearContents];
    [paste_board setString:(NSString*)copy_link_text forType:NSPasteboardTypeString];
}

- (void)pasteAndGo:(NSMenuItem*)sender
{
    id clipboard_text = [sender representedObject];
    if (![self isEditable]
        || ![clipboard_text isKindOfClass:[NSString class]]
        || [(NSString*)clipboard_text length] == 0)
        return;

    if (self.pasteAndGoHandler)
        self.pasteAndGoHandler((NSString*)clipboard_text);
}

- (void)layout
{
    [super layout];

    auto bounds = [self bounds];
    auto* cell = (NSSearchFieldCell*)[self cell];
    auto search_button_rect = [cell searchButtonRectForBounds:bounds];
    auto indicator_size = NSMakeSize(16, 16);
    [m_loading_indicator setFrame:NSMakeRect(
                                      NSMidX(search_button_rect) - indicator_size.width / 2,
                                      NSMidY(search_button_rect) - indicator_size.height / 2,
                                      indicator_size.width,
                                      indicator_size.height)];

    auto badge_max_x = NSMaxX(bounds) - BADGE_TRAILING_MARGIN;
    if ([self isBadgeVisible:m_bookmark_button]) {
        auto y = NSMidY(bounds) - BADGE_BUTTON_SIZE / 2;
        [m_bookmark_button setFrame:NSMakeRect(badge_max_x - BADGE_BUTTON_SIZE, y, BADGE_BUTTON_SIZE, BADGE_BUTTON_SIZE)];
        badge_max_x = NSMinX([m_bookmark_button frame]) - BADGE_GAP;
    }

    if ([self isBadgeVisible:m_zoom_button]) {
        auto width = [self zoomButtonWidth];
        auto y = NSMidY(bounds) - ZOOM_PILL_HEIGHT / 2;
        [m_zoom_button setFrame:NSMakeRect(badge_max_x - width, y, width, ZOOM_PILL_HEIGHT)];
    }
}

- (void)viewDidChangeEffectiveAppearance
{
    [super viewDidChangeEffectiveAppearance];
    [self updateLayerColors];
}

- (void)setLoading:(BOOL)loading
{
    if (m_loading == loading)
        return;

    m_loading = loading;
    if (m_loading) {
        [m_loading_indicator setHidden:NO];
        [m_loading_indicator startAnimation:self];
    } else {
        [m_loading_indicator stopAnimation:self];
        [m_loading_indicator setHidden:YES];
    }

    [self updateLeadingIcon];
}

- (void)setFavicon:(NSImage*)favicon
{
    m_favicon = [favicon copy];
    [m_favicon setSize:NSMakeSize(16, 16)];
    [m_favicon setTemplate:NO];
    [self updateLeadingIcon];
}

- (void)setShowsPageIcon:(BOOL)showsPageIcon
{
    m_shows_page_icon = showsPageIcon;
    [self updateLeadingIcon];
}

- (void)setBookmarkAction:(WebView::Action&)action
{
    if (m_bookmark_button != nil)
        [m_bookmark_button removeFromSuperview];

    m_bookmark_button = Ladybird::create_application_button(action, [LocationFieldBadgeButton class]);
    [m_bookmark_button setBordered:NO];
    [m_bookmark_button setImagePosition:NSImageOnly];
    [m_bookmark_button setRefusesFirstResponder:YES];
    [self addSubview:m_bookmark_button];
    [self badgeLayoutDidChange];
}

- (void)setZoomAction:(WebView::Action&)action
{
    if (m_zoom_button != nil)
        [m_zoom_button removeFromSuperview];

    m_zoom_button = Ladybird::create_application_button(action, [LocationFieldBadgeButton class]);
    [m_zoom_button setBordered:NO];
    [m_zoom_button setImagePosition:NSNoImage];
    [m_zoom_button setFont:[NSFont monospacedDigitSystemFontOfSize:[NSFont smallSystemFontSize] weight:NSFontWeightRegular]];
    [m_zoom_button setRefusesFirstResponder:YES];
    [m_zoom_button setWantsLayer:YES];
    [[m_zoom_button layer] setCornerRadius:ZOOM_PILL_HEIGHT / 2];
    [[m_zoom_button layer] setMasksToBounds:YES];
    [self updateLayerColors];
    [self addSubview:m_zoom_button];
    [self badgeLayoutDidChange];
}

- (void)updateLeadingIcon
{
    auto* cell = (NSSearchFieldCell*)[self cell];
    auto* search_button = [cell searchButtonCell];

    if (m_loading) {
        [search_button setImage:nil];
    } else if (m_shows_page_icon && m_favicon != nil) {
        [search_button setImage:m_favicon];
    } else if (!m_shows_page_icon) {
        [search_button setImage:[NSImage imageWithSystemSymbolName:@"magnifyingglass"
                                          accessibilityDescription:@"Search"]];
    } else {
        [search_button setImage:location_field_globe_icon()];
    }
}

- (void)updateLayerColors
{
    [[m_zoom_button layer] setBackgroundColor:[NSColor quaternaryLabelColor].CGColor];
}

- (BOOL)isBadgeVisible:(NSButton*)button
{
    return button != nil && ![button isHidden];
}

- (CGFloat)zoomButtonWidth
{
    NSDictionary* attributes = @{ NSFontAttributeName : [m_zoom_button font] };
    auto* title = [m_zoom_button title];
    auto width = [(title.length == 0 ? @"100%" : title) sizeWithAttributes:attributes].width;
    auto reserved_text_width = [@"100%" sizeWithAttributes:attributes].width;
    if (width < reserved_text_width)
        width = reserved_text_width;
    return ceil(width + ZOOM_PILL_HORIZONTAL_PADDING);
}

- (CGFloat)trailingBadgeInset
{
    CGFloat inset = 0;
    BOOL has_badge = NO;

    if ([self isBadgeVisible:m_bookmark_button]) {
        inset += BADGE_BUTTON_SIZE;
        has_badge = YES;
    }

    if (m_zoom_button != nil) {
        if (has_badge)
            inset += BADGE_GAP;
        inset += [self zoomButtonWidth];
        has_badge = YES;
    }

    if (has_badge)
        inset += BADGE_TRAILING_MARGIN;

    return inset;
}

- (void)badgeLayoutDidChange
{
    [self setNeedsLayout:YES];
    [self setNeedsDisplay:YES];
    [self layoutSubtreeIfNeeded];
}

// NSSearchField does not provide an intrinsic width, which causes an ambiguous layout warning when the toolbar auto-
// measures this view. This provides an initial fallback, which is overridden with an explicit width in windowDidResize.
- (NSSize)intrinsicContentSize
{
    auto size = [super intrinsicContentSize];
    if (size.width < 0)
        size.width = 400;
    return size;
}

@end
