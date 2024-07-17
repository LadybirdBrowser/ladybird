/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

@protocol TaskManagerDelegate <NSObject>

- (void)onTaskManagerClosed;

@end

@interface TaskManagerController : NSWindowController

- (instancetype)init:(id<TaskManagerDelegate>)delegate;

@end
