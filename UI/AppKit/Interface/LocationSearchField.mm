/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/LocationSearchField.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

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

@interface LocationSearchFieldCell : NSSearchFieldCell
@end

@implementation LocationSearchFieldCell

- (NSRect)cancelButtonRectForBounds:(NSRect)rect
{
    return NSZeroRect;
}

@end

@implementation LocationSearchField
{
    BOOL m_loading;
    BOOL m_shows_page_icon;
    NSImage* m_favicon;
    NSProgressIndicator* m_loading_indicator;
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

    auto* cell = (NSSearchFieldCell*)[self cell];
    auto search_button_rect = [cell searchButtonRectForBounds:[self bounds]];
    auto indicator_size = NSMakeSize(16, 16);
    [m_loading_indicator setFrame:NSMakeRect(
                                      NSMidX(search_button_rect) - indicator_size.width / 2,
                                      NSMidY(search_button_rect) - indicator_size.height / 2,
                                      indicator_size.width,
                                      indicator_size.height)];
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
