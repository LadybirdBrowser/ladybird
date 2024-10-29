/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

@class LadybirdWebView;

@interface LadybirdWebViewWindow : NSWindow

- (instancetype)initWithWebView:(LadybirdWebView*)web_view
                     windowRect:(NSRect)window_rect;

@property (nonatomic, strong) LadybirdWebView* web_view;

@end
