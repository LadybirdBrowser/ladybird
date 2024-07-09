/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Loader/UserAgent.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>

#import <Application/ApplicationDelegate.h>
#import <UI/LadybirdWebView.h>
#import <UI/Tab.h>
#import <UI/TabController.h>
#import <Utilities/Conversions.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static NSString* const TOOLBAR_IDENTIFIER = @"Toolbar";
static NSString* const TOOLBAR_NAVIGATE_BACK_IDENTIFIER = @"ToolbarNavigateBackIdentifier";
static NSString* const TOOLBAR_NAVIGATE_FORWARD_IDENTIFIER = @"ToolbarNavigateForwardIdentifier";
static NSString* const TOOLBAR_RELOAD_IDENTIFIER = @"ToolbarReloadIdentifier";
static NSString* const TOOLBAR_LOCATION_IDENTIFIER = @"ToolbarLocationIdentifier";
static NSString* const TOOLBAR_ZOOM_IDENTIFIER = @"ToolbarZoomIdentifier";
static NSString* const TOOLBAR_NEW_TAB_IDENTIFIER = @"ToolbarNewTabIdentifier";
static NSString* const TOOLBAR_TAB_OVERVIEW_IDENTIFIER = @"ToolbarTabOverviewIdentifer";

@interface LocationSearchField : NSSearchField

- (BOOL)becomeFirstResponder;

@end

@implementation LocationSearchField

- (BOOL)becomeFirstResponder
{
    BOOL result = [super becomeFirstResponder];
    if (result)
        [self performSelector:@selector(selectText:) withObject:self afterDelay:0];
    return result;
}

@end

@interface TabController () <NSToolbarDelegate, NSSearchFieldDelegate>
{
    ByteString m_title;

    TabSettings m_settings;

    bool m_can_navigate_back;
    bool m_can_navigate_forward;
}

@property (nonatomic, strong) NSToolbar* toolbar;
@property (nonatomic, strong) NSArray* toolbar_identifiers;

@property (nonatomic, strong) NSToolbarItem* navigate_back_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* navigate_forward_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* reload_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* location_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* zoom_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* new_tab_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* tab_overview_toolbar_item;

@property (nonatomic, assign) NSLayoutConstraint* location_toolbar_item_width;

@end

@implementation TabController

@synthesize toolbar_identifiers = _toolbar_identifiers;
@synthesize navigate_back_toolbar_item = _navigate_back_toolbar_item;
@synthesize navigate_forward_toolbar_item = _navigate_forward_toolbar_item;
@synthesize reload_toolbar_item = _reload_toolbar_item;
@synthesize location_toolbar_item = _location_toolbar_item;
@synthesize zoom_toolbar_item = _zoom_toolbar_item;
@synthesize new_tab_toolbar_item = _new_tab_toolbar_item;
@synthesize tab_overview_toolbar_item = _tab_overview_toolbar_item;

- (instancetype)init:(BOOL)block_popups
{
    if (self = [super init]) {
        self.toolbar = [[NSToolbar alloc] initWithIdentifier:TOOLBAR_IDENTIFIER];
        [self.toolbar setDelegate:self];
        [self.toolbar setDisplayMode:NSToolbarDisplayModeIconOnly];
        [self.toolbar setAllowsUserCustomization:NO];
        [self.toolbar setSizeMode:NSToolbarSizeModeRegular];

        m_settings = { .block_popups = block_popups };
        m_can_navigate_back = false;
        m_can_navigate_forward = false;
    }

    return self;
}

#pragma mark - Public methods

- (void)loadURL:(URL::URL const&)url
{
    [[self tab].web_view loadURL:url];
}

- (void)loadHTML:(StringView)html url:(URL::URL const&)url
{
    [[self tab].web_view loadHTML:html];
}

- (void)onLoadStart:(URL::URL const&)url isRedirect:(BOOL)isRedirect
{
    [self setLocationFieldText:url.serialize()];
}

- (void)onURLChange:(URL::URL const&)url
{
    [self setLocationFieldText:url.serialize()];
}

- (void)onBackNavigationEnabled:(BOOL)back_enabled
       forwardNavigationEnabled:(BOOL)forward_enabled
{
    m_can_navigate_back = back_enabled;
    m_can_navigate_forward = forward_enabled;
    [self updateNavigationButtonStates];
}

- (void)onTitleChange:(ByteString const&)title
{
    m_title = title;
}

- (void)zoomIn:(id)sender
{
    [[[self tab] web_view] zoomIn];
    [self updateZoomButton];
}

- (void)zoomOut:(id)sender
{
    [[[self tab] web_view] zoomOut];
    [self updateZoomButton];
}

- (void)resetZoom:(id)sender
{
    [[[self tab] web_view] resetZoom];
    [self updateZoomButton];
}

- (void)navigateBack:(id)sender
{
    [[[self tab] web_view] navigateBack];
}

- (void)navigateForward:(id)sender
{
    [[[self tab] web_view] navigateForward];
}

- (void)reload:(id)sender
{
    [[[self tab] web_view] reload];
}

- (void)clearHistory
{
    // FIXME: Reimplement clearing history using WebContent's history.
}

- (void)debugRequest:(ByteString const&)request argument:(ByteString const&)argument
{
    [[[self tab] web_view] debugRequest:request argument:argument];
}

- (void)viewSource:(id)sender
{
    [[[self tab] web_view] viewSource];
}

- (void)focusLocationToolbarItem
{
    [self.window makeFirstResponder:self.location_toolbar_item.view];
}

#pragma mark - Private methods

- (Tab*)tab
{
    return (Tab*)[self window];
}

- (void)createNewTab:(id)sender
{
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];

    self.tab.titlebarAppearsTransparent = NO;

    [delegate createNewTab:OptionalNone {}
                   fromTab:[self tab]
               activateTab:Web::HTML::ActivateTab::Yes];

    self.tab.titlebarAppearsTransparent = YES;
}

- (void)setLocationFieldText:(StringView)url
{
    NSMutableAttributedString* attributed_url;

    auto* dark_attributes = @{
        NSForegroundColorAttributeName : [NSColor systemGrayColor],
    };
    auto* highlight_attributes = @{
        NSForegroundColorAttributeName : [NSColor textColor],
    };

    if (auto url_parts = WebView::break_url_into_parts(url); url_parts.has_value()) {
        attributed_url = [[NSMutableAttributedString alloc] init];

        auto* attributed_scheme_and_subdomain = [[NSAttributedString alloc]
            initWithString:Ladybird::string_to_ns_string(url_parts->scheme_and_subdomain)
                attributes:dark_attributes];

        auto* attributed_effective_tld_plus_one = [[NSAttributedString alloc]
            initWithString:Ladybird::string_to_ns_string(url_parts->effective_tld_plus_one)
                attributes:highlight_attributes];

        auto* attributed_remainder = [[NSAttributedString alloc]
            initWithString:Ladybird::string_to_ns_string(url_parts->remainder)
                attributes:dark_attributes];

        [attributed_url appendAttributedString:attributed_scheme_and_subdomain];
        [attributed_url appendAttributedString:attributed_effective_tld_plus_one];
        [attributed_url appendAttributedString:attributed_remainder];
    } else {
        attributed_url = [[NSMutableAttributedString alloc]
            initWithString:Ladybird::string_to_ns_string(url)
                attributes:highlight_attributes];
    }

    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];
    [location_search_field setAttributedStringValue:attributed_url];
}

- (void)updateNavigationButtonStates
{
    auto* navigate_back_button = (NSButton*)[[self navigate_back_toolbar_item] view];
    [navigate_back_button setEnabled:m_can_navigate_back];

    auto* navigate_forward_button = (NSButton*)[[self navigate_forward_toolbar_item] view];
    [navigate_forward_button setEnabled:m_can_navigate_forward];
}

- (void)showTabOverview:(id)sender
{
    self.tab.titlebarAppearsTransparent = NO;
    [self.window toggleTabOverview:sender];
    self.tab.titlebarAppearsTransparent = YES;
}

- (void)updateZoomButton
{
    auto zoom_level = [[[self tab] web_view] zoomLevel];

    auto* zoom_level_text = [NSString stringWithFormat:@"%d%%", round_to<int>(zoom_level * 100.0f)];
    [self.zoom_toolbar_item setTitle:zoom_level_text];

    auto zoom_button_hidden = zoom_level == 1.0 ? YES : NO;
    [[self.zoom_toolbar_item view] setHidden:zoom_button_hidden];
}

- (void)dumpDOMTree:(id)sender
{
    [self debugRequest:"dump-dom-tree" argument:""];
}

- (void)dumpLayoutTree:(id)sender
{
    [self debugRequest:"dump-layout-tree" argument:""];
}

- (void)dumpPaintTree:(id)sender
{
    [self debugRequest:"dump-paint-tree" argument:""];
}

- (void)dumpStackingContextTree:(id)sender
{
    [self debugRequest:"dump-stacking-context-tree" argument:""];
}

- (void)dumpStyleSheets:(id)sender
{
    [self debugRequest:"dump-style-sheets" argument:""];
}

- (void)dumpAllResolvedStyles:(id)sender
{
    [self debugRequest:"dump-all-resolved-styles" argument:""];
}

- (void)dumpHistory:(id)sender
{
    [self debugRequest:"dump-session-history" argument:""];
}

- (void)dumpLocalStorage:(id)sender
{
    [self debugRequest:"dump-local-storage" argument:""];
}

- (void)toggleLineBoxBorders:(id)sender
{
    m_settings.should_show_line_box_borders = !m_settings.should_show_line_box_borders;
    [self debugRequest:"set-line-box-borders" argument:m_settings.should_show_line_box_borders ? "on" : "off"];
}

- (void)collectGarbage:(id)sender
{
    [self debugRequest:"collect-garbage" argument:""];
}

- (void)dumpGCGraph:(id)sender
{
    [self debugRequest:"dump-gc-graph" argument:""];
}

- (void)clearCache:(id)sender
{
    [self debugRequest:"clear-cache" argument:""];
}

- (void)toggleScripting:(id)sender
{
    m_settings.scripting_enabled = !m_settings.scripting_enabled;
    [self debugRequest:"scripting" argument:m_settings.scripting_enabled ? "on" : "off"];
}

- (void)togglePopupBlocking:(id)sender
{
    m_settings.block_popups = !m_settings.block_popups;
    [self debugRequest:"block-pop-ups" argument:m_settings.block_popups ? "on" : "off"];
}

- (void)toggleSameOriginPolicy:(id)sender
{
    m_settings.same_origin_policy_enabled = !m_settings.same_origin_policy_enabled;
    [self debugRequest:"same-origin-policy" argument:m_settings.same_origin_policy_enabled ? "on" : "off"];
}

- (void)setUserAgentSpoof:(NSMenuItem*)sender
{
    ByteString const user_agent_name = [[sender title] UTF8String];
    ByteString user_agent = "";
    if (user_agent_name == "Disabled"sv) {
        user_agent = Web::default_user_agent;
    } else {
        user_agent = WebView::user_agents.get(user_agent_name).value();
    }
    m_settings.user_agent_name = user_agent_name;

    [self debugRequest:"spoof-user-agent" argument:user_agent];
    [self debugRequest:"clear-cache" argument:""]; // clear the cache to ensure requests are re-done with the new user agent
}

#pragma mark - Properties

- (NSButton*)create_button:(NSImageName)image
               with_action:(nonnull SEL)action
              with_tooltip:(NSString*)tooltip
{
    auto* button = [NSButton buttonWithImage:[NSImage imageNamed:image]
                                      target:self
                                      action:action];
    if (tooltip) {
        [button setToolTip:tooltip];
    }

    [button setBordered:YES];

    return button;
}

- (NSToolbarItem*)navigate_back_toolbar_item
{
    if (!_navigate_back_toolbar_item) {
        auto* button = [self create_button:NSImageNameGoBackTemplate
                               with_action:@selector(navigateBack:)
                              with_tooltip:@"Navigate back"];
        [button setEnabled:NO];

        _navigate_back_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_NAVIGATE_BACK_IDENTIFIER];
        [_navigate_back_toolbar_item setView:button];
    }

    return _navigate_back_toolbar_item;
}

- (NSToolbarItem*)navigate_forward_toolbar_item
{
    if (!_navigate_forward_toolbar_item) {
        auto* button = [self create_button:NSImageNameGoForwardTemplate
                               with_action:@selector(navigateForward:)
                              with_tooltip:@"Navigate forward"];
        [button setEnabled:NO];

        _navigate_forward_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_NAVIGATE_FORWARD_IDENTIFIER];
        [_navigate_forward_toolbar_item setView:button];
    }

    return _navigate_forward_toolbar_item;
}

- (NSToolbarItem*)reload_toolbar_item
{
    if (!_reload_toolbar_item) {
        auto* button = [self create_button:NSImageNameRefreshTemplate
                               with_action:@selector(reload:)
                              with_tooltip:@"Reload page"];
        [button setEnabled:YES];

        _reload_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_RELOAD_IDENTIFIER];
        [_reload_toolbar_item setView:button];
    }

    return _reload_toolbar_item;
}

- (NSToolbarItem*)location_toolbar_item
{
    if (!_location_toolbar_item) {
        auto* location_search_field = [[LocationSearchField alloc] init];
        [location_search_field setPlaceholderString:@"Enter web address"];
        [location_search_field setTextColor:[NSColor textColor]];
        [location_search_field setDelegate:self];

        _location_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_LOCATION_IDENTIFIER];
        [_location_toolbar_item setView:location_search_field];
    }

    return _location_toolbar_item;
}

- (NSToolbarItem*)zoom_toolbar_item
{
    if (!_zoom_toolbar_item) {
        auto* button = [NSButton buttonWithTitle:@"100%"
                                          target:self
                                          action:@selector(resetZoom:)];
        [button setToolTip:@"Reset zoom level"];
        [button setHidden:YES];

        _zoom_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_ZOOM_IDENTIFIER];
        [_zoom_toolbar_item setView:button];
    }

    return _zoom_toolbar_item;
}

- (NSToolbarItem*)new_tab_toolbar_item
{
    if (!_new_tab_toolbar_item) {
        auto* button = [self create_button:NSImageNameAddTemplate
                               with_action:@selector(createNewTab:)
                              with_tooltip:@"New tab"];

        _new_tab_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_NEW_TAB_IDENTIFIER];
        [_new_tab_toolbar_item setView:button];
    }

    return _new_tab_toolbar_item;
}

- (NSToolbarItem*)tab_overview_toolbar_item
{
    if (!_tab_overview_toolbar_item) {
        auto* button = [self create_button:NSImageNameIconViewTemplate
                               with_action:@selector(showTabOverview:)
                              with_tooltip:@"Show all tabs"];

        _tab_overview_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_TAB_OVERVIEW_IDENTIFIER];
        [_tab_overview_toolbar_item setView:button];
    }

    return _tab_overview_toolbar_item;
}

- (NSArray*)toolbar_identifiers
{
    if (!_toolbar_identifiers) {
        _toolbar_identifiers = @[
            TOOLBAR_NAVIGATE_BACK_IDENTIFIER,
            TOOLBAR_NAVIGATE_FORWARD_IDENTIFIER,
            NSToolbarFlexibleSpaceItemIdentifier,
            TOOLBAR_RELOAD_IDENTIFIER,
            TOOLBAR_LOCATION_IDENTIFIER,
            TOOLBAR_ZOOM_IDENTIFIER,
            NSToolbarFlexibleSpaceItemIdentifier,
            TOOLBAR_NEW_TAB_IDENTIFIER,
            TOOLBAR_TAB_OVERVIEW_IDENTIFIER,
        ];
    }

    return _toolbar_identifiers;
}

#pragma mark - NSWindowController

- (IBAction)showWindow:(id)sender
{
    self.window = [[Tab alloc] init];
    [self.window setDelegate:self];

    [self.window setToolbar:self.toolbar];
    [self.window setToolbarStyle:NSWindowToolbarStyleUnified];

    [self.window makeKeyAndOrderFront:sender];

    [self focusLocationToolbarItem];

    auto* delegate = (ApplicationDelegate*)[NSApp delegate];
    [delegate setActiveTab:[self tab]];
}

#pragma mark - NSWindowDelegate

- (void)windowDidBecomeMain:(NSNotification*)notification
{
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];
    [delegate setActiveTab:[self tab]];
}

- (void)windowWillClose:(NSNotification*)notification
{
    [[self tab] tabWillClose];

    auto* delegate = (ApplicationDelegate*)[NSApp delegate];
    [delegate removeTab:self];
}

- (void)windowDidResize:(NSNotification*)notification
{
    if (self.location_toolbar_item_width != nil) {
        self.location_toolbar_item_width.active = NO;
    }

    auto width = [self window].frame.size.width * 0.6;
    self.location_toolbar_item_width = [[[self.location_toolbar_item view] widthAnchor] constraintEqualToConstant:width];
    self.location_toolbar_item_width.active = YES;

    if (![[self window] inLiveResize]) {
        [[[self tab] web_view] handleResize];
    }
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
    [[[self tab] web_view] handleDevicePixelRatioChange];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item
{
    if ([item action] == @selector(toggleLineBoxBorders:)) {
        [item setState:m_settings.should_show_line_box_borders ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(toggleScripting:)) {
        [item setState:m_settings.scripting_enabled ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(togglePopupBlocking:)) {
        [item setState:m_settings.block_popups ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(toggleSameOriginPolicy:)) {
        [item setState:m_settings.same_origin_policy_enabled ? NSControlStateValueOn : NSControlStateValueOff];
    } else if ([item action] == @selector(setUserAgentSpoof:)) {
        [item setState:(m_settings.user_agent_name == [[item title] UTF8String]) ? NSControlStateValueOn : NSControlStateValueOff];
    }

    return YES;
}

#pragma mark - NSToolbarDelegate

- (NSToolbarItem*)toolbar:(NSToolbar*)toolbar
        itemForItemIdentifier:(NSString*)identifier
    willBeInsertedIntoToolbar:(BOOL)flag
{
    if ([identifier isEqual:TOOLBAR_NAVIGATE_BACK_IDENTIFIER]) {
        return self.navigate_back_toolbar_item;
    }
    if ([identifier isEqual:TOOLBAR_NAVIGATE_FORWARD_IDENTIFIER]) {
        return self.navigate_forward_toolbar_item;
    }
    if ([identifier isEqual:TOOLBAR_RELOAD_IDENTIFIER]) {
        return self.reload_toolbar_item;
    }
    if ([identifier isEqual:TOOLBAR_LOCATION_IDENTIFIER]) {
        return self.location_toolbar_item;
    }
    if ([identifier isEqual:TOOLBAR_ZOOM_IDENTIFIER]) {
        return self.zoom_toolbar_item;
    }
    if ([identifier isEqual:TOOLBAR_NEW_TAB_IDENTIFIER]) {
        return self.new_tab_toolbar_item;
    }
    if ([identifier isEqual:TOOLBAR_TAB_OVERVIEW_IDENTIFIER]) {
        return self.tab_overview_toolbar_item;
    }

    return nil;
}

- (NSArray*)toolbarAllowedItemIdentifiers:(NSToolbar*)toolbar
{
    return self.toolbar_identifiers;
}

- (NSArray*)toolbarDefaultItemIdentifiers:(NSToolbar*)toolbar
{
    return self.toolbar_identifiers;
}

#pragma mark - NSSearchFieldDelegate

- (BOOL)control:(NSControl*)control
               textView:(NSTextView*)text_view
    doCommandBySelector:(SEL)selector
{
    if (selector != @selector(insertNewline:)) {
        return NO;
    }

    auto url_string = Ladybird::ns_string_to_string([[text_view textStorage] string]);
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];

    if (auto url = WebView::sanitize_url(url_string, [delegate searchEngine].query_url); url.has_value()) {
        [self loadURL:*url];
    }

    [self.window makeFirstResponder:nil];
    return YES;
}

- (void)controlTextDidEndEditing:(NSNotification*)notification
{
    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];

    auto url_string = Ladybird::ns_string_to_string([location_search_field stringValue]);
    [self setLocationFieldText:url_string];
}

@end
