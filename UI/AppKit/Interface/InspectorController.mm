/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/Inspector.h>
#import <Interface/InspectorController.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/Tab.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

@interface InspectorController () <NSWindowDelegate>

@property (nonatomic, strong) Tab* tab;

@end

@implementation InspectorController

- (instancetype)init:(Tab*)tab
{
    if (self = [super init]) {
        self.tab = tab;
    }

    return self;
}

#pragma mark - Private methods

- (Inspector*)inspector
{
    return (Inspector*)[self window];
}

#pragma mark - NSWindowController

- (IBAction)showWindow:(id)sender
{
    self.window = [[Inspector alloc] init:self.tab];
    [self.window setDelegate:self];
    [self.window makeKeyAndOrderFront:sender];
}

#pragma mark - NSWindowDelegate

- (void)windowWillClose:(NSNotification*)notification
{
    [self.tab onInspectorClosed];
}

- (void)windowDidResize:(NSNotification*)notification
{
    [[[self inspector] web_view] handleResize];
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
    [[[self inspector] web_view] handleDevicePixelRatioChange];
}

@end
