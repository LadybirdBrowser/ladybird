/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <UI/LadybirdWebView.h>
#import <UI/TaskManager.h>
#import <UI/TaskManagerController.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

@interface TaskManagerController () <NSWindowDelegate>

@property (nonatomic, weak) id<TaskManagerDelegate> delegate;

@end

@implementation TaskManagerController

- (instancetype)initWithDelegate:(id<TaskManagerDelegate>)delegate
{
    if (self = [super init]) {
        self.delegate = delegate;
    }

    return self;
}

#pragma mark - Private methods

- (TaskManager*)taskManager
{
    return (TaskManager*)[self window];
}

#pragma mark - NSWindowController

- (IBAction)showWindow:(id)sender
{
    self.window = [[TaskManager alloc] init];
    [self.window setDelegate:self];
    [self.window makeKeyAndOrderFront:sender];
}

#pragma mark - NSWindowDelegate

- (void)windowWillClose:(NSNotification*)notification
{
    [self.delegate onTaskManagerClosed];
}

- (void)windowDidResize:(NSNotification*)notification
{
    [[[self taskManager] web_view] handleResize];
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
    [[[self taskManager] web_view] handleDevicePixelRatioChange];
}

@end
