/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibCore/Resource.h>
#include <LibURL/URL.h>
#include <LibWebView/ViewImplementation.h>

#import <Application/ApplicationDelegate.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/SearchPanel.h>
#import <Interface/Tab.h>
#import <Interface/TabController.h>
#import <Utilities/Conversions.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const WINDOW_WIDTH = 1000;
static constexpr CGFloat const WINDOW_HEIGHT = 800;

@interface Tab () <LadybirdWebViewObserver>

@property (nonatomic, strong) NSString* title;
@property (nonatomic, strong) NSImage* favicon;

@property (nonatomic, strong) SearchPanel* search_panel;

@end

@implementation Tab

@dynamic title;

+ (NSImage*)defaultFavicon
{
    static NSImage* default_favicon;
    static dispatch_once_t token;

    dispatch_once(&token, ^{
        auto default_favicon_path = MUST(Core::Resource::load_from_uri("resource://icons/48x48/app-browser.png"sv));
        auto* ns_default_favicon_path = Ladybird::string_to_ns_string(default_favicon_path->filesystem_path());

        default_favicon = [[NSImage alloc] initWithContentsOfFile:ns_default_favicon_path];
    });

    return default_favicon;
}

- (instancetype)init
{
    auto* web_view = [[LadybirdWebView alloc] init:self];
    return [self initWithWebView:web_view];
}

- (instancetype)initAsChild:(Tab*)parent
                  pageIndex:(u64)page_index
{
    auto* web_view = [[LadybirdWebView alloc] initAsChild:self parent:[parent web_view] pageIndex:page_index];
    return [self initWithWebView:web_view];
}

- (instancetype)initWithWebView:(LadybirdWebView*)web_view
{
    auto screen_rect = [[NSScreen mainScreen] frame];
    auto position_x = (NSWidth(screen_rect) - WINDOW_WIDTH) / 2;
    auto position_y = (NSHeight(screen_rect) - WINDOW_HEIGHT) / 2;
    auto window_rect = NSMakeRect(position_x, position_y, WINDOW_WIDTH, WINDOW_HEIGHT);

    if (self = [super initWithWebView:web_view windowRect:window_rect]) {
        // Remember last window position
        self.frameAutosaveName = @"window";

        self.favicon = [Tab defaultFavicon];
        self.title = @"New Tab";
        [self updateTabTitleAndFavicon];

        [self setTitleVisibility:NSWindowTitleHidden];
        [self setIsVisible:YES];

        self.search_panel = [[SearchPanel alloc] init];
        [self.search_panel setHidden:YES];

        auto* stack_view = [NSStackView stackViewWithViews:@[
            self.search_panel,
            self.web_view,
        ]];

        [stack_view setOrientation:NSUserInterfaceLayoutOrientationVertical];
        [stack_view setSpacing:0];

        [self setContentView:stack_view];

        [[self.search_panel leadingAnchor] constraintEqualToAnchor:[self.contentView leadingAnchor]].active = YES;
    }

    return self;
}

#pragma mark - Public methods

- (void)find:(id)sender
{
    [self.search_panel find:sender];
}

- (void)findNextMatch:(id)sender
{
    [self.search_panel findNextMatch:sender];
}

- (void)findPreviousMatch:(id)sender
{
    [self.search_panel findPreviousMatch:sender];
}

- (void)useSelectionForFind:(id)sender
{
    [self.search_panel useSelectionForFind:sender];
}

#pragma mark - Private methods

- (TabController*)tabController
{
    return (TabController*)[self windowController];
}

- (void)updateTabTitleAndFavicon
{
    static constexpr CGFloat TITLE_FONT_SIZE = 12;
    static constexpr CGFloat FAVICON_SIZE = 16;

    NSFont* title_font = [NSFont systemFontOfSize:TITLE_FONT_SIZE];

    auto* favicon_attachment = [[NSTextAttachment alloc] init];
    favicon_attachment.image = self.favicon;

    // By default, the image attachment will "automatically adapt to the surrounding font and color
    // attributes in attributed strings". Therefore, we specify a clear color here to prevent the
    // favicon from having a weird tint.
    auto* favicon_attribute = (NSMutableAttributedString*)[NSMutableAttributedString attributedStringWithAttachment:favicon_attachment];
    [favicon_attribute addAttribute:NSForegroundColorAttributeName
                              value:[NSColor clearColor]
                              range:NSMakeRange(0, [favicon_attribute length])];

    // adjust the favicon image to middle center the title text
    CGFloat offset_y = (title_font.capHeight - FAVICON_SIZE) / 2.f;
    [favicon_attachment setBounds:CGRectMake(0, offset_y, FAVICON_SIZE, FAVICON_SIZE)];

    auto* title_attributes = @{
        NSForegroundColorAttributeName : [NSColor textColor],
        NSFontAttributeName : title_font
    };

    auto* title_attribute = [[NSAttributedString alloc] initWithString:self.title
                                                            attributes:title_attributes];

    auto* spacing_attribute = [[NSAttributedString alloc] initWithString:@"  "
                                                              attributes:title_attributes];

    auto* title_and_favicon = [[NSMutableAttributedString alloc] init];
    [title_and_favicon appendAttributedString:favicon_attribute];
    [title_and_favicon appendAttributedString:spacing_attribute];
    [title_and_favicon appendAttributedString:title_attribute];

    [[self tab] setAttributedTitle:title_and_favicon];
}

- (void)togglePageMuteState:(id)button
{
    auto& view = [[self web_view] view];
    view.toggle_page_mute_state();

    switch (view.audio_play_state()) {
    case Web::HTML::AudioPlayState::Paused:
        [[self tab] setAccessoryView:nil];
        break;

    case Web::HTML::AudioPlayState::Playing:
        [button setImage:[self iconForPageMuteState]];
        [button setToolTip:[self toolTipForPageMuteState]];
        break;
    }
}

- (NSImage*)iconForPageMuteState
{
    auto& view = [[self web_view] view];

    switch (view.page_mute_state()) {
    case Web::HTML::MuteState::Muted:
        return [NSImage imageNamed:NSImageNameTouchBarAudioOutputVolumeOffTemplate];
    case Web::HTML::MuteState::Unmuted:
        return [NSImage imageNamed:NSImageNameTouchBarAudioOutputVolumeHighTemplate];
    }

    VERIFY_NOT_REACHED();
}

- (NSString*)toolTipForPageMuteState
{
    auto& view = [[self web_view] view];

    switch (view.page_mute_state()) {
    case Web::HTML::MuteState::Muted:
        return @"Unmute tab";
    case Web::HTML::MuteState::Unmuted:
        return @"Mute tab";
    }

    VERIFY_NOT_REACHED();
}

#pragma mark - LadybirdWebViewObserver

- (String const&)onCreateNewTab:(Optional<URL::URL> const&)url
                    activateTab:(Web::HTML::ActivateTab)activate_tab
{
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];

    auto* controller = [delegate createNewTab:url
                                      fromTab:self
                                  activateTab:activate_tab];

    auto* tab = (Tab*)[controller window];
    return [[tab web_view] handle];
}

- (String const&)onCreateChildTab:(Optional<URL::URL> const&)url
                      activateTab:(Web::HTML::ActivateTab)activate_tab
                        pageIndex:(u64)page_index
{
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];

    auto* controller = [delegate createChildTab:url
                                        fromTab:self
                                    activateTab:activate_tab
                                      pageIndex:page_index];

    auto* tab = (Tab*)[controller window];
    return [[tab web_view] handle];
}

- (void)onLoadStart:(URL::URL const&)url isRedirect:(BOOL)is_redirect
{
    self.title = Ladybird::string_to_ns_string(url.serialize());
    self.favicon = [Tab defaultFavicon];
    [self updateTabTitleAndFavicon];

    [[self tabController] onLoadStart:url isRedirect:is_redirect];
}

- (void)onLoadFinish:(URL::URL const&)url
{
}

- (void)onURLChange:(URL::URL const&)url
{
    [[self tabController] onURLChange:url];
}

- (void)onTitleChange:(Utf16String const&)title
{
    self.title = Ladybird::utf16_string_to_ns_string(title);
    [self updateTabTitleAndFavicon];
}

- (void)onFaviconChange:(Gfx::Bitmap const&)bitmap
{
    auto* favicon = Ladybird::gfx_bitmap_to_ns_image(bitmap);
    [favicon setResizingMode:NSImageResizingModeStretch];
    self.favicon = favicon;
    [self updateTabTitleAndFavicon];
}

- (void)onAudioPlayStateChange:(Web::HTML::AudioPlayState)play_state
{
    auto& view = [[self web_view] view];

    switch (play_state) {
    case Web::HTML::AudioPlayState::Paused:
        if (view.page_mute_state() == Web::HTML::MuteState::Unmuted) {
            [[self tab] setAccessoryView:nil];
        }
        break;

    case Web::HTML::AudioPlayState::Playing:
        auto* button = [NSButton buttonWithImage:[self iconForPageMuteState]
                                          target:self
                                          action:@selector(togglePageMuteState:)];
        [button setToolTip:[self toolTipForPageMuteState]];

        [[self tab] setAccessoryView:button];
        break;
    }
}

- (void)onFindInPageResult:(size_t)current_match_index
           totalMatchCount:(Optional<size_t> const&)total_match_count
{
    [self.search_panel onFindInPageResult:current_match_index
                          totalMatchCount:total_match_count];
}

@end
