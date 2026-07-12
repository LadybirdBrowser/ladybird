/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/Autocomplete.h>
#import <Utilities/Conversions.h>

static NSString* const AUTOCOMPLETE_IDENTIFIER = @"Autocomplete";
static constexpr CGFloat const POPOVER_PADDING = 8;
static constexpr CGFloat const MINIMUM_WIDTH = 100;
static constexpr CGFloat const CELL_HORIZONTAL_PADDING = 12;
static constexpr CGFloat const CELL_VERTICAL_PADDING = 8;
static constexpr CGFloat const CELL_ICON_SIZE = 20;
static constexpr CGFloat const CELL_ICON_TEXT_SPACING = 10;
static constexpr CGFloat const CELL_LABEL_VERTICAL_SPACING = 4;
static constexpr size_t MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS = 6;

static NSFont* autocomplete_primary_font();
static NSFont* autocomplete_secondary_font();
static CGFloat autocomplete_text_field_height(NSFont* font)
{
    static CGFloat primary_text_field_height = 0;
    static CGFloat secondary_text_field_height = 0;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        auto* text_field = [[NSTextField alloc] initWithFrame:NSZeroRect];
        [text_field setBezeled:NO];
        [text_field setDrawsBackground:NO];
        [text_field setEditable:NO];
        [text_field setStringValue:@"Ladybird"];

        [text_field setFont:autocomplete_primary_font()];
        primary_text_field_height = ceil([text_field fittingSize].height);

        [text_field setFont:autocomplete_secondary_font()];
        secondary_text_field_height = ceil([text_field fittingSize].height);
    });

    if (font == autocomplete_secondary_font())
        return secondary_text_field_height;
    return primary_text_field_height;
}

static CGFloat autocomplete_row_height()
{
    static CGFloat row_height = 0;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        auto content_height = max(CELL_ICON_SIZE,
            autocomplete_text_field_height(autocomplete_primary_font())
                + CELL_LABEL_VERTICAL_SPACING
                + autocomplete_text_field_height(autocomplete_secondary_font()));
        row_height = ceil(content_height + (CELL_VERTICAL_PADDING * 2));
    });
    return row_height;
}

static CGFloat autocomplete_single_line_row_height()
{
    static CGFloat row_height = 0;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        row_height = ceil(max(CELL_ICON_SIZE, autocomplete_text_field_height(autocomplete_primary_font())) + (CELL_VERTICAL_PADDING * 2));
    });
    return row_height;
}

static NSFont* autocomplete_primary_font()
{
    static NSFont* font;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        font = [NSFont systemFontOfSize:[NSFont systemFontSize] + 1.5 weight:NSFontWeightRegular];
    });
    return font;
}

static NSFont* autocomplete_secondary_font()
{
    static NSFont* font;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize] + 1];
    });
    return font;
}

static NSImage* search_suggestion_icon()
{
    static NSImage* image;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        image = [NSImage imageWithSystemSymbolName:@"magnifyingglass" accessibilityDescription:@""];
        [image setSize:NSMakeSize(CELL_ICON_SIZE, CELL_ICON_SIZE)];
    });
    return image;
}

static NSImage* literal_url_suggestion_icon()
{
    static NSImage* image;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        image = [NSImage imageWithSystemSymbolName:@"globe" accessibilityDescription:@""];
        [image setSize:NSMakeSize(CELL_ICON_SIZE, CELL_ICON_SIZE)];
    });
    return image;
}

static NSAttributedString* autocomplete_attributed_string(StringView text, StringView highlight_input, NSFont* font, NSColor* color, bool emphasize_origin)
{
    auto string = MUST(String::from_utf8(text));
    auto* ns_string = Ladybird::string_to_ns_string(string);
    auto* attributed_string = [[NSMutableAttributedString alloc] initWithString:ns_string
                                                                     attributes:@ {
                                                                         NSFontAttributeName : font,
                                                                         NSForegroundColorAttributeName : color,
                                                                     }];

    if (emphasize_origin) {
        auto slash = text.find('/');
        auto origin_length = slash.value_or(text.length());
        auto origin = Ladybird::string_to_ns_string(MUST(String::from_utf8(text.substring_view(0, origin_length))));
        [attributed_string addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:font.pointSize weight:NSFontWeightMedium] range:NSMakeRange(0, origin.length)];
    }

    for (auto const& range : WebView::autocomplete_match_ranges(highlight_input, text)) {
        auto prefix = Ladybird::string_to_ns_string(MUST(String::from_utf8(text.substring_view(0, range.start))));
        auto match = Ladybird::string_to_ns_string(MUST(String::from_utf8(text.substring_view(range.start, range.length))));
        auto ns_range = NSMakeRange(prefix.length, match.length);
        [attributed_string addAttribute:NSFontAttributeName value:[NSFont systemFontOfSize:font.pointSize weight:NSFontWeightBold] range:ns_range];
        [attributed_string addAttribute:NSForegroundColorAttributeName value:[NSColor labelColor] range:ns_range];
    }
    return attributed_string;
}

static CGFloat autocomplete_visible_width(NSView* view)
{
    auto* scroll_view = view.enclosingScrollView;
    return scroll_view ? scroll_view.contentSize.width : NSWidth(view.bounds);
}

@interface AutocompleteRowView : NSTableRowView
@property (nonatomic) BOOL hovered;
@end

@implementation AutocompleteRowView

- (void)setHovered:(BOOL)hovered
{
    _hovered = hovered;
    [self setNeedsDisplay:YES];
}

- (void)drawBackgroundInRect:(NSRect)dirtyRect
{
    if (!self.hovered || self.isSelected)
        return;
    auto visible_bounds = NSMakeRect(0, 0, autocomplete_visible_width(self), NSHeight(self.bounds));
    auto* path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(visible_bounds, 4, 2) xRadius:7 yRadius:7];
    [[[NSColor labelColor] colorWithAlphaComponent:0.06] setFill];
    [path fill];
}

- (void)drawSelectionInRect:(NSRect)dirtyRect
{
    auto visible_bounds = NSMakeRect(0, 0, autocomplete_visible_width(self), NSHeight(self.bounds));
    auto selection_rect = NSInsetRect(visible_bounds, 4, 2);
    auto* selection_path = [NSBezierPath bezierPathWithRoundedRect:selection_rect xRadius:7 yRadius:7];

    [[[NSColor controlAccentColor] colorWithAlphaComponent:0.16] setFill];
    [selection_path fill];
}

@end

@interface AutocompleteSuggestionView : NSTableCellView

@property (nonatomic, strong) NSImageView* icon_view;
@property (nonatomic, strong) NSImageView* badge_view;
@property (nonatomic, strong) NSTextField* title_text_field;
@property (nonatomic, strong) NSTextField* url_text_field;

@end

@implementation AutocompleteSuggestionView
@end

@interface AutocompleteTableView : NSTableView

@property (nonatomic, strong) NSTrackingArea* tracking_area;
@property (nonatomic) NSInteger hovered_row;

@end

@implementation AutocompleteTableView

- (instancetype)initWithFrame:(NSRect)frame
{
    if (self = [super initWithFrame:frame])
        self.hovered_row = -1;
    return self;
}

- (void)updateTrackingAreas
{
    if (self.tracking_area != nil)
        [self removeTrackingArea:self.tracking_area];

    self.tracking_area = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                      options:NSTrackingActiveAlways | NSTrackingInVisibleRect | NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited
                                                        owner:self
                                                     userInfo:nil];
    [self addTrackingArea:self.tracking_area];

    [super updateTrackingAreas];
}

- (void)mouseMoved:(NSEvent*)event
{
    [super mouseMoved:event];

    auto point = [self convertPoint:event.locationInWindow fromView:nil];
    auto row = [self rowAtPoint:point];
    if (row == self.hovered_row)
        return;
    if (self.hovered_row >= 0)
        [(AutocompleteRowView*)[self rowViewAtRow:self.hovered_row makeIfNecessary:NO] setHovered:NO];
    self.hovered_row = row;
    if (self.hovered_row >= 0)
        [(AutocompleteRowView*)[self rowViewAtRow:self.hovered_row makeIfNecessary:NO] setHovered:YES];
}

- (void)mouseExited:(NSEvent*)event
{
    [super mouseExited:event];
    if (self.hovered_row >= 0)
        [(AutocompleteRowView*)[self rowViewAtRow:self.hovered_row makeIfNecessary:NO] setHovered:NO];
    self.hovered_row = -1;
}

@end

@interface AutocompleteWindow : NSWindow
@end

@implementation AutocompleteWindow

- (BOOL)canBecomeKeyWindow
{
    return NO;
}

- (BOOL)canBecomeMainWindow
{
    return NO;
}

@end

@interface Autocomplete () <NSTableViewDataSource, NSTableViewDelegate>
{
    Vector<WebView::AutocompleteSuggestion> m_suggestions;
}

@property (nonatomic, weak) id<AutocompleteObserver> observer;
@property (nonatomic, weak) NSToolbarItem* toolbar_item;

@property (nonatomic, strong) AutocompleteWindow* popup_window;
@property (nonatomic, strong) NSView* content_view;
@property (nonatomic, strong) NSScrollView* scroll_view;
@property (nonatomic, strong) NSTableView* table_view;
@property (nonatomic, strong) NSMutableDictionary<NSString*, NSImage*>* suggestion_icons;

- (NSInteger)tableRowForSuggestionIndex:(NSInteger)suggestion_index;
- (BOOL)isSelectableRow:(NSInteger)row;
- (CGFloat)heightOfRowAtIndex:(size_t)row;
- (CGFloat)tableHeightForVisibleSuggestionCount:(size_t)visible_suggestion_count;
- (void)selectRow:(NSInteger)row;

@end

@implementation Autocomplete

- (instancetype)init:(id<AutocompleteObserver>)observer
     withToolbarItem:(NSToolbarItem*)toolbar_item
{
    if (self = [super init]) {
        self.observer = observer;
        self.toolbar_item = toolbar_item;

        auto* column = [[NSTableColumn alloc] init];
        [column setEditable:NO];

        self.table_view = [[AutocompleteTableView alloc] initWithFrame:NSZeroRect];
        [self.table_view setAction:@selector(selectSuggestion:)];
        [self.table_view setBackgroundColor:[NSColor clearColor]];
        [self.table_view setHeaderView:nil];
        [self.table_view setIntercellSpacing:NSMakeSize(0, 0)];
        [self.table_view setRefusesFirstResponder:YES];
        [self.table_view setStyle:NSTableViewStyleFullWidth];
        [self.table_view setRowSizeStyle:NSTableViewRowSizeStyleCustom];
        [self.table_view setRowHeight:autocomplete_row_height()];
        [self.table_view setSelectionHighlightStyle:NSTableViewSelectionHighlightStyleRegular];
        [self.table_view addTableColumn:column];
        [self.table_view setDataSource:self];
        [self.table_view setDelegate:self];
        [self.table_view setTarget:self];

        self.scroll_view = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        [self.scroll_view setAutohidesScrollers:YES];
        [self.scroll_view setBorderType:NSNoBorder];
        [self.scroll_view setDrawsBackground:NO];
        [self.scroll_view setHasHorizontalScroller:NO];
        [self.scroll_view setHasVerticalScroller:NO];
        [self.scroll_view setHorizontalScrollElasticity:NSScrollElasticityNone];
        [self.scroll_view setVerticalScrollElasticity:NSScrollElasticityNone];
        [self.scroll_view setDocumentView:self.table_view];

        self.content_view = [[NSView alloc] initWithFrame:NSZeroRect];
        [self.content_view setWantsLayer:YES];
        [self.content_view.layer setBackgroundColor:[NSColor windowBackgroundColor].CGColor];
        [self.content_view.layer setCornerRadius:10];
        [self.content_view.layer setMaskedCorners:kCALayerMinXMinYCorner | kCALayerMaxXMinYCorner];
        [self.content_view.layer setBorderWidth:1];
        [self.content_view.layer setBorderColor:[[NSColor separatorColor] colorWithAlphaComponent:0.55].CGColor];
        [self.content_view addSubview:self.scroll_view];
        self.suggestion_icons = [NSMutableDictionary dictionary];

        self.popup_window = [[AutocompleteWindow alloc] initWithContentRect:NSZeroRect
                                                                  styleMask:NSWindowStyleMaskBorderless
                                                                    backing:NSBackingStoreBuffered
                                                                      defer:NO];
        [self.popup_window setAcceptsMouseMovedEvents:YES];
        [self.popup_window setBackgroundColor:[NSColor clearColor]];
        [self.popup_window setContentView:self.content_view];
        [self.popup_window setHasShadow:YES];
        [self.popup_window setLevel:NSPopUpMenuWindowLevel];
        [self.popup_window setOpaque:NO];
        [self.popup_window setReleasedWhenClosed:NO];
    }

    return self;
}

#pragma mark - Public methods

- (void)showWithSuggestions:(Vector<WebView::AutocompleteSuggestion>)suggestions
    selectedSuggestionIndex:(NSInteger)selected_suggestion_index
{
    m_suggestions = move(suggestions);
    [self.suggestion_icons removeAllObjects];

    for (auto const& suggestion : m_suggestions) {
        if (suggestion.favicon_base64_png.has_value()) {
            auto* suggestion_text = Ladybird::string_to_ns_string(suggestion.text);
            if (auto* favicon = Ladybird::image_from_base64_png(*suggestion.favicon_base64_png, NSMakeSize(CELL_ICON_SIZE, CELL_ICON_SIZE)); favicon != nil)
                [self.suggestion_icons setObject:favicon forKey:suggestion_text];
        }
    }

    [self.table_view reloadData];

    if (m_suggestions.is_empty())
        [self close];
    else
        [self show];

    [self setSelectedSuggestionIndex:selected_suggestion_index];
}

- (void)setSelectedSuggestionIndex:(NSInteger)selected_suggestion_index
{
    auto table_row = [self tableRowForSuggestionIndex:selected_suggestion_index];
    if (table_row == NSNotFound)
        [self clearSelection];
    else if (table_row != self.table_view.selectedRow) {
        // Refreshing the default row should not behave like an explicit
        // highlight, or the location field will re-preview the suggestion.
        [self selectRow:table_row];
    }
}

- (BOOL)close
{
    if (!self.popup_window.isVisible)
        return NO;

    if (auto* parent_window = [self.toolbar_item.view window])
        [parent_window removeChildWindow:self.popup_window];

    [self.popup_window orderOut:nil];
    [self.observer onAutocompleteDidClose];
    return YES;
}

- (BOOL)isVisible
{
    return self.popup_window.isVisible;
}

- (void)clearSelection
{
    [self.table_view deselectAll:nil];
}

- (void)selectSuggestion:(id)sender
{
    auto row = [self.table_view selectedRow];
    if (![self isSelectableRow:row])
        return;

    [self.observer onSelectedSuggestion:static_cast<size_t>(row)];
}

#pragma mark - Private methods

- (BOOL)isSelectableRow:(NSInteger)row
{
    if (row < 0 || row >= static_cast<NSInteger>(m_suggestions.size()))
        return NO;
    return YES;
}

- (NSInteger)tableRowForSuggestionIndex:(NSInteger)suggestion_index
{
    if (suggestion_index < 0 || suggestion_index >= static_cast<NSInteger>(m_suggestions.size()))
        return NSNotFound;
    return suggestion_index;
}

- (CGFloat)heightOfRowAtIndex:(size_t)row
{
    VERIFY(row < m_suggestions.size());
    auto const& suggestion = m_suggestions[row];
    return suggestion.title.has_value() ? autocomplete_row_height() : autocomplete_single_line_row_height();
}

- (CGFloat)tableHeightForVisibleSuggestionCount:(size_t)visible_suggestion_count
{
    if (visible_suggestion_count == 0)
        return 0;

    CGFloat total_height = 0;
    size_t seen_suggestion_count = 0;

    for (size_t row = 0; row < m_suggestions.size(); ++row) {
        total_height += [self heightOfRowAtIndex:row];
        ++seen_suggestion_count;
        if (seen_suggestion_count >= visible_suggestion_count)
            break;
    }

    return ceil(total_height);
}

- (void)show
{
    auto* toolbar_view = self.toolbar_item.view;
    auto* parent_window = [toolbar_view window];
    if (parent_window == nil)
        return;
    auto was_visible = self.popup_window.isVisible;

    auto visible_suggestion_count = min(m_suggestions.size(), MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS);

    auto visible_table_height = [self tableHeightForVisibleSuggestionCount:visible_suggestion_count];
    auto full_table_height = [self tableHeightForVisibleSuggestionCount:m_suggestions.size()];
    auto width = max<CGFloat>(toolbar_view.frame.size.width, MINIMUM_WIDTH);
    auto content_size = NSMakeSize(width, visible_table_height + (POPOVER_PADDING * 2));

    [self.content_view setFrame:NSMakeRect(0, 0, content_size.width, content_size.height)];
    [self.scroll_view setFrame:NSInsetRect(self.content_view.bounds, 0, POPOVER_PADDING)];

    CGFloat document_width = self.scroll_view.contentSize.width;
    [self.table_view setFrame:NSMakeRect(0, 0, document_width, full_table_height)];

    if (auto* column = self.table_view.tableColumns.firstObject)
        [column setWidth:document_width];

    if (!was_visible)
        [self.table_view deselectAll:nil];
    [self.table_view scrollRowToVisible:self.table_view.selectedRow >= 0 ? self.table_view.selectedRow : 0];

    auto anchor_rect = [toolbar_view convertRect:toolbar_view.bounds toView:nil];
    auto popup_rect = [parent_window convertRectToScreen:anchor_rect];
    popup_rect.origin.y -= content_size.height;
    popup_rect.size = content_size;

    [self.popup_window setFrame:popup_rect display:NO];

    if (!was_visible)
        [parent_window addChildWindow:self.popup_window ordered:NSWindowAbove];

    [self.popup_window orderFront:nil];
}

- (void)selectRow:(NSInteger)row
{
    if (![self isSelectableRow:row])
        return;

    [self.table_view selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    [self.table_view scrollRowToVisible:[self.table_view selectedRow]];
}

#pragma mark - NSTableViewDataSource

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
    return static_cast<NSInteger>(m_suggestions.size());
}

#pragma mark - NSTableViewDelegate

- (CGFloat)tableView:(NSTableView*)tableView heightOfRow:(NSInteger)row
{
    return [self heightOfRowAtIndex:static_cast<size_t>(row)];
}

- (NSTableRowView*)tableView:(NSTableView*)tableView
               rowViewForRow:(NSInteger)row
{
    auto visible_width = autocomplete_visible_width(tableView);
    return [[AutocompleteRowView alloc] initWithFrame:NSMakeRect(0, 0, visible_width, [self tableView:tableView heightOfRow:row])];
}

- (NSView*)tableView:(NSTableView*)table_view
    viewForTableColumn:(NSTableColumn*)table_column
                   row:(NSInteger)row
{
    auto visible_width = autocomplete_visible_width(table_view);

    AutocompleteSuggestionView* view = (AutocompleteSuggestionView*)[table_view makeViewWithIdentifier:AUTOCOMPLETE_IDENTIFIER owner:self];

    if (view == nil) {
        view = [[AutocompleteSuggestionView alloc] initWithFrame:NSZeroRect];

        NSImageView* icon_view = [[NSImageView alloc] initWithFrame:NSZeroRect];
        [icon_view setImageScaling:NSImageScaleProportionallyDown];
        [view addSubview:icon_view];
        [view setIcon_view:icon_view];

        NSImageView* badge_view = [[NSImageView alloc] initWithFrame:NSZeroRect];
        [badge_view setImageScaling:NSImageScaleProportionallyDown];
        [badge_view setImage:[NSImage imageWithSystemSymbolName:@"star.fill" accessibilityDescription:@""]];
        [badge_view setContentTintColor:[NSColor secondaryLabelColor]];
        [view addSubview:badge_view];
        [view setBadge_view:badge_view];

        NSTextField* title_text_field = [[NSTextField alloc] initWithFrame:NSZeroRect];
        [title_text_field setBezeled:NO];
        [title_text_field setDrawsBackground:NO];
        [title_text_field setEditable:NO];
        [title_text_field setFont:autocomplete_primary_font()];
        [title_text_field setLineBreakMode:NSLineBreakByTruncatingTail];
        [title_text_field setSelectable:NO];
        [view addSubview:title_text_field];
        [view setTitle_text_field:title_text_field];

        NSTextField* url_text_field = [[NSTextField alloc] initWithFrame:NSZeroRect];
        [url_text_field setBezeled:NO];
        [url_text_field setDrawsBackground:NO];
        [url_text_field setEditable:NO];
        [url_text_field setLineBreakMode:NSLineBreakByTruncatingTail];
        [url_text_field setSelectable:NO];
        [view addSubview:url_text_field];
        [view setUrl_text_field:url_text_field];

        [view setIdentifier:AUTOCOMPLETE_IDENTIFIER];
    }

    auto const& suggestion = m_suggestions[row];
    auto* suggestion_text = Ladybird::string_to_ns_string(suggestion.text);
    auto suggestion_display_text = WebView::autocomplete_suggestion_display_text(suggestion);
    auto* title_text = suggestion.title.has_value() ? Ladybird::string_to_ns_string(*suggestion.title) : nil;
    auto* favicon = [self.suggestion_icons objectForKey:suggestion_text];
    auto* icon = suggestion.source == WebView::AutocompleteSuggestionSource::LiteralURL
        ? literal_url_suggestion_icon()
        : suggestion.source == WebView::AutocompleteSuggestionSource::Search ? search_suggestion_icon()
        : favicon != nil                                                     ? favicon
                                                                             : literal_url_suggestion_icon();

    [view setFrame:NSMakeRect(0, 0, visible_width, [self tableView:table_view heightOfRow:row])];

    auto primary_text_height = autocomplete_text_field_height(autocomplete_primary_font());
    auto secondary_text_height = autocomplete_text_field_height(autocomplete_secondary_font());
    CGFloat text_origin_x = CELL_HORIZONTAL_PADDING + CELL_ICON_SIZE + CELL_ICON_TEXT_SPACING;
    CGFloat text_width = NSWidth(view.bounds) - text_origin_x - CELL_HORIZONTAL_PADDING;

    [view.icon_view setFrame:NSMakeRect(
                                 CELL_HORIZONTAL_PADDING,
                                 floor((NSHeight(view.bounds) - CELL_ICON_SIZE) / 2.f),
                                 CELL_ICON_SIZE,
                                 CELL_ICON_SIZE)];
    [view.icon_view setImage:icon];
    auto is_local_suggestion = suggestion.source == WebView::AutocompleteSuggestionSource::History
        || suggestion.source == WebView::AutocompleteSuggestionSource::Bookmark
        || suggestion.source == WebView::AutocompleteSuggestionSource::Adaptive;
    [view.icon_view setContentTintColor:!is_local_suggestion || favicon == nil ? [NSColor secondaryLabelColor] : nil];
    [view.icon_view setHidden:NO];
    static constexpr CGFloat badge_size = 9;
    [view.badge_view setFrame:NSMakeRect(CELL_HORIZONTAL_PADDING + CELL_ICON_SIZE - badge_size + 2,
                                  floor((NSHeight(view.bounds) - CELL_ICON_SIZE) / 2.f) - 2,
                                  badge_size,
                                  badge_size)];
    [view.badge_view setHidden:suggestion.source != WebView::AutocompleteSuggestionSource::Bookmark];

    if (title_text != nil) {
        CGFloat text_block_height = primary_text_height + CELL_LABEL_VERTICAL_SPACING + secondary_text_height;
        CGFloat text_block_origin_y = floor((NSHeight(view.bounds) - text_block_height) / 2.f);

        [view.title_text_field setHidden:NO];
        [view.title_text_field setAttributedStringValue:autocomplete_attributed_string(suggestion.title->bytes_as_string_view(), suggestion.highlight_input, autocomplete_primary_font(), [NSColor textColor], false)];
        [view.title_text_field setFrame:NSMakeRect(
                                            text_origin_x,
                                            text_block_origin_y + secondary_text_height + CELL_LABEL_VERTICAL_SPACING,
                                            text_width,
                                            primary_text_height)];

        [view.url_text_field setFont:autocomplete_secondary_font()];
        [view.url_text_field setTextColor:[NSColor secondaryLabelColor]];
        [view.url_text_field setFrame:NSMakeRect(
                                          text_origin_x,
                                          text_block_origin_y,
                                          text_width,
                                          secondary_text_height)];
    } else {
        [view.title_text_field setHidden:YES];

        [view.url_text_field setFont:autocomplete_primary_font()];
        [view.url_text_field setTextColor:[NSColor textColor]];
        [view.url_text_field setFrame:NSMakeRect(
                                          text_origin_x,
                                          floor((NSHeight(view.bounds) - primary_text_height) / 2.f),
                                          text_width,
                                          primary_text_height)];
    }

    auto secondary_string = suggestion.subtitle.has_value() ? suggestion.subtitle->bytes_as_string_view() : suggestion_display_text.bytes_as_string_view();
    auto* secondary_font = title_text != nil ? autocomplete_secondary_font() : autocomplete_primary_font();
    auto* secondary_color = title_text != nil || suggestion.source == WebView::AutocompleteSuggestionSource::Search
        ? [NSColor secondaryLabelColor]
        : [NSColor textColor];
    auto emphasize_origin = suggestion.source != WebView::AutocompleteSuggestionSource::Search && !suggestion.subtitle.has_value();
    [view.url_text_field setAttributedStringValue:autocomplete_attributed_string(secondary_string, suggestion.highlight_input, secondary_font, secondary_color, emphasize_origin)];
    return view;
}

@end
