/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

@class LadybirdWebView;

@interface TaskManager : NSWindow

- (instancetype)init;

@property (nonatomic, strong) LadybirdWebView* web_view;

@end
