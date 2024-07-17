/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

@class Tab;

@interface InspectorController : NSWindowController

- (instancetype)init:(Tab*)tab;

@end
