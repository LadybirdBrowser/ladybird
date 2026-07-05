/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/DownloadPresentation.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/URL.h>
#include <LibWebView/ViewImplementation.h>

#import <Application/Application.h>
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
static NSString* const TOOLBAR_DOWNLOADS_IDENTIFIER = @"ToolbarDownloadsIdentifier";
static NSString* const TOOLBAR_NEW_TAB_IDENTIFIER = @"ToolbarNewTabIdentifier";
static NSString* const TOOLBAR_TAB_OVERVIEW_IDENTIFIER = @"ToolbarTabOverviewIdentifier";

static constexpr CGFloat DOWNLOADS_POPOVER_WIDTH = 380;
static constexpr CGFloat DOWNLOADS_POPOVER_HORIZONTAL_PADDING = 12;
static constexpr CGFloat DOWNLOADS_POPOVER_VERTICAL_PADDING = 12;
static constexpr CGFloat DOWNLOADS_POPOVER_SPACING = 8;
static constexpr CGFloat DOWNLOADS_POPOVER_EMPTY_HEIGHT = 80;
static constexpr CGFloat DOWNLOADS_POPOVER_MAX_HEIGHT = 360;
static constexpr CGFloat DOWNLOADS_POPOVER_ROW_HEIGHT = 66;
static constexpr CGFloat DOWNLOADS_POPOVER_ROW_SPACING = 6;
static constexpr CGFloat DOWNLOADS_POPOVER_SHOW_ALL_BUTTON_HEIGHT = 28;

enum class LocationFieldDisplay {
    Editing,
    NotEditing,
};

class DownloadsObserver;

static NSString* candidate_by_trimming_root_trailing_slash(NSString* candidate);

static NSImage* downloads_toolbar_icon(bool active)
{
    auto* image = [NSImage imageWithSystemSymbolName:active ? @"arrow.down.circle.fill" : @"arrow.down.circle"
                            accessibilityDescription:@"Downloads"];
    [image setTemplate:YES];
    return image;
}

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

static NSImage* location_field_globe_icon()
{
    static NSImage* image;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        image = [NSImage imageWithSystemSymbolName:@"globe" accessibilityDescription:@"Page icon"];
        [image setSize:NSMakeSize(16, 16)];
    });
    return image;
}

@interface LocationSearchField : NSSearchField
{
    BOOL m_loading;
    BOOL m_shows_page_icon;
    NSImage* m_favicon;
    NSProgressIndicator* m_loading_indicator;
}

- (BOOL)becomeFirstResponder;
- (void)setLoading:(BOOL)loading;
- (void)setFavicon:(NSImage*)favicon;
- (void)setShowsPageIcon:(BOOL)showsPageIcon;

@property (nonatomic, copy) void (^willBeginEditing)(void);

@end

@implementation LocationSearchField

- (instancetype)init
{
    if (self = [super init]) {
        m_loading_indicator = [[NSProgressIndicator alloc] init];
        [m_loading_indicator setStyle:NSProgressIndicatorStyleSpinning];
        [m_loading_indicator setControlSize:NSControlSizeSmall];
        [m_loading_indicator setDisplayedWhenStopped:NO];
        [m_loading_indicator setHidden:YES];
        [self addSubview:m_loading_indicator];

        [self updateLeadingIcon];
    }

    return self;
}

- (BOOL)becomeFirstResponder
{
    BOOL result = [super becomeFirstResponder];
    if (result) {
        if (self.willBeginEditing)
            self.willBeginEditing();
        [self performSelector:@selector(selectText:) withObject:self afterDelay:0];
    }
    return result;
}

- (void)mouseDown:(NSEvent*)event
{
    [super mouseDown:event];

    if (self.willBeginEditing)
        self.willBeginEditing();
}

- (void)layout
{
    [super layout];

    auto* cell = (NSSearchFieldCell*)[self cell];
    auto search_button_rect = [cell searchButtonRectForBounds:[self bounds]];
    auto indicator_size = NSMakeSize(16, 16);
    [m_loading_indicator setFrame:NSMakeRect(
                                      NSMidX(search_button_rect) - indicator_size.width / 2,
                                      NSMidY(search_button_rect) - indicator_size.height / 2,
                                      indicator_size.width,
                                      indicator_size.height)];
}

- (void)setLoading:(BOOL)loading
{
    if (m_loading == loading)
        return;

    m_loading = loading;
    if (m_loading) {
        [m_loading_indicator setHidden:NO];
        [m_loading_indicator startAnimation:self];
    } else {
        [m_loading_indicator stopAnimation:self];
        [m_loading_indicator setHidden:YES];
    }

    [self updateLeadingIcon];
}

- (void)setFavicon:(NSImage*)favicon
{
    m_favicon = [favicon copy];
    [m_favicon setSize:NSMakeSize(16, 16)];
    [m_favicon setTemplate:NO];
    [self updateLeadingIcon];
}

- (void)setShowsPageIcon:(BOOL)showsPageIcon
{
    m_shows_page_icon = showsPageIcon;
    [self updateLeadingIcon];
}

- (void)updateLeadingIcon
{
    auto* cell = (NSSearchFieldCell*)[self cell];
    auto* search_button = [cell searchButtonCell];

    if (m_loading) {
        [search_button setImage:nil];
    } else if (m_shows_page_icon && m_favicon != nil) {
        [search_button setImage:m_favicon];
    } else if (!m_shows_page_icon) {
        [search_button setImage:[NSImage imageWithSystemSymbolName:@"magnifyingglass"
                                          accessibilityDescription:@"Search"]];
    } else {
        [search_button setImage:location_field_globe_icon()];
    }
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

@interface DownloadRowView : NSView

- (instancetype)initWithFrame:(NSRect)frame
                     download:(WebView::FileDownloader::Download const&)download;

@property (nonatomic, readonly) u64 downloadID;
@property (nonatomic, copy) void (^onCancelDownload)(u64);

@end

@implementation DownloadRowView
{
    u64 m_download_id;
    NSTextField* m_file_name_label;
    NSTextField* m_status_label;
    NSProgressIndicator* m_progress_indicator;
    NSButton* m_cancel_button;
}

- (instancetype)initWithFrame:(NSRect)frame
                     download:(WebView::FileDownloader::Download const&)download
{
    if (self = [super initWithFrame:frame]) {
        m_download_id = download.id;

        [self setWantsLayer:YES];
        [self layer].cornerRadius = 6;
        [self layer].borderWidth = 1;
        [self updateLayerColors];

        auto* row_stack = [[NSStackView alloc] init];
        [row_stack setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
        [row_stack setSpacing:8];
        [row_stack setAlignment:NSLayoutAttributeTop];
        [row_stack setDistribution:NSStackViewDistributionFill];
        [row_stack setDetachesHiddenViews:YES];
        [row_stack setEdgeInsets:NSEdgeInsets { 8, 10, 8, 10 }];
        [row_stack setTranslatesAutoresizingMaskIntoConstraints:NO];

        auto* details_stack = [[NSStackView alloc] init];
        [details_stack setOrientation:NSUserInterfaceLayoutOrientationVertical];
        [details_stack setSpacing:4];
        [details_stack setAlignment:NSLayoutAttributeLeading];
        [details_stack setDistribution:NSStackViewDistributionFill];
        [details_stack setDetachesHiddenViews:YES];
        [details_stack setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                                forOrientation:NSLayoutConstraintOrientationHorizontal];

        m_file_name_label = [NSTextField labelWithString:@""];
        [m_file_name_label setFont:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]];
        [m_file_name_label setMaximumNumberOfLines:1];
        [[m_file_name_label cell] setLineBreakMode:NSLineBreakByTruncatingTail];
        [m_file_name_label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                                    forOrientation:NSLayoutConstraintOrientationHorizontal];

        m_status_label = [NSTextField labelWithString:@""];

        // Use monospaced digits so the status text does not jitter as the downloaded size ticks up.
        [m_status_label setFont:[NSFont monospacedDigitSystemFontOfSize:[NSFont smallSystemFontSize] weight:NSFontWeightRegular]];
        [m_status_label setTextColor:[NSColor secondaryLabelColor]];
        [m_status_label setMaximumNumberOfLines:1];
        [[m_status_label cell] setLineBreakMode:NSLineBreakByTruncatingTail];
        [m_status_label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                                 forOrientation:NSLayoutConstraintOrientationHorizontal];

        m_progress_indicator = [[NSProgressIndicator alloc] init];
        [m_progress_indicator setStyle:NSProgressIndicatorStyleBar];
        [m_progress_indicator setIndeterminate:NO];
        [m_progress_indicator setMinValue:0.0];
        [m_progress_indicator setMaxValue:1.0];
        [m_progress_indicator setControlSize:NSControlSizeSmall];
        [m_progress_indicator setDisplayedWhenStopped:YES];
        [m_progress_indicator setHidden:YES];
        [m_progress_indicator setContentHuggingPriority:NSLayoutPriorityDefaultLow
                                         forOrientation:NSLayoutConstraintOrientationHorizontal];
        [[m_progress_indicator heightAnchor] constraintEqualToConstant:4].active = YES;

        [details_stack addArrangedSubview:m_file_name_label];
        [details_stack addArrangedSubview:m_status_label];
        [details_stack addArrangedSubview:m_progress_indicator];

        m_cancel_button = [NSButton buttonWithTitle:@"Cancel"
                                             target:self
                                             action:@selector(cancelDownload:)];
        [m_cancel_button setBezelStyle:NSBezelStyleRounded];
        [m_cancel_button setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                                  forOrientation:NSLayoutConstraintOrientationHorizontal];

        [row_stack addArrangedSubview:details_stack];
        [row_stack addArrangedSubview:m_cancel_button];

        [self addSubview:row_stack];
        [NSLayoutConstraint activateConstraints:@[
            [[row_stack leadingAnchor] constraintEqualToAnchor:[self leadingAnchor]],
            [[row_stack trailingAnchor] constraintEqualToAnchor:[self trailingAnchor]],
            [[row_stack topAnchor] constraintEqualToAnchor:[self topAnchor]],
            [[row_stack bottomAnchor] constraintEqualToAnchor:[self bottomAnchor]],
            [[m_progress_indicator widthAnchor] constraintEqualToAnchor:[details_stack widthAnchor]],
        ]];

        [self updateWithDownload:download];
    }

    return self;
}

- (u64)downloadID
{
    return m_download_id;
}

- (void)viewDidChangeEffectiveAppearance
{
    [super viewDidChangeEffectiveAppearance];
    [self updateLayerColors];
}

- (void)updateLayerColors
{
    [self layer].backgroundColor = [NSColor controlBackgroundColor].CGColor;
    [self layer].borderColor = [NSColor separatorColor].CGColor;
}

- (void)updateWithDownload:(WebView::FileDownloader::Download const&)download
{
    VERIFY(download.id == m_download_id);

    auto* file_name = Ladybird::string_to_ns_string(download.destination.basename());
    auto* path = Ladybird::string_to_ns_string(download.destination.string());
    [m_file_name_label setStringValue:file_name];
    [m_file_name_label setToolTip:path];

    auto* status_text = Ladybird::string_to_ns_string(WebView::download_status_text(download));
    [m_status_label setStringValue:status_text];
    [m_status_label setToolTip:status_text];

    auto progress = download.progress();
    auto show_progress = download.status == WebView::FileDownloader::DownloadStatus::InProgress && progress.has_value();
    [m_progress_indicator setHidden:!show_progress];
    if (progress.has_value())
        [m_progress_indicator setDoubleValue:*progress];

    [m_cancel_button setHidden:download.status != WebView::FileDownloader::DownloadStatus::InProgress];
}

- (void)cancelDownload:(id)sender
{
    if (self.onCancelDownload)
        self.onCancelDownload(m_download_id);
}

@end

@interface DownloadsRowsView : NSView
@end

@implementation DownloadsRowsView

- (BOOL)isFlipped
{
    return YES;
}

@end

@interface DownloadsPopoverViewController : NSViewController

- (BOOL)setDownloads:(ReadonlySpan<WebView::FileDownloader::Download>)downloads;

@property (nonatomic, copy) void (^onCancelDownload)(u64);
@property (nonatomic, copy) void (^onOpenAllDownloads)(void);

@end

@implementation DownloadsPopoverViewController
{
    NSTextField* m_title_label;
    NSTextField* m_empty_label;
    NSScrollView* m_scroll_view;
    DownloadsRowsView* m_rows_view;
    Vector<DownloadRowView*> m_download_rows;
    NSLayoutConstraint* m_scroll_view_height_constraint;
}

- (void)loadView
{
    auto* stack = [[NSStackView alloc] initWithFrame:NSMakeRect(0, 0, DOWNLOADS_POPOVER_WIDTH, DOWNLOADS_POPOVER_EMPTY_HEIGHT)];
    [stack setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [stack setSpacing:DOWNLOADS_POPOVER_SPACING];
    [stack setEdgeInsets:NSEdgeInsets {
                             DOWNLOADS_POPOVER_VERTICAL_PADDING,
                             DOWNLOADS_POPOVER_HORIZONTAL_PADDING,
                             DOWNLOADS_POPOVER_VERTICAL_PADDING,
                             DOWNLOADS_POPOVER_HORIZONTAL_PADDING,
                         }];
    [stack setDetachesHiddenViews:YES];
    [[stack widthAnchor] constraintEqualToConstant:DOWNLOADS_POPOVER_WIDTH].active = YES;

    m_title_label = [NSTextField labelWithString:@"Downloads"];
    [m_title_label setFont:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]];
    [stack addArrangedSubview:m_title_label];

    m_empty_label = [NSTextField labelWithString:@"No downloads"];
    [m_empty_label setTextColor:[NSColor secondaryLabelColor]];
    [m_empty_label setAlignment:NSTextAlignmentCenter];
    [[m_empty_label heightAnchor] constraintEqualToConstant:DOWNLOADS_POPOVER_EMPTY_HEIGHT].active = YES;
    [stack addArrangedSubview:m_empty_label];

    m_scroll_view = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, DOWNLOADS_POPOVER_WIDTH, DOWNLOADS_POPOVER_EMPTY_HEIGHT)];
    [m_scroll_view setHasVerticalScroller:YES];
    [m_scroll_view setDrawsBackground:NO];
    [m_scroll_view setBorderType:NSNoBorder];
    [m_scroll_view setHidden:YES];
    m_rows_view = [[DownloadsRowsView alloc] initWithFrame:NSZeroRect];
    [m_scroll_view setDocumentView:m_rows_view];
    m_scroll_view_height_constraint = [[m_scroll_view heightAnchor] constraintEqualToConstant:DOWNLOADS_POPOVER_EMPTY_HEIGHT];
    m_scroll_view_height_constraint.active = YES;
    [stack addArrangedSubview:m_scroll_view];

    auto* show_all_button = [NSButton buttonWithTitle:@"Show All Downloads"
                                               target:self
                                               action:@selector(openAllDownloads:)];
    [show_all_button setBezelStyle:NSBezelStyleRounded];
    [[show_all_button heightAnchor] constraintEqualToConstant:DOWNLOADS_POPOVER_SHOW_ALL_BUTTON_HEIGHT].active = YES;
    [stack addArrangedSubview:show_all_button];

    [self setView:stack];
}

- (BOOL)setDownloads:(ReadonlySpan<WebView::FileDownloader::Download>)downloads
{
    (void)[self view];
    bool geometry_changed = false;

    auto visible_downloads = WebView::recent_downloads_for_popover(downloads);

    auto is_empty = visible_downloads.is_empty();
    if ([m_empty_label isHidden] == is_empty) {
        [m_empty_label setHidden:!is_empty];
        geometry_changed = true;
    }
    if ([m_scroll_view isHidden] != is_empty) {
        [m_scroll_view setHidden:is_empty];
        geometry_changed = true;
    }

    auto row_width = DOWNLOADS_POPOVER_WIDTH - (DOWNLOADS_POPOVER_HORIZONTAL_PADDING * 2);
    CGFloat rows_height = 0;
    if (!is_empty)
        rows_height = (DOWNLOADS_POPOVER_ROW_HEIGHT * visible_downloads.size()) + (DOWNLOADS_POPOVER_ROW_SPACING * (visible_downloads.size() - 1));

    bool rows_match = m_download_rows.size() == visible_downloads.size();
    if (rows_match) {
        for (size_t i = 0; i < visible_downloads.size(); ++i) {
            if ([m_download_rows[i] downloadID] != visible_downloads[i]->id) {
                rows_match = false;
                break;
            }
        }
    }

    if (!rows_match) {
        for (auto* row : m_download_rows)
            [row removeFromSuperview];
        m_download_rows.clear();
        m_download_rows.ensure_capacity(visible_downloads.size());
        geometry_changed = true;

        CGFloat y = 0;
        for (auto* download : visible_downloads) {
            auto* row = [[DownloadRowView alloc] initWithFrame:NSMakeRect(0, y, row_width, DOWNLOADS_POPOVER_ROW_HEIGHT)
                                                      download:*download];
            __weak DownloadsPopoverViewController* weak_self = self;
            [row setOnCancelDownload:^(u64 id) {
                DownloadsPopoverViewController* self = weak_self;
                if (self != nil && self.onCancelDownload)
                    self.onCancelDownload(id);
            }];
            [m_rows_view addSubview:row];
            m_download_rows.append(row);

            y += DOWNLOADS_POPOVER_ROW_HEIGHT + DOWNLOADS_POPOVER_ROW_SPACING;
        }
    } else {
        for (size_t i = 0; i < visible_downloads.size(); ++i)
            [m_download_rows[i] updateWithDownload:*visible_downloads[i]];
    }

    auto rows_frame = [m_rows_view frame];
    if (rows_frame.size.width != row_width || rows_frame.size.height != rows_height)
        [m_rows_view setFrameSize:NSMakeSize(row_width, rows_height)];

    CGFloat body_height = DOWNLOADS_POPOVER_EMPTY_HEIGHT;
    if (!is_empty) {
        auto max_scroll_height = DOWNLOADS_POPOVER_MAX_HEIGHT
            - (DOWNLOADS_POPOVER_VERTICAL_PADDING * 2)
            - (DOWNLOADS_POPOVER_SPACING * 2)
            - DOWNLOADS_POPOVER_SHOW_ALL_BUTTON_HEIGHT
            - [m_title_label fittingSize].height;
        body_height = rows_height < max_scroll_height ? rows_height : max_scroll_height;
    }

    auto should_show_vertical_scroller = rows_height > body_height;
    if ([m_scroll_view hasVerticalScroller] != should_show_vertical_scroller) {
        [m_scroll_view setHasVerticalScroller:should_show_vertical_scroller];
        geometry_changed = true;
    }

    if (m_scroll_view_height_constraint.constant != body_height) {
        m_scroll_view_height_constraint.constant = body_height;
        geometry_changed = true;
    }

    if (geometry_changed)
        [self updatePreferredContentSize];
    return geometry_changed;
}

- (void)updatePreferredContentSize
{
    [[self view] layoutSubtreeIfNeeded];

    auto fitting_size = [[self view] fittingSize];
    fitting_size.width = DOWNLOADS_POPOVER_WIDTH;
    [self setPreferredContentSize:fitting_size];
}

- (void)openAllDownloads:(id)sender
{
    if (self.onOpenAllDownloads)
        self.onOpenAllDownloads();
}

@end

@interface DownloadsButton : NSButton

- (BOOL)isReadyToAnchorPopover;

@property (nonatomic, copy) void (^onReadyToAnchorPopover)(void);

@end

@implementation DownloadsButton

- (BOOL)isReadyToAnchorPopover
{
    return [self window] != nil && !NSIsEmptyRect([self frame]);
}

- (void)notifyIfReadyToAnchorPopover
{
    if ([self isReadyToAnchorPopover] && self.onReadyToAnchorPopover)
        self.onReadyToAnchorPopover();
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    [self notifyIfReadyToAnchorPopover];
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self notifyIfReadyToAnchorPopover];
}

@end

@interface TabController () <NSToolbarDelegate, NSSearchFieldDelegate, AutocompleteObserver>
{
    u64 m_page_index;

    OwnPtr<WebView::Autocomplete> m_autocomplete;
    OwnPtr<DownloadsObserver> m_downloads_observer;
    bool m_show_downloads_popover_when_button_is_ready;
    bool m_is_applying_inline_autocomplete;
    bool m_should_suppress_inline_autocomplete_on_next_change;

    bool m_fullscreen_requested_for_web_content;
    bool m_fullscreen_exit_was_ui_initiated;
    bool m_fullscreen_should_restore_tab_bar;
    Function<void()> m_pending_immediate_close;
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
@property (nonatomic, strong) NSToolbarItem* downloads_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* new_tab_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* tab_overview_toolbar_item;
@property (nonatomic, strong) DownloadsButton* downloads_button;
@property (nonatomic, strong) NSPopover* downloads_popover;

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
- (void)downloadAdded:(WebView::FileDownloader::Download const&)download;
- (void)downloadUpdated:(WebView::FileDownloader::Download const&)download;
- (void)downloadRemoved:(u64)download_id;
- (void)downloadsButtonReadyToAnchorPopover;

@end

class DownloadsObserver final : public WebView::FileDownloaderObserver {
public:
    explicit DownloadsObserver(TabController* controller)
        : m_controller(controller)
    {
    }

private:
    virtual void download_added(WebView::FileDownloader::Download const& download) override
    {
        [m_controller downloadAdded:download];
    }

    virtual void download_updated(WebView::FileDownloader::Download const& download) override
    {
        [m_controller downloadUpdated:download];
    }

    virtual void download_removed(u64 download_id) override
    {
        [m_controller downloadRemoved:download_id];
    }

    __weak TabController* m_controller { nil };
};

@implementation TabController

@synthesize toolbar_identifiers = _toolbar_identifiers;
@synthesize navigate_back_toolbar_item = _navigate_back_toolbar_item;
@synthesize navigate_forward_toolbar_item = _navigate_forward_toolbar_item;
@synthesize reload_toolbar_item = _reload_toolbar_item;
@synthesize location_toolbar_item = _location_toolbar_item;
@synthesize zoom_toolbar_item = _zoom_toolbar_item;
@synthesize bookmark_toolbar_item = _bookmark_toolbar_item;
@synthesize downloads_toolbar_item = _downloads_toolbar_item;
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
        m_autocomplete = make<WebView::Autocomplete>(WebView::IsPrivate::No);
        m_downloads_observer = make<DownloadsObserver>(self);

        m_autocomplete->on_autocomplete_query_complete = [weak_self](auto suggestions, WebView::AutocompleteResultKind result_kind) {
            TabController* self = weak_self;
            if (self == nil) {
                return;
            }

            auto selected_row = [self applyInlineAutocomplete:suggestions];

            // Do not update the popup while results are still changing.
            // Intermediate updates are triggered on every keystroke and would
            // cause visible flicker in the suggestion list.
            // Only final results are used to refresh the UI.
            if (result_kind == WebView::AutocompleteResultKind::Intermediate && [self.autocomplete isVisible])
                return;

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
    [(LocationSearchField*)[self.location_toolbar_item view] setFavicon:nil];
    [(LocationSearchField*)[self.location_toolbar_item view] setLoading:YES];
}

- (void)onLoadFinish:(URL::URL const&)url
{
    (void)url;
    [(LocationSearchField*)[self.location_toolbar_item view] setLoading:NO];
}

- (void)onFaviconChange:(NSImage*)favicon
{
    [(LocationSearchField*)[self.location_toolbar_item view] setFavicon:favicon];
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
    [self restoreLocationFieldForEditing];
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

- (void)setLocationFieldText:(StringView)url display:(LocationFieldDisplay)display
{
    NSMutableAttributedString* attributed_url;
    auto maybe_url = URL::create_with_url_or_path(url);
    auto display_url = MUST(String::from_utf8(url));
    if (display == LocationFieldDisplay::NotEditing && maybe_url.has_value())
        display_url = WebView::url_for_display(*maybe_url);

    auto url_parts = WebView::break_url_into_parts(url);

    auto* dark_attributes = @{
        NSForegroundColorAttributeName : [NSColor systemGrayColor],
    };
    auto* highlight_attributes = @{
        NSForegroundColorAttributeName : [NSColor textColor],
    };

    if (url_parts.has_value()) {
        attributed_url = [[NSMutableAttributedString alloc] init];

        auto scheme_and_subdomain = url_parts->scheme_and_subdomain;
        auto remainder = url_parts->remainder;
        if (display == LocationFieldDisplay::NotEditing && maybe_url.has_value() && maybe_url->scheme().is_one_of("http"sv, "https"sv)) {
            auto scheme_prefix_length = maybe_url->scheme().bytes_as_string_view().length() + "://"sv.length();
            scheme_and_subdomain = scheme_and_subdomain.substring_view(scheme_prefix_length);
            if (scheme_and_subdomain.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
                scheme_and_subdomain = scheme_and_subdomain.substring_view(4);
            if (remainder == "/"sv)
                remainder = {};
        }

        auto* attributed_scheme_and_subdomain = [[NSAttributedString alloc]
            initWithString:Ladybird::string_to_ns_string(scheme_and_subdomain)
                attributes:dark_attributes];

        auto* attributed_effective_tld_plus_one = [[NSAttributedString alloc]
            initWithString:Ladybird::string_to_ns_string(url_parts->effective_tld_plus_one)
                attributes:highlight_attributes];

        auto* attributed_remainder = [[NSAttributedString alloc]
            initWithString:Ladybird::string_to_ns_string(remainder)
                attributes:dark_attributes];

        [attributed_url appendAttributedString:attributed_scheme_and_subdomain];
        [attributed_url appendAttributedString:attributed_effective_tld_plus_one];
        [attributed_url appendAttributedString:attributed_remainder];
    } else {
        attributed_url = [[NSMutableAttributedString alloc]
            initWithString:Ladybird::string_to_ns_string(display_url)
                attributes:highlight_attributes];
    }

    if (display == LocationFieldDisplay::NotEditing && maybe_url.has_value() && ![[attributed_url string] isEqualToString:Ladybird::string_to_ns_string(display_url)]) {
        attributed_url = [[NSMutableAttributedString alloc]
            initWithString:Ladybird::string_to_ns_string(display_url)
                attributes:highlight_attributes];
    }

    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];
    [location_search_field setAttributedStringValue:attributed_url];
    [location_search_field setShowsPageIcon:url_parts.has_value()];
}

- (void)setLocationFieldText:(StringView)url
{
    [self setLocationFieldText:url display:LocationFieldDisplay::NotEditing];
}

- (void)restoreLocationFieldForEditing
{
    auto const& url = [[[self tab] web_view] view].url();
    auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];
    if (![[location_search_field stringValue] isEqualToString:Ladybird::string_to_ns_string(WebView::url_for_display(url))])
        return;

    m_is_applying_inline_autocomplete = true;
    [self setLocationFieldText:url.serialize() display:LocationFieldDisplay::Editing];

    auto* editor = (NSTextView*)[location_search_field currentEditor];
    if (editor != nil && [self.window firstResponder] == editor && ![editor hasMarkedText]) {
        auto* serialized_url = Ladybird::string_to_ns_string(url.serialize());
        [editor setString:serialized_url];
        [editor setSelectedRange:NSMakeRange(0, serialized_url.length)];
    }
    m_is_applying_inline_autocomplete = false;
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
    } else {
        [[[self tab] web_view] view].load_navigation_error_page(location);
    }

    self.current_inline_autocomplete_suggestion = nil;
    self.suppressed_inline_autocomplete_query = nil;
    m_should_suppress_inline_autocomplete_on_next_change = false;
    [self focusWebView];
    [self.autocomplete close];

    return YES;
}

- (void)showTabOverview:(id)sender
{
    self.tab.titlebarAppearsTransparent = NO;
    [self.window toggleTabOverview:sender];
    self.tab.titlebarAppearsTransparent = YES;
}

- (void)showDownloadsPopover:(id)sender
{
    if (!self.downloads_button)
        return;

    // Unhiding the downloads toolbar item does not add the button's view to the window and give it a frame
    // until a later layout pass, and the popover anchors to the centre of the window if it is shown before
    // then. Wait for the button to become ready instead.
    if (![self.downloads_button isReadyToAnchorPopover]) {
        m_show_downloads_popover_when_button_is_ready = true;
        return;
    }

    if (!self.downloads_popover) {
        auto* content_view_controller = [[DownloadsPopoverViewController alloc] init];
        __weak TabController* weak_self = self;
        [content_view_controller setOnCancelDownload:^(u64 id) {
            TabController* self = weak_self;
            if (self == nil)
                return;

            auto& file_downloader = WebView::Application::the().file_downloader();
            if (auto download = file_downloader.download(id); download.has_value() && download->status == WebView::FileDownloader::DownloadStatus::InProgress)
                file_downloader.cancel_download(id);
            [self updateDownloadsPopover];
        }];
        [content_view_controller setOnOpenAllDownloads:^{
            TabController* self = weak_self;
            if (self == nil)
                return;

            [self.downloads_popover close];
            WebView::Application::the().open_downloads_page_action().activate();
        }];

        self.downloads_popover = [[NSPopover alloc] init];
        [self.downloads_popover setAnimates:YES];
        [self.downloads_popover setBehavior:NSPopoverBehaviorTransient];
        [self.downloads_popover setContentViewController:content_view_controller];
    }

    [self updateDownloadsPopover];
    [self.downloads_popover showRelativeToToolbarItem:self.downloads_toolbar_item];
}

- (void)downloadsButtonReadyToAnchorPopover
{
    if (!m_show_downloads_popover_when_button_is_ready)
        return;
    m_show_downloads_popover_when_button_is_ready = false;

    [self showDownloadsPopover:self.downloads_button];
}

- (void)toggleDownloadsPopover:(id)sender
{
    if (self.downloads_popover && [self.downloads_popover isShown]) {
        [self.downloads_popover close];
        return;
    }

    [self showDownloadsPopover:sender];
}

- (void)updateDownloadsButton
{
    if (!self.downloads_button)
        return;

    auto downloads = WebView::Application::the().file_downloader().downloads();
    auto button_state = WebView::downloads_button_state(downloads);

    auto should_hide_button = !button_state.has_downloads;
    if (@available(macOS 15, *)) {
        [self.downloads_toolbar_item setHidden:should_hide_button];
    } else {
        [self.downloads_button setHidden:should_hide_button];
    }

    [self.downloads_button setImage:downloads_toolbar_icon(button_state.active_download_count > 0)];

    auto* tooltip = Ladybird::string_to_ns_string(button_state.tooltip);
    [self.downloads_button setToolTip:tooltip];
    [self.downloads_toolbar_item setToolTip:tooltip];

    if (!button_state.has_downloads) {
        m_show_downloads_popover_when_button_is_ready = false;

        if (self.downloads_popover && [self.downloads_popover isShown])
            [self.downloads_popover close];
    }
}

- (void)updateDownloadsPopover
{
    if (!self.downloads_popover)
        return;

    auto* content_view_controller = (DownloadsPopoverViewController*)[self.downloads_popover contentViewController];
    if ([content_view_controller setDownloads:WebView::Application::the().file_downloader().downloads()])
        [self.downloads_popover setContentSize:[content_view_controller preferredContentSize]];
}

- (void)downloadAdded:(WebView::FileDownloader::Download const&)download
{
    (void)download;

    [self updateDownloadsButton];

    if ([self.window isKeyWindow] && self.downloads_button) {
        [self showDownloadsPopover:self.downloads_button];
        return;
    }

    [self updateDownloadsPopover];
}

- (void)downloadUpdated:(WebView::FileDownloader::Download const&)download
{
    (void)download;

    [self updateDownloadsButton];
    [self updateDownloadsPopover];
}

- (void)downloadRemoved:(u64)download_id
{
    (void)download_id;

    [self updateDownloadsButton];
    [self updateDownloadsPopover];
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
        __weak TabController* weak_self = self;
        [location_search_field setWillBeginEditing:^{
            [weak_self restoreLocationFieldForEditing];
        }];

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

- (NSToolbarItem*)downloads_toolbar_item
{
    if (!_downloads_toolbar_item) {
        auto* button = (DownloadsButton*)Ladybird::create_application_button(WebView::Application::the().open_downloads_page_action(), [DownloadsButton class]);
        [button setImage:downloads_toolbar_icon(false)];
        [button setTarget:self];
        [button setAction:@selector(toggleDownloadsPopover:)];
        [button setToolTip:@"Downloads"];

        __weak TabController* weak_self = self;
        [button setOnReadyToAnchorPopover:^{
            TabController* self = weak_self;
            if (self != nil)
                [self downloadsButtonReadyToAnchorPopover];
        }];

        self.downloads_button = button;

        _downloads_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_DOWNLOADS_IDENTIFIER];
        [_downloads_toolbar_item setView:button];
        [_downloads_toolbar_item setLabel:@"Downloads"];
        [_downloads_toolbar_item setPaletteLabel:@"Downloads"];
        [_downloads_toolbar_item setToolTip:@"Downloads"];
        [_downloads_toolbar_item setMenuFormRepresentation:Ladybird::create_application_menu_item(WebView::Application::the().open_downloads_page_action())];

        [self updateDownloadsButton];
    }

    return _downloads_toolbar_item;
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
            TOOLBAR_DOWNLOADS_IDENTIFIER,
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
    [self updateDownloadsButton];

    auto* delegate = (ApplicationDelegate*)[NSApp delegate];
    [delegate setActiveTab:[self tab]];
}

#pragma mark - NSWindowDelegate

- (void)windowDidBecomeMain:(NSNotification*)notification
{
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];
    [delegate setActiveTab:[self tab]];
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification
{
    [[[self tab] web_view] handleVisibility:([self.window occlusionState] & NSWindowOcclusionStateVisible) != 0];
}

- (void)windowDidResignKey:(NSNotification*)notification
{
    [self.autocomplete close];
}

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];
    auto confirm_canceling_downloads = [&]() {
        if ([delegate tabCount] > 1)
            return true;
        return [(Application*)NSApp confirmCancelActiveDownloads];
    };

    if (![[[self tab] web_view] needsBeforeUnloadCheck]) {
        if (!confirm_canceling_downloads())
            return false;

        m_pending_immediate_close = [[[self tab] web_view] prepareForImmediateClose];
        return true;
    }

    // Prevent closing on first request so WebContent can cleanly shutdown (e.g. asking if the user is sure they want
    // to leave, closing WebSocket connections, etc.)
    if (!self.already_requested_close) {
        self.already_requested_close = true;
        [[[self tab] web_view] requestClose];
        return false;
    }

    // If the user has already requested a close, then respect the user's request and just close the tab.
    // For example, the WebContent process may not be responding.
    if (!confirm_canceling_downloads())
        return false;

    return true;
}

- (void)windowWillClose:(NSNotification*)notification
{
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];
    [delegate removeTab:self];

    auto request_close = AK::move(m_pending_immediate_close);
    if (request_close)
        Core::deferred_invoke(AK::move(request_close));
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
    if ([identifier isEqual:TOOLBAR_DOWNLOADS_IDENTIFIER]) {
        return self.downloads_toolbar_item;
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

- (void)controlTextDidBeginEditing:(NSNotification*)notification
{
    [self restoreLocationFieldForEditing];
}

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
    NSString* url_string = [[location_search_field stringValue] copy];

    // AppKit can send this while focus is still settling into the field
    // editor. Wait until the next turn so transient notifications do not
    // format the live editor contents as a non-editing URL.
    dispatch_async(dispatch_get_main_queue(), ^{
        auto* location_search_field = (LocationSearchField*)[self.location_toolbar_item view];
        auto* editor = (NSTextView*)[location_search_field currentEditor];
        if (editor != nil && [self.window firstResponder] == editor)
            return;

        m_autocomplete->cancel_pending_query();
        self.current_inline_autocomplete_suggestion = nil;
        self.suppressed_inline_autocomplete_query = nil;
        m_should_suppress_inline_autocomplete_on_next_change = false;
        [self.autocomplete close];
        [self setLocationFieldText:Ladybird::ns_string_to_string(url_string)];
    });
}

- (void)controlTextDidChange:(NSNotification*)notification
{
    if (m_is_applying_inline_autocomplete)
        return;

    [(LocationSearchField*)[self.location_toolbar_item view] setShowsPageIcon:NO];

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
