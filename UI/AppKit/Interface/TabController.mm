/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/BookmarkStore.h>
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
static NSString* const TOOLBAR_BOOKMARK_IDENTIFIER = @"ToolbarBookmarkIdentifier";
static NSString* const TOOLBAR_NEW_TAB_IDENTIFIER = @"ToolbarNewTabIdentifier";
static NSString* const TOOLBAR_TAB_OVERVIEW_IDENTIFIER = @"ToolbarTabOverviewIdentifier";

static NSString* candidate_by_trimming_root_trailing_slash(NSString* candidate);

static bool query_matches_candidate_exactly(NSString* query, NSString* candidate)
{
    auto* trimmed_candidate = candidate_by_trimming_root_trailing_slash(candidate);
    return [trimmed_candidate compare:query options:NSCaseInsensitiveSearch] == NSOrderedSame;
}

static NSString* inline_autocomplete_text_for_candidate(NSString* query, NSString* candidate)
{
    if (query.length == 0 || candidate.length <= query.length)
        return nil;

    auto prefix_range = [candidate rangeOfString:query options:NSCaseInsensitiveSearch | NSAnchoredSearch];
    if (prefix_range.location == NSNotFound)
        return nil;

    auto* suffix = [candidate substringFromIndex:query.length];
    return [query stringByAppendingString:suffix];
}

static NSString* inline_autocomplete_text_for_suggestion(NSString* query, NSString* suggestion_text)
{
    auto* trimmed_suggestion_text = candidate_by_trimming_root_trailing_slash(suggestion_text);

    if (auto* direct_match = inline_autocomplete_text_for_candidate(query, trimmed_suggestion_text); direct_match != nil)
        return direct_match;

    if ([trimmed_suggestion_text hasPrefix:@"www."]) {
        auto* stripped_www_suggestion = [trimmed_suggestion_text substringFromIndex:4];
        if (auto* stripped_www_match = inline_autocomplete_text_for_candidate(query, stripped_www_suggestion); stripped_www_match != nil)
            return stripped_www_match;
    }

    for (NSString* scheme_prefix in @[ @"https://", @"http://" ]) {
        if (![trimmed_suggestion_text hasPrefix:scheme_prefix])
            continue;

        auto* stripped_suggestion = [trimmed_suggestion_text substringFromIndex:scheme_prefix.length];
        if (auto* stripped_match = inline_autocomplete_text_for_candidate(query, stripped_suggestion); stripped_match != nil)
            return stripped_match;

        if ([stripped_suggestion hasPrefix:@"www."]) {
            auto* stripped_www_suggestion = [stripped_suggestion substringFromIndex:4];
            if (auto* stripped_www_match = inline_autocomplete_text_for_candidate(query, stripped_www_suggestion); stripped_www_match != nil)
                return stripped_www_match;
        }
    }

    return nil;
}

static bool suggestion_matches_query_exactly(NSString* query, NSString* suggestion_text)
{
    auto* trimmed_suggestion_text = candidate_by_trimming_root_trailing_slash(suggestion_text);

    if (query_matches_candidate_exactly(query, trimmed_suggestion_text))
        return true;

    if ([trimmed_suggestion_text hasPrefix:@"www."]) {
        auto* stripped_www_suggestion = [trimmed_suggestion_text substringFromIndex:4];
        if (query_matches_candidate_exactly(query, stripped_www_suggestion))
            return true;
    }

    for (NSString* scheme_prefix in @[ @"https://", @"http://" ]) {
        if (![trimmed_suggestion_text hasPrefix:scheme_prefix])
            continue;

        auto* stripped_suggestion = [trimmed_suggestion_text substringFromIndex:scheme_prefix.length];
        if (query_matches_candidate_exactly(query, stripped_suggestion))
            return true;

        if ([stripped_suggestion hasPrefix:@"www."]) {
            auto* stripped_www_suggestion = [stripped_suggestion substringFromIndex:4];
            if (query_matches_candidate_exactly(query, stripped_www_suggestion))
                return true;
        }
    }

    return false;
}

static NSString* candidate_by_trimming_root_trailing_slash(NSString* candidate)
{
    if (![candidate hasSuffix:@"/"])
        return candidate;

    auto* host_and_path = candidate;
    for (NSString* scheme_prefix in @[ @"https://", @"http://" ]) {
        if ([host_and_path hasPrefix:scheme_prefix]) {
            host_and_path = [host_and_path substringFromIndex:scheme_prefix.length];
            break;
        }
    }

    auto first_slash = [host_and_path rangeOfString:@"/"];
    if (first_slash.location == NSNotFound || first_slash.location != host_and_path.length - 1)
        return candidate;

    return [candidate substringToIndex:candidate.length - 1];
}

static bool should_suppress_inline_autocomplete_for_selector(SEL selector)
{
    return selector == @selector(deleteBackward:)
        || selector == @selector(deleteBackwardByDecomposingPreviousCharacter:)
        || selector == @selector(deleteForward:)
        || selector == @selector(deleteToBeginningOfLine:)
        || selector == @selector(deleteToEndOfLine:)
        || selector == @selector(deleteWordBackward:)
        || selector == @selector(deleteWordForward:);
}

static NSInteger autocomplete_suggestion_index(NSString* suggestion_text, Vector<WebView::AutocompleteSuggestion> const& suggestions)
{
    for (size_t index = 0; index < suggestions.size(); ++index) {
        auto* candidate_text = Ladybird::string_to_ns_string(suggestions[index].text);
        if ([candidate_text isEqualToString:suggestion_text])
            return static_cast<NSInteger>(index);
    }

    return NSNotFound;
}

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

// NSSearchField does not provide an intrinsic width, which causes an ambiguous layout warning when the toolbar auto-
// measures this view. This provides an initial fallback, which is overridden with an explicit width in windowDidResize.
- (NSSize)intrinsicContentSize
{
    auto size = [super intrinsicContentSize];
    if (size.width < 0)
        size.width = 400;
    return size;
}

@end

@interface TabController () <NSToolbarDelegate, NSSearchFieldDelegate, AutocompleteObserver>
{
    u64 m_page_index;

    OwnPtr<WebView::Autocomplete> m_autocomplete;
    bool m_is_applying_inline_autocomplete;
    bool m_should_suppress_inline_autocomplete_on_next_change;

    bool m_fullscreen_requested_for_web_content;
    bool m_fullscreen_exit_was_ui_initiated;
    bool m_fullscreen_should_restore_tab_bar;
}

@property (nonatomic, assign) BOOL already_requested_close;

@property (nonatomic, strong) Tab* parent;

@property (nonatomic, strong) NSToolbar* toolbar;
@property (nonatomic, strong) NSArray* toolbar_identifiers;

@property (nonatomic, strong) NSToolbarItem* navigate_back_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* navigate_forward_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* reload_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* location_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* zoom_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* bookmark_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* new_tab_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* tab_overview_toolbar_item;

@property (nonatomic, strong) Autocomplete* autocomplete;
@property (nonatomic, copy) NSString* current_inline_autocomplete_suggestion;
@property (nonatomic, copy) NSString* suppressed_inline_autocomplete_query;

@property (nonatomic, assign) NSLayoutConstraint* location_toolbar_item_width;

- (NSString*)currentLocationFieldQuery;
- (BOOL)applyInlineAutocompleteSuggestionText:(NSString*)suggestion_text
                                     forQuery:(NSString*)query;
- (void)applyLocationFieldInlineAutocompleteText:(NSString*)inline_text
                                        forQuery:(NSString*)query;
- (NSInteger)applyInlineAutocomplete:(Vector<WebView::AutocompleteSuggestion> const&)suggestions;
- (void)previewHighlightedSuggestionInLocationField:(String const&)suggestion;
- (void)restoreLocationFieldQuery;

@end

@implementation TabController

@synthesize toolbar_identifiers = _toolbar_identifiers;
@synthesize navigate_back_toolbar_item = _navigate_back_toolbar_item;
@synthesize navigate_forward_toolbar_item = _navigate_forward_toolbar_item;
@synthesize reload_toolbar_item = _reload_toolbar_item;
@synthesize location_toolbar_item = _location_toolbar_item;
@synthesize zoom_toolbar_item = _zoom_toolbar_item;
@synthesize bookmark_toolbar_item = _bookmark_toolbar_item;
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
            if ([self.toolbar respondsToSelector:@selector(setAllowsDisplayModeCustomization:)]) {
                [self.toolbar performSelector:@selector(setAllowsDisplayModeCustomization:) withObject:nil];
            }
        }
        [self.toolbar setAllowsUserCustomization:NO];
        [self.toolbar setSizeMode:NSToolbarSizeModeRegular];

        m_page_index = 0;
        m_is_applying_inline_autocomplete = false;
        m_should_suppress_inline_autocomplete_on_next_change = false;
        m_fullscreen_requested_for_web_content = false;
        m_fullscreen_exit_was_ui_initiated = true;
        m_fullscreen_should_restore_tab_bar = false;

        self.autocomplete = [[Autocomplete alloc] init:self withToolbarItem:self.location_toolbar_item];
        m_autocomplete = make<WebView::Autocomplete>();

        m_autocomplete->on_autocomplete_query_complete = [weak_self](auto suggestions, WebView::AutocompleteResultKind result_kind) {
            TabController* self = weak_self;
            if (self == nil) {
                return;
            }

            auto selected_row = [self applyInlineAutocomplete:suggestions];
            if (result_kind == WebView::AutocompleteResultKind::Intermediate && [self.autocomplete isVisible]) {
                if (auto selected_suggestion = [self.autocomplete selectedSuggestion];
                    selected_suggestion.has_value()) {
                    for (auto const& suggestion : suggestions) {
                        if (suggestion.text == *selected_suggestion)
                            return;
                    }
                }

                [self.autocomplete clearSelection];
                return;
            }

            [self.autocomplete showWithSuggestions:move(suggestions)
                                       selectedRow:selected_row];
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
        m_fullscreen_requested_for_web_content = false;
        m_fullscreen_exit_was_ui_initiated = true;
        m_fullscreen_should_restore_tab_bar = false;
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

    // Don't steal focus from the location bar when loading the new tab page
    if (url != WebView::Application::settings().new_tab_page_url())
        [self focusWebView];
}

- (void)onEnterFullscreenWindow
{
    m_fullscreen_requested_for_web_content = true;

    if (([self.window styleMask] & NSWindowStyleMaskFullScreen) == 0) {
        [self.window toggleFullScreen:nil];
    }
}

- (void)onExitFullscreenWindow
{
    if (([self.window styleMask] & NSWindowStyleMaskFullScreen) != 0) {
        m_fullscreen_exit_was_ui_initiated = false;
        [self.window toggleFullScreen:nil];
    }
}

- (void)focusLocationToolbarItem
{
    [self tab].preferred_first_responder = self.location_toolbar_item.view;
    [self.window makeFirstResponder:self.location_toolbar_item.view];
}

- (void)focusWebViewWhenActivated
{
    [self tab].preferred_first_responder = [self tab].web_view;
}

- (void)focusWebView
{
    [self tab].preferred_first_responder = [self tab].web_view;
    [self.window makeFirstResponder:[self tab].web_view];
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

- (NSString*)currentLocationFieldQuery
{
    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];
    auto* editor = (NSTextView*)[location_search_field currentEditor];

    // Inline autocomplete mutates the field contents in place, so callers
    // need a detached copy of the typed prefix for asynchronous comparisons.
    if (editor == nil || [self.window firstResponder] != editor)
        return [[location_search_field stringValue] copy];

    auto* text = [[editor textStorage] string];
    auto selected_range = [editor selectedRange];
    if (selected_range.location == NSNotFound)
        return [text copy];

    if (selected_range.length == 0)
        return [text copy];

    if (NSMaxRange(selected_range) != text.length)
        return [text copy];

    return [[text substringToIndex:selected_range.location] copy];
}

- (NSInteger)applyInlineAutocomplete:(Vector<WebView::AutocompleteSuggestion> const&)suggestions
{
    if (m_is_applying_inline_autocomplete)
        return NSNotFound;

    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];
    auto* editor = (NSTextView*)[location_search_field currentEditor];

    if (editor == nil || [self.window firstResponder] != editor || [editor hasMarkedText])
        return NSNotFound;

    auto* current_text = [[editor textStorage] string];
    auto selected_range = [editor selectedRange];
    if (selected_range.location == NSNotFound)
        return NSNotFound;

    auto current_text_length = current_text.length;

    NSString* query = nil;
    if (selected_range.length == 0) {
        if (selected_range.location != current_text_length)
            return NSNotFound;
        query = current_text;
    } else {
        if (NSMaxRange(selected_range) != current_text_length)
            return NSNotFound;
        query = [current_text substringToIndex:selected_range.location];
    }

    if (suggestions.is_empty())
        return NSNotFound;

    // Row 0 drives both the visible highlight and (if its text prefix-matches
    // the query) the inline completion preview. The user-visible rule is
    // "the top row is the default action"; see the Qt implementation in
    // UI/Qt/LocationEdit.cpp for a longer discussion.

    // A literal URL always wins: no preview, restore the typed text.
    if (suggestions.first().source == WebView::AutocompleteSuggestionSource::LiteralURL) {
        self.current_inline_autocomplete_suggestion = nil;
        if (selected_range.length != 0 || ![current_text isEqualToString:query])
            [self restoreLocationFieldQuery];
        return 0;
    }

    // Backspace suppression: the user just deleted into this query, so don't
    // re-apply an inline preview — but still honor the "highlight the top
    // row" rule.
    if (self.suppressed_inline_autocomplete_query != nil && [self.suppressed_inline_autocomplete_query isEqualToString:query]) {
        self.current_inline_autocomplete_suggestion = nil;
        if (selected_range.length != 0 || ![current_text isEqualToString:query])
            [self restoreLocationFieldQuery];
        return 0;
    }

    // Preserve an existing inline preview if its row is still present and
    // still extends the typed prefix. This keeps the preview stable while the
    // user is still forward-typing into a suggestion.
    if (self.current_inline_autocomplete_suggestion != nil) {
        auto preserved_row = autocomplete_suggestion_index(self.current_inline_autocomplete_suggestion, suggestions);
        if (preserved_row != NSNotFound) {
            if (auto* preserved_inline = inline_autocomplete_text_for_suggestion(query, self.current_inline_autocomplete_suggestion); preserved_inline != nil) {
                [self applyLocationFieldInlineAutocompleteText:preserved_inline forQuery:query];
                return preserved_row;
            }
        }
    }

    // Try to inline-preview row 0 specifically.
    auto* row_0_text = Ladybird::string_to_ns_string(suggestions.first().text);
    if (auto* row_0_inline = inline_autocomplete_text_for_suggestion(query, row_0_text); row_0_inline != nil) {
        self.current_inline_autocomplete_suggestion = row_0_text;
        [self applyLocationFieldInlineAutocompleteText:row_0_inline forQuery:query];
        return 0;
    }

    // Row 0 does not prefix-match the query: clear any stale inline preview,
    // restore the typed text, and still highlight row 0.
    self.current_inline_autocomplete_suggestion = nil;
    if (selected_range.length != 0 || ![current_text isEqualToString:query])
        [self restoreLocationFieldQuery];
    return 0;
}

- (BOOL)applyInlineAutocompleteSuggestionText:(NSString*)suggestion_text
                                     forQuery:(NSString*)query
{
    if (suggestion_matches_query_exactly(query, suggestion_text)) {
        [self restoreLocationFieldQuery];
        self.current_inline_autocomplete_suggestion = nil;
        return YES;
    }

    auto* inline_text = inline_autocomplete_text_for_suggestion(query, suggestion_text);
    if (inline_text == nil)
        return NO;

    self.current_inline_autocomplete_suggestion = suggestion_text;
    [self applyLocationFieldInlineAutocompleteText:inline_text forQuery:query];
    return YES;
}

- (void)applyLocationFieldInlineAutocompleteText:(NSString*)inline_text
                                        forQuery:(NSString*)query
{
    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];
    auto* editor = (NSTextView*)[location_search_field currentEditor];

    if (editor == nil || [self.window firstResponder] != editor || [editor hasMarkedText])
        return;

    auto* current_text = [[editor textStorage] string];
    auto selected_range = [editor selectedRange];
    auto completion_range = NSMakeRange(query.length, inline_text.length - query.length);
    if ([current_text isEqualToString:inline_text] && NSEqualRanges(selected_range, completion_range))
        return;

    m_is_applying_inline_autocomplete = true;
    [location_search_field setStringValue:inline_text];
    [editor setString:inline_text];
    [editor setSelectedRange:completion_range];
    m_is_applying_inline_autocomplete = false;
}

- (void)previewHighlightedSuggestionInLocationField:(String const&)suggestion
{
    auto* query = [self currentLocationFieldQuery];
    auto* suggestion_text = Ladybird::string_to_ns_string(suggestion);
    [self applyInlineAutocompleteSuggestionText:suggestion_text forQuery:query];
}

- (void)restoreLocationFieldQuery
{
    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];
    auto* editor = (NSTextView*)[location_search_field currentEditor];

    if (editor == nil || [self.window firstResponder] != editor || [editor hasMarkedText])
        return;

    auto* query = [self currentLocationFieldQuery];
    auto* current_text = [[editor textStorage] string];
    auto selected_range = [editor selectedRange];
    auto query_selection = NSMakeRange(query.length, 0);
    if ([current_text isEqualToString:query] && NSEqualRanges(selected_range, query_selection))
        return;

    m_is_applying_inline_autocomplete = true;
    [location_search_field setStringValue:query];
    [editor setString:query];
    [editor setSelectedRange:query_selection];
    m_is_applying_inline_autocomplete = false;
}

- (BOOL)navigateToLocation:(String)location
{
    m_autocomplete->cancel_pending_query();

    if (auto url = WebView::sanitize_url(location, WebView::Application::settings().search_engine()); url.has_value()) {
        [self loadURL:*url];
    }

    self.current_inline_autocomplete_suggestion = nil;
    self.suppressed_inline_autocomplete_query = nil;
    m_should_suppress_inline_autocomplete_on_next_change = false;
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

- (NSToolbarItem*)bookmark_toolbar_item
{
    if (!_bookmark_toolbar_item) {
        auto* button = Ladybird::create_application_button([[[self tab] web_view] view].toggle_bookmark_action());

        _bookmark_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_BOOKMARK_IDENTIFIER];
        [_bookmark_toolbar_item setView:button];
    }

    return _bookmark_toolbar_item;
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
            TOOLBAR_BOOKMARK_IDENTIFIER,
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

- (void)windowDidResignKey:(NSNotification*)notification
{
    [self.autocomplete close];
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    // Prevent closing on first request so WebContent can cleanly shutdown (e.g. asking if the user is sure they want
    // to leave, closing WebSocket connections, etc.)
    if (!self.already_requested_close) {
        self.already_requested_close = true;
        [[[self tab] web_view] requestClose];
        return false;
    }

    // If the user has already requested a close, then respect the user's request and just close the tab.
    // For example, the WebContent process may not be responding.
    return true;
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

- (void)windowWillStartLiveResize:(NSNotification*)notification
{
    [self.autocomplete close];
}

- (void)windowDidResize:(NSNotification*)notification
{
    [self.autocomplete close];

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

- (void)windowWillEnterFullScreen:(NSNotification*)notification
{
    if (m_fullscreen_requested_for_web_content) {
        [self.toolbar setVisible:NO];
        [[self tab] updateBookmarksBarDisplay:NO];

        m_fullscreen_should_restore_tab_bar = [[self.window tabGroup] isTabBarVisible];
        if (m_fullscreen_should_restore_tab_bar) {
            [self.window toggleTabBar:nil];
        }
    }
}

- (void)windowDidEnterFullScreen:(NSNotification*)notification
{
    if (m_fullscreen_requested_for_web_content)
        [[[self tab] web_view] handleEnteredFullScreen];
}

- (void)windowWillExitFullScreen:(NSNotification*)notification
{
    if (exchange(m_fullscreen_exit_was_ui_initiated, true))
        [[[self tab] web_view] handleExitFullScreen];
}

- (void)windowDidExitFullScreen:(NSNotification*)notification
{
    if (exchange(m_fullscreen_requested_for_web_content, false)) {
        [self.toolbar setVisible:YES];
        [[self tab] updateBookmarksBarDisplay:WebView::Application::settings().show_bookmarks_bar()];

        if (m_fullscreen_should_restore_tab_bar && ![[self.window tabGroup] isTabBarVisible]) {
            [self.window toggleTabBar:nil];
        }
    }

    [[[self tab] web_view] handleExitedFullScreen];
}

- (NSApplicationPresentationOptions)window:(NSWindow*)window
      willUseFullScreenPresentationOptions:(NSApplicationPresentationOptions)proposed_options
{
    if (m_fullscreen_requested_for_web_content) {
        return NSApplicationPresentationAutoHideDock
            | NSApplicationPresentationAutoHideToolbar
            | NSApplicationPresentationAutoHideMenuBar
            | NSApplicationPresentationFullScreen;
    }

    return proposed_options;
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
    if ([identifier isEqual:TOOLBAR_BOOKMARK_IDENTIFIER]) {
        return self.bookmark_toolbar_item;
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
    if (should_suppress_inline_autocomplete_for_selector(selector))
        m_should_suppress_inline_autocomplete_on_next_change = true;

    if (selector == @selector(cancelOperation:)) {
        if ([self.autocomplete close])
            return YES;
        auto const& url = [[[self tab] web_view] view].url();
        self.suppressed_inline_autocomplete_query = nil;
        m_should_suppress_inline_autocomplete_on_next_change = false;
        [self setLocationFieldText:url.serialize()];
        [self.window makeFirstResponder:nil];
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
    m_autocomplete->cancel_pending_query();
    self.current_inline_autocomplete_suggestion = nil;
    self.suppressed_inline_autocomplete_query = nil;
    m_should_suppress_inline_autocomplete_on_next_change = false;
    [self.autocomplete close];
    [self setLocationFieldText:url_string];
}

- (void)controlTextDidChange:(NSNotification*)notification
{
    if (m_is_applying_inline_autocomplete)
        return;

    auto* query = [self currentLocationFieldQuery];
    if (m_should_suppress_inline_autocomplete_on_next_change) {
        self.suppressed_inline_autocomplete_query = query;
        m_should_suppress_inline_autocomplete_on_next_change = false;
    } else if (self.suppressed_inline_autocomplete_query != nil && ![self.suppressed_inline_autocomplete_query isEqualToString:query]) {
        self.suppressed_inline_autocomplete_query = nil;
    }

    if (self.suppressed_inline_autocomplete_query == nil && self.current_inline_autocomplete_suggestion != nil) {
        if (![self applyInlineAutocompleteSuggestionText:self.current_inline_autocomplete_suggestion forQuery:query])
            self.current_inline_autocomplete_suggestion = nil;
    }

    auto url_string = Ladybird::ns_string_to_string(query);
    m_autocomplete->query_autocomplete_engine(move(url_string), MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS);
}

#pragma mark - AutocompleteObserver

- (void)onHighlightedSuggestion:(String)suggestion
{
    [self previewHighlightedSuggestionInLocationField:suggestion];
}

- (void)onAutocompleteDidClose
{
    self.current_inline_autocomplete_suggestion = nil;
    [self restoreLocationFieldQuery];
}

- (void)onSelectedSuggestion:(String)suggestion
{
    [self navigateToLocation:move(suggestion)];
}

@end
