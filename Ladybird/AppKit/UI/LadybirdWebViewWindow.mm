/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <UI/LadybirdWebView.h>
#import <UI/LadybirdWebViewWindow.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

@interface LadybirdWebViewWindow ()
@end

@implementation LadybirdWebViewWindow

- (instancetype)initWithWebView:(LadybirdWebView*)web_view
                     windowRect:(NSRect)window_rect
{
    static constexpr auto style_mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

    self = [super initWithContentRect:window_rect
                            styleMask:style_mask
                              backing:NSBackingStoreBuffered
                                defer:NO];

    if (self) {
        self.web_view = web_view;
        if (self.web_view == nil)
            self.web_view = [[LadybirdWebView alloc] init:nil];

        [self.web_view setPostsBoundsChangedNotifications:YES];

        auto* scroll_view = [[NSScrollView alloc] init];
        [scroll_view setHasVerticalScroller:NO];
        [scroll_view setHasHorizontalScroller:NO];
        [scroll_view setLineScroll:24];

        [scroll_view setContentView:self.web_view];
        [scroll_view setDocumentView:[[NSView alloc] init]];

        [[NSNotificationCenter defaultCenter]
            addObserver:self
               selector:@selector(onContentScroll:)
                   name:NSViewBoundsDidChangeNotification
                 object:[scroll_view contentView]];
    }

    return self;
}

#pragma mark - Private methods

- (void)onContentScroll:(NSNotification*)notification
{
    [self.web_view handleScroll];
}

#pragma mark - NSWindow

- (void)setIsVisible:(BOOL)flag
{
    [self.web_view handleVisibility:flag];
    [super setIsVisible:flag];
}

- (void)setIsMiniaturized:(BOOL)flag
{
    [self.web_view handleVisibility:!flag];
    [super setIsMiniaturized:flag];
}

@end
