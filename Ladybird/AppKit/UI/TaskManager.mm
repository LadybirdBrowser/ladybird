/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibCore/Timer.h>
#include <LibWebView/Application.h>

#import <UI/LadybirdWebView.h>
#import <UI/TaskManager.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const WINDOW_WIDTH = 600;
static constexpr CGFloat const WINDOW_HEIGHT = 400;

@interface TaskManager ()
{
    RefPtr<Core::Timer> m_update_timer;
}

@end

@implementation TaskManager

- (instancetype)init
{
    auto tab_rect = [[NSApp keyWindow] frame];
    auto position_x = tab_rect.origin.x + (tab_rect.size.width - WINDOW_WIDTH) / 2;
    auto position_y = tab_rect.origin.y + (tab_rect.size.height - WINDOW_HEIGHT) / 2;

    auto window_rect = NSMakeRect(position_x, position_y, WINDOW_WIDTH, WINDOW_HEIGHT);
    auto style_mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

    self = [super initWithContentRect:window_rect
                            styleMask:style_mask
                              backing:NSBackingStoreBuffered
                                defer:NO];

    if (self) {
        self.web_view = [[LadybirdWebView alloc] init:nil];
        [self.web_view setPostsBoundsChangedNotifications:YES];

        auto* scroll_view = [[NSScrollView alloc] init];
        [scroll_view setHasVerticalScroller:YES];
        [scroll_view setHasHorizontalScroller:YES];
        [scroll_view setLineScroll:24];

        [scroll_view setContentView:self.web_view];
        [scroll_view setDocumentView:[[NSView alloc] init]];

        __weak TaskManager* weak_self = self;

        m_update_timer = Core::Timer::create_repeating(1000, [weak_self] {
            TaskManager* strong_self = weak_self;
            if (strong_self == nil) {
                return;
            }

            [strong_self updateStatistics];
        });

        [self setContentView:scroll_view];
        [self setTitle:@"Task Manager"];
        [self setIsVisible:YES];

        [self updateStatistics];
        m_update_timer->start();
    }

    return self;
}

- (void)updateStatistics
{
    WebView::Application::the().update_process_statistics();
    [self.web_view loadHTML:WebView::Application::the().generate_process_statistics_html()];
}

@end
