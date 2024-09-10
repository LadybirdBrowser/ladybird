/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <UI/InspectorController.h>
#import <UI/InspectorWindow.h>
#import <UI/LadybirdWebView.h>
#import <UI/Tab.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

@interface InspectorController () <NSWindowDelegate>

@property (nonatomic, strong) Tab* tab;
@property (nonatomic, strong) Inspector* inspector;

@end

@implementation InspectorController

- (instancetype)init:(Tab*)tab
           inspector:(Inspector*)inspector
{
    if (self = [super init]) {
        self.tab = tab;
        self.inspector = inspector;
    }

    return self;
}

#pragma mark - Private methods

- (InspectorWindow*)inspectorWindow
{
    return (InspectorWindow*)[self window];
}

#pragma mark - NSWindowController

- (IBAction)showWindow:(id)sender
{
    self.window = [[InspectorWindow alloc] init:self.tab inspector:self.inspector];
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
    if (![[self window] inLiveResize]) {
        [[[self tab] web_view] handleResize];
    }
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
    [[[self tab] web_view] handleDevicePixelRatioChange];
}

@end
