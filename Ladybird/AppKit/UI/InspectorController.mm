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

- (InspectorWindow*)inspectorWindow
{
    return (InspectorWindow*)[self window];
}

#pragma mark - NSWindowController

- (IBAction)showWindow:(id)sender
{
    self.window = [[InspectorWindow alloc] init:self.tab];
    [self.window setDelegate:self];
    [self.window makeKeyAndOrderFront:sender];
}

- (void)close
{
    // Temporarily remove the window delegate to prevent `windowWillClose`
    // from being called. This avoids deallocating the inspector when
    // we just want to move it to the main window and close the
    // inspector's window
    auto delegate = self.window.delegate;
    [self.window setDelegate:nil];
    [self.window close];
    [self.window setDelegate:delegate];
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
