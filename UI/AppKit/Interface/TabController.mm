/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/DownloadPresentation.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/Omnibox.h>
#include <LibWebView/URL.h>
#include <LibWebView/ViewImplementation.h>

#import <Application/Application.h>
#import <Application/ApplicationDelegate.h>
#import <Interface/Autocomplete.h>
#import <Interface/LadybirdWebView.h>
#import <Interface/LocationSearchField.h>
#import <Interface/Menu.h>
#import <Interface/Palette.h>
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
static NSString* const TOOLBAR_PRIVATE_BROWSING_IDENTIFIER = @"ToolbarPrivateBrowsingIdentifier";
static NSString* const TOOLBAR_DOWNLOADS_IDENTIFIER = @"ToolbarDownloadsIdentifier";
static NSString* const TOOLBAR_NEW_TAB_IDENTIFIER = @"ToolbarNewTabIdentifier";
static NSString* const TOOLBAR_TAB_OVERVIEW_IDENTIFIER = @"ToolbarTabOverviewIdentifier";

static constexpr CGFloat PRIVATE_BROWSING_BADGE_HEIGHT = 22;
static constexpr CGFloat PRIVATE_BROWSING_BADGE_HORIZONTAL_PADDING = 10;
static constexpr CGFloat PRIVATE_BROWSING_BADGE_CORNER_RADIUS = 10;

static constexpr CGFloat PRIVATE_SESSION_POPOVER_WIDTH = 320;
static constexpr CGFloat PRIVATE_SESSION_POPOVER_HORIZONTAL_PADDING = 16;
static constexpr CGFloat PRIVATE_SESSION_POPOVER_VERTICAL_PADDING = 14;
static constexpr CGFloat PRIVATE_SESSION_POPOVER_SPACING = 10;
static constexpr CGFloat PRIVATE_SESSION_POPOVER_BUTTON_SPACING = 8;
static constexpr CGFloat PRIVATE_SESSION_POPOVER_CONTENT_WIDTH = PRIVATE_SESSION_POPOVER_WIDTH - (PRIVATE_SESSION_POPOVER_HORIZONTAL_PADDING * 2);

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

static NSImage* downloads_toolbar_icon(bool active)
{
    auto* image = [NSImage imageWithSystemSymbolName:active ? @"arrow.down.circle.fill" : @"arrow.down.circle"
                            accessibilityDescription:@"Downloads"];
    [image setTemplate:YES];
    return image;
}

static bool selector_deletes_text(SEL selector)
{
    return selector == @selector(deleteBackward:)
        || selector == @selector(deleteBackwardByDecomposingPreviousCharacter:)
        || selector == @selector(deleteForward:)
        || selector == @selector(deleteToBeginningOfLine:)
        || selector == @selector(deleteToEndOfLine:)
        || selector == @selector(deleteWordBackward:)
        || selector == @selector(deleteWordForward:);
}

static NSUInteger ns_string_index_for_byte_offset(String const& text, size_t byte_offset)
{
    auto prefix = text.bytes_as_string_view().substring_view(0, byte_offset);
    return Ladybird::string_to_ns_string(prefix).length;
}

static NSInteger ns_index_for_selected_suggestion(Optional<size_t> selected_suggestion)
{
    if (!selected_suggestion.has_value())
        return NSNotFound;
    return static_cast<NSInteger>(*selected_suggestion);
}

@interface PrivateBrowsingBadgeButton : NSButton
@end

@implementation PrivateBrowsingBadgeButton

- (instancetype)init
{
    if (self = [super init]) {
        [self setTitle:@"Private"];
        [self setBordered:NO];
        [self setButtonType:NSButtonTypeMomentaryChange];
        [self setFocusRingType:NSFocusRingTypeNone];
        [self setAlignment:NSTextAlignmentCenter];
        [self setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize] weight:NSFontWeightSemibold]];
        [self setToolTip:@"Close all private windows and start a fresh private browsing session"];
        [self updateTitleAttributes];

        [self setWantsLayer:YES];
        [self layer].cornerRadius = PRIVATE_BROWSING_BADGE_CORNER_RADIUS;
        [self layer].borderWidth = 1;
        [self updateLayerColors];

        [[self heightAnchor] constraintEqualToConstant:PRIVATE_BROWSING_BADGE_HEIGHT].active = YES;

        [self setContentHuggingPriority:NSLayoutPriorityRequired
                         forOrientation:NSLayoutConstraintOrientationHorizontal];
        [self setContentHuggingPriority:NSLayoutPriorityRequired
                         forOrientation:NSLayoutConstraintOrientationVertical];
    }

    return self;
}

- (NSSize)intrinsicContentSize
{
    auto* attributes = @{
        NSFontAttributeName : [self font],
    };
    auto label_size = [[self title] sizeWithAttributes:attributes];
    return NSMakeSize(label_size.width + (PRIVATE_BROWSING_BADGE_HORIZONTAL_PADDING * 2), PRIVATE_BROWSING_BADGE_HEIGHT);
}

- (void)viewDidChangeEffectiveAppearance
{
    [super viewDidChangeEffectiveAppearance];
    [self updateTitleAttributes];
    [self updateLayerColors];
}

- (void)updateTitleAttributes
{
    auto* attributes = @{
        NSFontAttributeName : [self font],
        NSForegroundColorAttributeName : [NSColor labelColor],
    };
    [self setAttributedTitle:[[NSAttributedString alloc] initWithString:@"Private"
                                                             attributes:attributes]];
}

- (void)updateLayerColors
{
    auto is_dark = Ladybird::is_using_dark_system_theme();

    auto* surface_color = is_dark
        ? Ladybird::gfx_color_to_ns_color({ 0x19, 0x0c, 0x4a })
        : Ladybird::gfx_color_to_ns_color({ 0xe0, 0xd4, 0xff });
    auto* border_color = is_dark
        ? Ladybird::gfx_color_to_ns_color({ 0x9c, 0x90, 0xc8 })
        : Ladybird::gfx_color_to_ns_color({ 0x6c, 0x5f, 0x93 });

    [self layer].backgroundColor = surface_color.CGColor;
    [self layer].borderColor = border_color.CGColor;
}

@end

@interface PrivateSessionPopoverViewController : NSViewController

@property (nonatomic, copy) void (^onCancel)(void);
@property (nonatomic, copy) void (^onConfirm)(void);

@end

@implementation PrivateSessionPopoverViewController

- (void)loadView
{
    auto* stack = [[NSStackView alloc] initWithFrame:NSMakeRect(0, 0, PRIVATE_SESSION_POPOVER_WIDTH, 0)];
    [stack setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [stack setSpacing:PRIVATE_SESSION_POPOVER_SPACING];
    [stack setEdgeInsets:NSEdgeInsets {
                             PRIVATE_SESSION_POPOVER_VERTICAL_PADDING,
                             PRIVATE_SESSION_POPOVER_HORIZONTAL_PADDING,
                             PRIVATE_SESSION_POPOVER_VERTICAL_PADDING,
                             PRIVATE_SESSION_POPOVER_HORIZONTAL_PADDING,
                         }];
    [[stack widthAnchor] constraintEqualToConstant:PRIVATE_SESSION_POPOVER_WIDTH].active = YES;

    auto* title_label = [NSTextField labelWithString:@"Start a fresh private session?"];
    [title_label setFont:[NSFont boldSystemFontOfSize:[NSFont systemFontSize]]];
    [title_label setLineBreakMode:NSLineBreakByWordWrapping];
    [title_label setMaximumNumberOfLines:0];
    [title_label setPreferredMaxLayoutWidth:PRIVATE_SESSION_POPOVER_CONTENT_WIDTH];
    [[title_label widthAnchor] constraintEqualToConstant:PRIVATE_SESSION_POPOVER_CONTENT_WIDTH].active = YES;
    [stack addArrangedSubview:title_label];

    auto* body_label = [NSTextField labelWithString:@"This closes all private windows and deletes history and other site data from the current private browsing session."];
    [body_label setTextColor:[NSColor secondaryLabelColor]];
    [body_label setLineBreakMode:NSLineBreakByWordWrapping];
    [body_label setMaximumNumberOfLines:0];
    [body_label setPreferredMaxLayoutWidth:PRIVATE_SESSION_POPOVER_CONTENT_WIDTH];
    [[body_label widthAnchor] constraintEqualToConstant:PRIVATE_SESSION_POPOVER_CONTENT_WIDTH].active = YES;
    [stack addArrangedSubview:body_label];

    auto* button_row = [[NSStackView alloc] init];
    [button_row setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [button_row setSpacing:PRIVATE_SESSION_POPOVER_BUTTON_SPACING];

    auto* spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
    [button_row addArrangedSubview:spacer];

    auto* cancel_button = [NSButton buttonWithTitle:@"Cancel"
                                             target:self
                                             action:@selector(cancelPrivateSessionReset:)];
    [cancel_button setBezelStyle:NSBezelStyleRounded];
    [button_row addArrangedSubview:cancel_button];

    auto* restart_button = [NSButton buttonWithTitle:@"Restart Private Session"
                                              target:self
                                              action:@selector(confirmPrivateSessionReset:)];
    [restart_button setBezelStyle:NSBezelStyleRounded];
    [restart_button setKeyEquivalent:@"\r"];
    [button_row addArrangedSubview:restart_button];

    [stack addArrangedSubview:button_row];
    [self setView:stack];

    [stack layoutSubtreeIfNeeded];
    auto fitting_size = [stack fittingSize];
    fitting_size.width = PRIVATE_SESSION_POPOVER_WIDTH;
    [self setPreferredContentSize:fitting_size];
}

- (void)cancelPrivateSessionReset:(id)sender
{
    if (self.onCancel)
        self.onCancel();
}

- (void)confirmPrivateSessionReset:(id)sender
{
    if (self.onCancel)
        self.onCancel();

    if (self.onConfirm)
        self.onConfirm();
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
    WebView::IsPrivate m_is_private;
    u64 m_page_index;

    OwnPtr<WebView::Omnibox> m_omnibox;
    OwnPtr<DownloadsObserver> m_downloads_observer;
    bool m_show_downloads_popover_when_button_is_ready;
    bool m_is_applying_omnibox_display;

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
@property (nonatomic, strong) NSToolbarItem* private_browsing_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* downloads_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* new_tab_toolbar_item;
@property (nonatomic, strong) NSToolbarItem* tab_overview_toolbar_item;

@property (nonatomic, strong) NSPopover* private_session_popover;

@property (nonatomic, strong) DownloadsButton* downloads_button;
@property (nonatomic, strong) NSPopover* downloads_popover;

@property (nonatomic, strong) Autocomplete* autocomplete;

@property (nonatomic, assign) NSLayoutConstraint* location_toolbar_item_width;

- (LocationSearchField*)locationSearchField;
- (NSString*)currentLocationFieldQuery;
- (BOOL)locationFieldCursorIsAtEnd;
- (void)applyOmniboxDisplay:(WebView::Omnibox::Display const&)display;
- (void)locationFieldSelectionDidChange:(NSNotification*)notification;
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
@synthesize private_browsing_toolbar_item = _private_browsing_toolbar_item;
@synthesize downloads_toolbar_item = _downloads_toolbar_item;
@synthesize new_tab_toolbar_item = _new_tab_toolbar_item;
@synthesize tab_overview_toolbar_item = _tab_overview_toolbar_item;

- (instancetype)init:(WebView::IsPrivate)is_private
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

        m_is_private = is_private;
        m_page_index = 0;

        m_is_applying_omnibox_display = false;
        m_fullscreen_requested_for_web_content = false;
        m_fullscreen_exit_was_ui_initiated = true;
        m_fullscreen_should_restore_tab_bar = false;

        self.autocomplete = [[Autocomplete alloc] init:self withToolbarItem:self.location_toolbar_item];
        m_omnibox = make<WebView::Omnibox>(m_is_private);
        m_downloads_observer = make<DownloadsObserver>(self);

        m_omnibox->on_display_change = [weak_self](WebView::Omnibox::Display const& display) {
            TabController* self = weak_self;
            if (self == nil)
                return;

            [self applyOmniboxDisplay:display];
        };

        m_omnibox->on_suggestions_change = [weak_self] {
            TabController* self = weak_self;
            if (self == nil)
                return;

            if (!self->m_omnibox->is_popup_visible()) {
                [self.autocomplete close];
                return;
            }

            [self.autocomplete showWithSuggestions:self->m_omnibox->suggestions()
                           selectedSuggestionIndex:ns_index_for_selected_suggestion(self->m_omnibox->selected_suggestion())];
        };

        m_omnibox->on_selection_change = [weak_self] {
            TabController* self = weak_self;
            if (self == nil)
                return;

            [self.autocomplete setSelectedSuggestionIndex:ns_index_for_selected_suggestion(self->m_omnibox->selected_suggestion())];
        };

        m_omnibox->on_commit = [weak_self](String input) {
            TabController* self = weak_self;
            if (self == nil)
                return;

            [self navigateToLocation:move(input)];
        };
    }

    return self;
}

- (instancetype)initAsChild:(Tab*)parent
                  pageIndex:(u64)page_index
{
    if (self = [self init:[parent isPrivate]]) {
        self.parent = parent;

        m_page_index = page_index;
        m_fullscreen_requested_for_web_content = false;
        m_fullscreen_exit_was_ui_initiated = true;
        m_fullscreen_should_restore_tab_bar = false;
    }

    return self;
}

#pragma mark - Public methods

- (WebView::IsPrivate)isPrivate
{
    return m_is_private;
}

- (void)loadURL:(URL::URL const&)url
{
    [[self tab].web_view loadURL:url];
}

- (void)onLoadStart
{
    [[self locationSearchField] setLoading:YES];
}

- (void)onLoadFinish
{
    [[self locationSearchField] setLoading:NO];
}

- (void)onFaviconChange:(NSImage*)favicon
{
    [[self locationSearchField] setFavicon:favicon];
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

- (LocationSearchField*)locationSearchField
{
    return (LocationSearchField*)[self.location_toolbar_item view];
}

- (void)createNewTab:(id)sender
{
    auto* delegate = (ApplicationDelegate*)[NSApp delegate];

    self.tab.titlebarAppearsTransparent = NO;

    [delegate createNewTab:WebView::Application::settings().new_tab_page_url()
                   fromTab:[self tab]
                 isPrivate:[[self tab] isPrivate]
               activateTab:Web::HTML::ActivateTab::Yes
               tabLocation:TabLocation::end()];

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

    auto* location_search_field = [self locationSearchField];
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
    auto* location_search_field = [self locationSearchField];
    if (![[location_search_field stringValue] isEqualToString:Ladybird::string_to_ns_string(WebView::url_for_display(url))])
        return;

    m_is_applying_omnibox_display = true;
    [self setLocationFieldText:url.serialize() display:LocationFieldDisplay::Editing];

    auto* editor = (NSTextView*)[location_search_field currentEditor];
    if (editor != nil && [self.window firstResponder] == editor && ![editor hasMarkedText]) {
        auto* serialized_url = Ladybird::string_to_ns_string(url.serialize());
        [editor setString:serialized_url];
        [editor setSelectedRange:NSMakeRange(0, serialized_url.length)];
    }
    m_is_applying_omnibox_display = false;
}

- (NSString*)currentLocationFieldQuery
{
    auto* location_search_field = [self locationSearchField];
    auto* editor = (NSTextView*)[location_search_field currentEditor];

    // Omnibox display updates mutate the field contents in place, so callers need the typed prefix,
    // not an inline completion or row preview suffix selected through the end of the text.
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

- (BOOL)locationFieldCursorIsAtEnd
{
    auto* location_search_field = [self locationSearchField];
    auto* editor = (NSTextView*)[location_search_field currentEditor];
    if (editor == nil || [self.window firstResponder] != editor)
        return NO;

    auto* text = [[editor textStorage] string];
    auto selected_range = [editor selectedRange];
    if (selected_range.location == NSNotFound)
        return NO;

    return selected_range.location == text.length
        || (selected_range.length != 0 && NSMaxRange(selected_range) == text.length);
}

- (void)applyOmniboxDisplay:(WebView::Omnibox::Display const&)display
{
    auto* location_search_field = [self locationSearchField];
    auto* editor = (NSTextView*)[location_search_field currentEditor];
    if (editor != nil && [self.window firstResponder] == editor && [editor hasMarkedText])
        return;

    auto* display_text = Ladybird::string_to_ns_string(display.text);
    NSRange selection_range = NSMakeRange(display_text.length, 0);
    if (display.selection_start.has_value()) {
        auto selection_start = ns_string_index_for_byte_offset(display.text, *display.selection_start);
        selection_range = NSMakeRange(selection_start, display_text.length - selection_start);
    }

    m_is_applying_omnibox_display = true;
    [location_search_field setStringValue:display_text];
    if (editor != nil && [self.window firstResponder] == editor) {
        [editor setString:display_text];
        [editor setSelectedRange:selection_range];
        [editor scrollRangeToVisible:NSMakeRange(selection_range.location, 0)];
    }
    m_is_applying_omnibox_display = false;
}

- (void)locationFieldSelectionDidChange:(NSNotification*)notification
{
    if (m_is_applying_omnibox_display)
        return;

    auto* editor = (NSTextView*)[[self locationSearchField] currentEditor];
    if (editor == nil || [notification object] != editor || [self.window firstResponder] != editor || [editor hasMarkedText])
        return;

    m_omnibox->cursor_moved([self locationFieldCursorIsAtEnd]);
}

- (BOOL)navigateToLocation:(String)location
{
    if (auto url = WebView::sanitize_url(location, WebView::Application::settings().search_engine()); url.has_value()) {
        [self loadURL:*url];
    } else {
        [[[self tab] web_view] view].load_navigation_error_page(location);
    }

    [self focusWebView];

    return YES;
}

- (void)showTabOverview:(id)sender
{
    self.tab.titlebarAppearsTransparent = NO;
    [self.window toggleTabOverview:sender];
    self.tab.titlebarAppearsTransparent = YES;
}

- (void)showPrivateSessionPopover:(id)sender
{
    if (m_is_private == WebView::IsPrivate::No)
        return;

    if (!self.private_session_popover) {
        auto* content_view_controller = [[PrivateSessionPopoverViewController alloc] init];
        __weak TabController* weak_self = self;
        [content_view_controller setOnCancel:^{
            TabController* self = weak_self;
            if (self != nil)
                [self.private_session_popover close];
        }];
        [content_view_controller setOnConfirm:^{
            TabController* self = weak_self;
            if (self == nil)
                return;

            auto* delegate = (ApplicationDelegate*)[NSApp delegate];
            [delegate restartPrivateBrowsingSession];
        }];

        self.private_session_popover = [[NSPopover alloc] init];
        [self.private_session_popover setAnimates:YES];
        [self.private_session_popover setBehavior:NSPopoverBehaviorTransient];
        [self.private_session_popover setContentViewController:content_view_controller];
    }

    [self.private_session_popover showRelativeToToolbarItem:self.private_browsing_toolbar_item];
}

- (void)togglePrivateSessionPopover:(id)sender
{
    if (self.private_session_popover && [self.private_session_popover isShown]) {
        [self.private_session_popover close];
        return;
    }

    [self showPrivateSessionPopover:sender];
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

- (NSToolbarItem*)private_browsing_toolbar_item
{
    if (!_private_browsing_toolbar_item) {
        auto* badge_button = [[PrivateBrowsingBadgeButton alloc] init];
        [badge_button setTarget:self];
        [badge_button setAction:@selector(togglePrivateSessionPopover:)];

        _private_browsing_toolbar_item = [[NSToolbarItem alloc] initWithItemIdentifier:TOOLBAR_PRIVATE_BROWSING_IDENTIFIER];
        [_private_browsing_toolbar_item setView:badge_button];
        [_private_browsing_toolbar_item setLabel:@"Private Browsing"];
        [_private_browsing_toolbar_item setPaletteLabel:@"Private Browsing"];
    }

    return _private_browsing_toolbar_item;
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
        if (m_is_private == WebView::IsPrivate::Yes) {
            _toolbar_identifiers = @[
                TOOLBAR_NAVIGATE_BACK_IDENTIFIER,
                TOOLBAR_NAVIGATE_FORWARD_IDENTIFIER,
                NSToolbarFlexibleSpaceItemIdentifier,
                TOOLBAR_RELOAD_IDENTIFIER,
                TOOLBAR_LOCATION_IDENTIFIER,
                TOOLBAR_PRIVATE_BROWSING_IDENTIFIER,
                TOOLBAR_DOWNLOADS_IDENTIFIER,
                NSToolbarFlexibleSpaceItemIdentifier,
                TOOLBAR_NEW_TAB_IDENTIFIER,
                TOOLBAR_TAB_OVERVIEW_IDENTIFIER,
            ];
        } else {
            _toolbar_identifiers = @[
                TOOLBAR_NAVIGATE_BACK_IDENTIFIER,
                TOOLBAR_NAVIGATE_FORWARD_IDENTIFIER,
                NSToolbarFlexibleSpaceItemIdentifier,
                TOOLBAR_RELOAD_IDENTIFIER,
                TOOLBAR_LOCATION_IDENTIFIER,
                TOOLBAR_DOWNLOADS_IDENTIFIER,
                NSToolbarFlexibleSpaceItemIdentifier,
                TOOLBAR_NEW_TAB_IDENTIFIER,
                TOOLBAR_TAB_OVERVIEW_IDENTIFIER,
            ];
        }
    }

    return _toolbar_identifiers;
}

#pragma mark - NSWindowController

- (IBAction)showWindow:(id)sender
{
    self.window = self.parent
        ? [[Tab alloc] initAsChild:self.parent pageIndex:m_page_index]
        : [[Tab alloc] init:m_is_private];

    [self.window setDelegate:self];

    [self.window setToolbar:self.toolbar];
    [self.window setToolbarStyle:NSWindowToolbarStyleUnified];

    auto& view = [[[self tab] web_view] view];
    [[self locationSearchField] setBookmarkAction:view.toggle_bookmark_action()];
    [[self locationSearchField] setZoomAction:view.reset_zoom_action()];

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
    if ([identifier isEqual:TOOLBAR_PRIVATE_BROWSING_IDENTIFIER]) {
        return self.private_browsing_toolbar_item;
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

    auto* location_search_field = [self locationSearchField];
    m_omnibox->begin_editing(Ladybird::ns_string_to_string([location_search_field stringValue]));

    auto* editor = (NSTextView*)[location_search_field currentEditor];
    if (editor != nil) {
        [[NSNotificationCenter defaultCenter] removeObserver:self
                                                        name:NSTextViewDidChangeSelectionNotification
                                                      object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self
                                                 selector:@selector(locationFieldSelectionDidChange:)
                                                     name:NSTextViewDidChangeSelectionNotification
                                                   object:editor];
    }
}

- (BOOL)control:(NSControl*)control
               textView:(NSTextView*)text_view
    doCommandBySelector:(SEL)selector
{
    if (selector_deletes_text(selector))
        m_omnibox->will_delete_text();

    if (selector == @selector(cancelOperation:)) {
        if (m_omnibox->escape_pressed() == WebView::Omnibox::EscapeAction::ClosedPopup)
            return YES;
        auto const& url = [[[self tab] web_view] view].url();
        [self setLocationFieldText:url.serialize()];
        [self.window makeFirstResponder:nil];
        return YES;
    }

    if (selector == @selector(moveDown:)) {
        if (m_omnibox->select_next_suggestion())
            return YES;
    }

    if (selector == @selector(moveUp:)) {
        if (m_omnibox->select_previous_suggestion())
            return YES;
    }

    if (selector != @selector(insertNewline:)) {
        return NO;
    }

    m_omnibox->return_pressed();
    return YES;
}

- (void)controlTextDidEndEditing:(NSNotification*)notification
{
    // AppKit can send this while focus is still settling into the field
    // editor. Wait until the next turn so transient notifications do not
    // format the live editor contents as a non-editing URL.
    dispatch_async(dispatch_get_main_queue(), ^{
        auto* location_search_field = [self locationSearchField];
        auto* editor = (NSTextView*)[location_search_field currentEditor];
        if (editor != nil && [self.window firstResponder] == editor)
            return;

        [[NSNotificationCenter defaultCenter] removeObserver:self
                                                        name:NSTextViewDidChangeSelectionNotification
                                                      object:nil];
        m_omnibox->end_editing();
        [self.autocomplete close];
        NSString* url_string = [[location_search_field stringValue] copy];
        [self setLocationFieldText:Ladybird::ns_string_to_string(url_string)];
    });
}

- (void)controlTextDidChange:(NSNotification*)notification
{
    if (m_is_applying_omnibox_display)
        return;

    auto* location_search_field = [self locationSearchField];
    auto* editor = (NSTextView*)[location_search_field currentEditor];
    if (editor != nil && [self.window firstResponder] == editor && [editor hasMarkedText]) {
        m_omnibox->set_suspended(true);
        return;
    }

    [location_search_field setShowsPageIcon:NO];
    m_omnibox->set_suspended(false);
    auto* query = [self currentLocationFieldQuery];
    m_omnibox->text_edited(Ladybird::ns_string_to_string(query), [self locationFieldCursorIsAtEnd]);
}

#pragma mark - AutocompleteObserver

- (void)onHighlightedSuggestion:(NSUInteger)suggestion_index
{
    m_omnibox->suggestion_hovered(static_cast<size_t>(suggestion_index));
}

- (void)onAutocompleteDidClose
{
    m_omnibox->popup_dismissed();
}

- (void)onSelectedSuggestion:(NSUInteger)suggestion_index
{
    m_omnibox->suggestion_clicked(static_cast<size_t>(suggestion_index));
}

@end
