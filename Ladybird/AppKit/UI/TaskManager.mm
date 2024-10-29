/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
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

    if (self = [super initWithWebView:nil windowRect:window_rect]) {
        __weak TaskManager* weak_self = self;

        m_update_timer = Core::Timer::create_repeating(1000, [weak_self] {
            TaskManager* strong_self = weak_self;
            if (strong_self == nil) {
                return;
            }

            [strong_self updateStatistics];
        });

        [self setContentView:self.web_view.enclosingScrollView];
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
