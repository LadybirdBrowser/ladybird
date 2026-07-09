/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/LocationSearchField.h>
#import <Interface/Menu.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const BADGE_TRAILING_MARGIN = 6;
static constexpr CGFloat const BADGE_BUTTON_SIZE = 22;

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

@interface LocationSearchField ()

- (CGFloat)trailingBadgeInset;
- (void)badgeLayoutDidChange;

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
    }
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

- (BOOL)isBadgeVisible:(NSButton*)button
{
    return button != nil && ![button isHidden];
}

- (CGFloat)trailingBadgeInset
{
    CGFloat inset = 0;
    BOOL has_badge = NO;

    if ([self isBadgeVisible:m_bookmark_button]) {
        inset += BADGE_BUTTON_SIZE;
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
