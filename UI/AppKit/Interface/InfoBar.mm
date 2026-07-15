/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/InfoBar.h>
#import <Interface/Tab.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const INFO_BAR_HEIGHT = 40;

@interface InfoBar ()

@property (nonatomic, strong) NSTextField* text_label;
@property (nonatomic, strong) NSButton* action_button;
@property (nonatomic, strong) NSButton* dismiss_button;
@property (nonatomic, copy) InfoBarButtonClicked on_action;
@property (nonatomic, copy) InfoBarButtonClicked on_dismissed;

@end

@implementation InfoBar

- (instancetype)init
{
    if (self = [super init]) {
        self.text_label = [NSTextField labelWithString:@""];

        self.action_button = [NSButton buttonWithTitle:@""
                                                target:self
                                                action:@selector(performAction:)];
        [self.action_button setBezelStyle:NSBezelStyleAccessoryBarAction];

        self.dismiss_button = [NSButton buttonWithTitle:@""
                                                 target:self
                                                 action:@selector(dismiss:)];
        [self.dismiss_button setBezelStyle:NSBezelStyleAccessoryBarAction];

        [self addView:self.text_label inGravity:NSStackViewGravityLeading];
        [self addView:self.action_button inGravity:NSStackViewGravityTrailing];
        [self addView:self.dismiss_button inGravity:NSStackViewGravityTrailing];

        [self setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
        [self setEdgeInsets:NSEdgeInsets { 0, 8, 0, 8 }];

        [[self heightAnchor] constraintEqualToConstant:INFO_BAR_HEIGHT].active = YES;
    }

    return self;
}

- (void)showWithMessage:(NSString*)message
       actionButtonTitle:(NSString*)action_title
     actionButtonClicked:(InfoBarButtonClicked)on_action
      dismissButtonTitle:(NSString*)title
    dismissButtonClicked:(InfoBarButtonClicked)on_dismissed
               activeTab:(Tab*)tab
{
    [self.text_label setStringValue:message];

    self.action_button.title = action_title;
    self.on_action = on_action;
    self.dismiss_button.title = title;
    self.on_dismissed = on_dismissed;

    if (tab) {
        [self attachToTab:tab];
    }

    [self setHidden:NO];
}

- (void)performAction:(id)sender
{
    if (self.on_action)
        self.on_action();
}

- (void)dismiss:(id)sender
{
    if (self.on_dismissed) {
        self.on_dismissed();
    }

    [self hide];
}

- (void)hide
{
    [self removeFromSuperview];
    [self setHidden:YES];
}

- (void)tabBecameActive:(Tab*)tab
{
    if (![self isHidden]) {
        [self attachToTab:tab];
    }
}

- (void)attachToTab:(Tab*)tab
{
    [self removeFromSuperview];

    [tab.contentView addView:self inGravity:NSStackViewGravityTrailing];
    [[self leadingAnchor] constraintEqualToAnchor:[tab.contentView leadingAnchor]].active = YES;
}

@end
