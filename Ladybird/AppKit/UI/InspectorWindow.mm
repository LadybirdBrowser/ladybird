/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Neil Viloria <neilcviloria@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/InspectorClient.h>

#import <UI/InspectorWindow.h>
#import <UI/LadybirdWebView.h>
#import <UI/Tab.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const WINDOW_WIDTH = 875;
static constexpr CGFloat const WINDOW_HEIGHT = 825;

@interface InspectorWindow ()
{
    OwnPtr<WebView::InspectorClient> m_inspector_client;
}

@property (nonatomic, strong) Tab* tab;

@end

@implementation InspectorWindow

@synthesize tab = _tab;

- (instancetype)init:(Tab*)tab
{
    auto tab_rect = [tab frame];
    auto position_x = tab_rect.origin.x + (tab_rect.size.width - WINDOW_WIDTH) / 2;
    auto position_y = tab_rect.origin.y + (tab_rect.size.height - WINDOW_HEIGHT) / 2;

    auto window_rect = NSMakeRect(position_x, position_y, WINDOW_WIDTH, WINDOW_HEIGHT);
    auto style_mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

    self = [super initWithContentRect:window_rect
                            styleMask:style_mask
                              backing:NSBackingStoreBuffered
                                defer:NO];

    if (self) {
        self.tab = tab;

        [self setContentView:tab.inspector];
        [self setTitle:@"Inspector"];
        [self setIsVisible:YES];
    }
    return self;
}

@end
