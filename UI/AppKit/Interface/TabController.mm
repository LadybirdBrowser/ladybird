/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/URL.h>
#include <LibWebView/ViewImplementation.h>

#import <Application/ApplicationDelegate.h>
#import <Interface/Autocomplete.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/Menu.h>
#import <Interface/Tab.h>
#import <Interface/TabController.h>
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

@interface TabController () <NSToolbarDelegate, NSSearchFieldDelegate, AutocompleteObserver>
{
    u64 m_page_index;

    OwnPtr<WebView::Autocomplete> m_autocomplete;
}

@property (nonatomic, strong) Tab* parent;

@property (nonatomic, strong) NSToolbar* toolbar;
@property (nonatomic, strong) NSArray* toolbar_identifiers;

@property (nonatomic, strong) NSToolbarItem* navigate_back_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* navigate_forward_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* reload_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* location_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* zoom_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* new_tab_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* tab_overview_toolbar_item;

@property (nonatomic, strong) Autocomplete* autocomplete;

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

- (instancetype)init
{
    if (self = [super init]) {
        __weak TabController* weak_self = self;

        self.toolbar = [[NSToolbar alloc] initWithIdentifier:TOOLBAR_IDENTIFIER];
        [self.toolbar setDelegate:self];
        [self.toolbar setDisplayMode:NSToolbarDisplayModeIconOnly];
        if (@available(macOS 15, *)) {
            [self.toolbar setAllowsDisplayModeCustomization:NO];
        }
        [self.toolbar setAllowsUserCustomization:NO];
        [self.toolbar setSizeMode:NSToolbarSizeModeRegular];

        m_page_index = 0;

        self.autocomplete = [[Autocomplete alloc] init:self withToolbarItem:self.location_toolbar_item];
        m_autocomplete = make<WebView::Autocomplete>();

        m_autocomplete->on_autocomplete_query_complete = [weak_self](auto suggestions) {
            TabController* self = weak_self;
            if (self == nil) {
                return;
            }

            [self.autocomplete showWithSuggestions:move(suggestions)];
        };
    }

    return self;
}

- (instancetype)initAsChild:(Tab*)parent
                  pageIndex:(u64)page_index
{
    if (self = [self init]) {
        self.parent = parent;
        m_page_index = page_index;
    }

    return self;
}

#pragma mark - Public methods

- (void)loadURL:(URL::URL const&)url
{
    [[self tab].web_view loadURL:url];
}

- (void)onLoadStart:(URL::URL const&)url isRedirect:(BOOL)isRedirect
{
    [self setLocationFieldText:url.serialize()];
}

- (void)onURLChange:(URL::URL const&)url
{
    [self setLocationFieldText:url.serialize()];
    [self.window makeFirstResponder:[self tab].web_view];
}

- (void)clearHistory
{
    // FIXME: Reimplement clearing history using WebContent's history.
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

    [delegate createNewTab:WebView::Application::settings().new_tab_page_url()
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

- (BOOL)navigateToLocation:(String)location
{
    if (auto url = WebView::sanitize_url(location, WebView::Application::settings().search_engine()); url.has_value()) {
        [self loadURL:*url];
    }

    [self.window makeFirstResponder:nil];
    [self.autocomplete close];

    return YES;
}

- (void)showTabOverview:(id)sender
{
    self.tab.titlebarAppearsTransparent = NO;
    [self.window toggleTabOverview:sender];
    self.tab.titlebarAppearsTransparent = YES;
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
        auto* button = Ladybird::create_application_button([[[self tab] web_view] view].navigate_back_action());

        _navigate_back_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_NAVIGATE_BACK_IDENTIFIER];
        [_navigate_back_toolbar_item setView:button];
    }

    return _navigate_back_toolbar_item;
}

- (NSToolbarItem*)navigate_forward_toolbar_item
{
    if (!_navigate_forward_toolbar_item) {
        auto* button = Ladybird::create_application_button([[[self tab] web_view] view].navigate_forward_action());

        _navigate_forward_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_NAVIGATE_FORWARD_IDENTIFIER];
        [_navigate_forward_toolbar_item setView:button];
    }

    return _navigate_forward_toolbar_item;
}

- (NSToolbarItem*)reload_toolbar_item
{
    if (!_reload_toolbar_item) {
        auto* button = Ladybird::create_application_button(WebView::Application::the().reload_action());

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

        if (@available(macOS 26, *)) {
            [location_search_field setBordered:YES];
        }

        _location_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_LOCATION_IDENTIFIER];
        [_location_toolbar_item setView:location_search_field];
    }

    return _location_toolbar_item;
}

- (NSToolbarItem*)zoom_toolbar_item
{
    if (!_zoom_toolbar_item) {
        auto* button = Ladybird::create_application_button([[[self tab] web_view] view].reset_zoom_action());

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
    self.window = self.parent
        ? [[Tab alloc] initAsChild:self.parent pageIndex:m_page_index]
        : [[Tab alloc] init];

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
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];
    [delegate removeTab:self];
}

- (void)windowDidMove:(NSNotification*)notification
{
    auto position = Ladybird::ns_point_to_gfx_point([[self tab] frame].origin);
    [[[self tab] web_view] setWindowPosition:position];
}

- (void)windowDidResize:(NSNotification*)notification
{
    if (self.location_toolbar_item_width != nil) {
        self.location_toolbar_item_width.active = NO;
    }

    auto width = [self window].frame.size.width * 0.6;
    self.location_toolbar_item_width = [[[self.location_toolbar_item view] widthAnchor] constraintEqualToConstant:width];
    self.location_toolbar_item_width.active = YES;

    [[[self tab] web_view] handleResize];
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification
{
    [[[self tab] web_view] handleDevicePixelRatioChange];
}

- (void)windowDidChangeScreen:(NSNotification*)notification
{
    [[[self tab] web_view] handleDisplayRefreshRateChange];
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
    if (selector == @selector(cancelOperation:)) {
        if ([self.autocomplete close])
            return YES;
    }

    if (selector == @selector(moveDown:)) {
        if ([self.autocomplete selectNextSuggestion])
            return YES;
    }

    if (selector == @selector(moveUp:)) {
        if ([self.autocomplete selectPreviousSuggestion])
            return YES;
    }

    if (selector != @selector(insertNewline:)) {
        return NO;
    }

    auto location = [self.autocomplete selectedSuggestion].value_or_lazy_evaluated([&]() {
        return Ladybird::ns_string_to_string([[text_view textStorage] string]);
    });

    [self navigateToLocation:move(location)];
    return YES;
}

- (void)controlTextDidEndEditing:(NSNotification*)notification
{
    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];

    auto url_string = Ladybird::ns_string_to_string([location_search_field stringValue]);
    [self setLocationFieldText:url_string];
}

- (void)controlTextDidChange:(NSNotification*)notification
{
    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];

    auto url_string = Ladybird::ns_string_to_string([location_search_field stringValue]);
    m_autocomplete->query_autocomplete_engine(move(url_string));
}

#pragma mark - AutocompleteObserver

- (void)onSelectedSuggestion:(String)suggestion
{
    [self navigateToLocation:move(suggestion)];
}

@end
