/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/Autocomplete.h>
#import <Utilities/Conversions.h>

static NSString* const AUTOCOMPLETE_IDENTIFIER = @"Autocomplete";
static NSString* const AUTOCOMPLETE_SECTION_HEADER_IDENTIFIER = @"AutocompleteSectionHeader";
static constexpr CGFloat const POPOVER_PADDING = 8;
static constexpr CGFloat const MINIMUM_WIDTH = 100;
static constexpr CGFloat const CELL_HORIZONTAL_PADDING = 8;
static constexpr CGFloat const CELL_VERTICAL_PADDING = 10;
static constexpr CGFloat const CELL_ICON_SIZE = 16;
static constexpr CGFloat const CELL_ICON_TEXT_SPACING = 6;
static constexpr CGFloat const CELL_LABEL_VERTICAL_SPACING = 5;
static constexpr CGFloat const SECTION_HEADER_HORIZONTAL_PADDING = 10;
static constexpr CGFloat const SECTION_HEADER_VERTICAL_PADDING = 4;

static NSFont* autocomplete_primary_font();
static NSFont* autocomplete_secondary_font();
static NSFont* autocomplete_section_header_font();

enum class AutocompleteRowKind {
    SectionHeader,
    Suggestion,
};

struct AutocompleteRowModel {
    AutocompleteRowKind kind;
    String text;
    size_t suggestion_index { 0 };
};

static CGFloat autocomplete_text_field_height(NSFont* font)
{
    static CGFloat primary_text_field_height = 0;
    static CGFloat secondary_text_field_height = 0;
    static CGFloat section_header_text_field_height = 0;
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

        [text_field setFont:autocomplete_section_header_font()];
        section_header_text_field_height = ceil([text_field fittingSize].height);
    });

    if (font == autocomplete_secondary_font())
        return secondary_text_field_height;
    if (font == autocomplete_section_header_font())
        return section_header_text_field_height;
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

static CGFloat autocomplete_section_header_height()
{
    static CGFloat row_height = 0;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        row_height = ceil(autocomplete_text_field_height(autocomplete_section_header_font()) + (SECTION_HEADER_VERTICAL_PADDING * 2));
    });
    return row_height;
}

static NSFont* autocomplete_primary_font()
{
    static NSFont* font;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        font = [NSFont systemFontOfSize:[NSFont systemFontSize] weight:NSFontWeightSemibold];
    });
    return font;
}

static NSFont* autocomplete_secondary_font()
{
    static NSFont* font;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    });
    return font;
}

static NSFont* autocomplete_section_header_font()
{
    static NSFont* font;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize] weight:NSFontWeightSemibold];
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

@protocol AutocompleteTableViewHoverObserver <NSObject>

- (void)autocompleteTableViewHoveredRowChanged:(NSInteger)row;

@end

@interface AutocompleteRowView : NSTableRowView
@end

@implementation AutocompleteRowView

- (void)drawSelectionInRect:(NSRect)dirtyRect
{
    auto selection_rect = NSInsetRect(self.bounds, 2, 3);
    auto* selection_path = [NSBezierPath bezierPathWithRoundedRect:selection_rect xRadius:6 yRadius:6];

    [[[NSColor controlAccentColor] colorWithAlphaComponent:0.25] setFill];
    [selection_path fill];
}

@end

@interface AutocompleteSuggestionView : NSTableCellView

@property (nonatomic, strong) NSImageView* icon_view;
@property (nonatomic, strong) NSTextField* title_text_field;
@property (nonatomic, strong) NSTextField* url_text_field;

@end

@implementation AutocompleteSuggestionView
@end

@interface AutocompleteSectionHeaderView : NSTableCellView

@property (nonatomic, strong) NSTextField* text_field;

@end

@implementation AutocompleteSectionHeaderView
@end

@interface AutocompleteScrollView : NSScrollView
@end

@implementation AutocompleteScrollView

- (void)scrollWheel:(NSEvent*)event
{
}

@end

@interface AutocompleteTableView : NSTableView

@property (nonatomic, weak) id<AutocompleteTableViewHoverObserver> hover_observer;
@property (nonatomic, strong) NSTrackingArea* tracking_area;

@end

@implementation AutocompleteTableView

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
    [self.hover_observer autocompleteTableViewHoveredRowChanged:[self rowAtPoint:point]];
}

- (void)mouseExited:(NSEvent*)event
{
    [super mouseExited:event];
    [self.hover_observer autocompleteTableViewHoveredRowChanged:-1];
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

@interface Autocomplete () <AutocompleteTableViewHoverObserver, NSTableViewDataSource, NSTableViewDelegate>
{
    Vector<WebView::AutocompleteSuggestion> m_suggestions;
    Vector<AutocompleteRowModel> m_rows;
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
- (void)rebuildRows;
- (void)selectRow:(NSInteger)row notifyObserver:(BOOL)notify_observer;
- (NSInteger)stepToSelectableRowFrom:(NSInteger)row direction:(NSInteger)direction;

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
        [(AutocompleteTableView*)self.table_view setHover_observer:self];

        self.scroll_view = [[AutocompleteScrollView alloc] initWithFrame:NSZeroRect];
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
        [self.content_view.layer setCornerRadius:8];
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
                selectedRow:(NSInteger)selected_row
{
    m_suggestions = move(suggestions);
    [self rebuildRows];
    [self.suggestion_icons removeAllObjects];

    for (auto const& suggestion : m_suggestions) {
        if (suggestion.favicon_base64_png.has_value()) {
            auto* suggestion_text = Ladybird::string_to_ns_string(suggestion.text);
            if (auto* favicon = Ladybird::image_from_base64_png(*suggestion.favicon_base64_png, NSMakeSize(CELL_ICON_SIZE, CELL_ICON_SIZE)); favicon != nil)
                [self.suggestion_icons setObject:favicon forKey:suggestion_text];
        }
    }

    [self.table_view reloadData];

    if (m_rows.is_empty())
        [self close];
    else
        [self show];

    auto table_row = [self tableRowForSuggestionIndex:selected_row];
    if (table_row == NSNotFound)
        [self clearSelection];
    else if (table_row != self.table_view.selectedRow) {
        // Refreshing the default row should not behave like an explicit
        // highlight, or the location field will re-preview the suggestion.
        [self selectRow:table_row notifyObserver:NO];
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

- (Optional<String>)selectedSuggestion
{
    if (!self.popup_window.isVisible || self.table_view.numberOfRows == 0)
        return {};

    auto row = [self.table_view selectedRow];
    if (![self isSelectableRow:row])
        return {};

    return m_suggestions[m_rows[row].suggestion_index].text;
}

- (BOOL)selectNextSuggestion
{
    if (self.table_view.numberOfRows == 0)
        return NO;

    if (!self.popup_window.isVisible) {
        [self show];
        if (auto row = [self stepToSelectableRowFrom:-1 direction:1]; row != NSNotFound)
            [self selectRow:row notifyObserver:YES];
        return YES;
    }

    if (auto row = [self stepToSelectableRowFrom:[self.table_view selectedRow] direction:1]; row != NSNotFound)
        [self selectRow:row notifyObserver:YES];
    return YES;
}

- (BOOL)selectPreviousSuggestion
{
    if (self.table_view.numberOfRows == 0)
        return NO;

    if (!self.popup_window.isVisible) {
        [self show];
        if (auto row = [self stepToSelectableRowFrom:0 direction:-1]; row != NSNotFound)
            [self selectRow:row notifyObserver:YES];
        return YES;
    }

    if (auto row = [self stepToSelectableRowFrom:[self.table_view selectedRow] direction:-1]; row != NSNotFound)
        [self selectRow:row notifyObserver:YES];
    return YES;
}

- (void)selectSuggestion:(id)sender
{
    if (auto suggestion = [self selectedSuggestion]; suggestion.has_value())
        [self.observer onSelectedSuggestion:suggestion.release_value()];
}

#pragma mark - Private methods

- (void)rebuildRows
{
    m_rows.clear();
    m_rows.ensure_capacity(m_suggestions.size() * 2);

    auto current_section = WebView::AutocompleteSuggestionSection::None;
    for (size_t suggestion_index = 0; suggestion_index < m_suggestions.size(); ++suggestion_index) {
        auto const& suggestion = m_suggestions[suggestion_index];
        if (suggestion.section != WebView::AutocompleteSuggestionSection::None && suggestion.section != current_section) {
            current_section = suggestion.section;
            m_rows.append({
                .kind = AutocompleteRowKind::SectionHeader,
                .text = MUST(String::from_utf8(WebView::autocomplete_section_title(current_section))),
            });
        }

        m_rows.append({ .kind = AutocompleteRowKind::Suggestion, .suggestion_index = suggestion_index });
    }
}

- (BOOL)isSelectableRow:(NSInteger)row
{
    if (row < 0 || row >= static_cast<NSInteger>(m_rows.size()))
        return NO;
    return m_rows[row].kind == AutocompleteRowKind::Suggestion;
}

- (NSInteger)tableRowForSuggestionIndex:(NSInteger)suggestion_index
{
    if (suggestion_index < 0)
        return NSNotFound;

    for (size_t row = 0; row < m_rows.size(); ++row) {
        auto const& row_model = m_rows[row];
        if (row_model.kind == AutocompleteRowKind::Suggestion
            && row_model.suggestion_index == static_cast<size_t>(suggestion_index))
            return static_cast<NSInteger>(row);
    }

    return NSNotFound;
}

- (CGFloat)heightOfRowAtIndex:(size_t)row
{
    VERIFY(row < m_rows.size());
    return m_rows[row].kind == AutocompleteRowKind::SectionHeader
        ? autocomplete_section_header_height()
        : autocomplete_row_height();
}

- (CGFloat)tableHeightForVisibleSuggestionCount:(size_t)visible_suggestion_count
{
    if (visible_suggestion_count == 0)
        return 0;

    CGFloat total_height = 0;
    size_t seen_suggestion_count = 0;

    for (size_t row = 0; row < m_rows.size(); ++row) {
        total_height += [self heightOfRowAtIndex:row];
        if (m_rows[row].kind == AutocompleteRowKind::Suggestion) {
            ++seen_suggestion_count;
            if (seen_suggestion_count >= visible_suggestion_count)
                break;
        }
    }

    return ceil(total_height);
}

- (NSInteger)stepToSelectableRowFrom:(NSInteger)row direction:(NSInteger)direction
{
    if (self.table_view.numberOfRows == 0)
        return NSNotFound;

    auto candidate = row;
    for (NSInteger attempt = 0; attempt < self.table_view.numberOfRows; ++attempt) {
        candidate += direction;
        if (candidate < 0)
            candidate = self.table_view.numberOfRows - 1;
        else if (candidate >= self.table_view.numberOfRows)
            candidate = 0;

        if ([self isSelectableRow:candidate])
            return candidate;
    }

    return NSNotFound;
}

- (void)autocompleteTableViewHoveredRowChanged:(NSInteger)row
{
    if (![self isSelectableRow:row])
        return;

    if (row == self.table_view.selectedRow)
        return;

    [self selectRow:row notifyObserver:YES];
}

- (void)show
{
    auto* toolbar_view = self.toolbar_item.view;
    auto* parent_window = [toolbar_view window];
    if (parent_window == nil)
        return;
    auto was_visible = self.popup_window.isVisible;

    size_t visible_suggestion_count = 0;
    for (auto const& row_model : m_rows) {
        if (row_model.kind == AutocompleteRowKind::Suggestion)
            ++visible_suggestion_count;
    }

    auto visible_table_height = [self tableHeightForVisibleSuggestionCount:visible_suggestion_count];
    auto width = max<CGFloat>(toolbar_view.frame.size.width, MINIMUM_WIDTH);
    auto content_size = NSMakeSize(width, visible_table_height + (POPOVER_PADDING * 2));

    [self.content_view setFrame:NSMakeRect(0, 0, content_size.width, content_size.height)];
    [self.scroll_view setFrame:NSInsetRect(self.content_view.bounds, 0, POPOVER_PADDING)];

    CGFloat document_width = self.scroll_view.contentSize.width;
    [self.table_view setFrame:NSMakeRect(0, 0, document_width, visible_table_height)];

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
    notifyObserver:(BOOL)notify_observer
{
    if (![self isSelectableRow:row])
        return;

    [self.table_view selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    [self.table_view scrollRowToVisible:[self.table_view selectedRow]];

    if (notify_observer) {
        if (auto suggestion = [self selectedSuggestion]; suggestion.has_value())
            [self.observer onHighlightedSuggestion:suggestion.release_value()];
    }
}

#pragma mark - NSTableViewDataSource

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
    return static_cast<NSInteger>(m_rows.size());
}

#pragma mark - NSTableViewDelegate

- (CGFloat)tableView:(NSTableView*)tableView heightOfRow:(NSInteger)row
{
    return [self heightOfRowAtIndex:static_cast<size_t>(row)];
}

- (NSTableRowView*)tableView:(NSTableView*)tableView
               rowViewForRow:(NSInteger)row
{
    return [[AutocompleteRowView alloc] initWithFrame:NSMakeRect(0, 0, NSWidth(tableView.bounds), [self tableView:tableView heightOfRow:row])];
}

- (NSView*)tableView:(NSTableView*)table_view
    viewForTableColumn:(NSTableColumn*)table_column
                   row:(NSInteger)row
{
    auto const& row_model = m_rows[row];
    if (row_model.kind == AutocompleteRowKind::SectionHeader) {
        AutocompleteSectionHeaderView* view = (AutocompleteSectionHeaderView*)[table_view makeViewWithIdentifier:AUTOCOMPLETE_SECTION_HEADER_IDENTIFIER owner:self];

        if (view == nil) {
            view = [[AutocompleteSectionHeaderView alloc] initWithFrame:NSZeroRect];

            NSTextField* text_field = [[NSTextField alloc] initWithFrame:NSZeroRect];
            [text_field setBezeled:NO];
            [text_field setDrawsBackground:NO];
            [text_field setEditable:NO];
            [text_field setFont:autocomplete_section_header_font()];
            [text_field setSelectable:NO];
            [view addSubview:text_field];
            [view setText_field:text_field];
            [view setIdentifier:AUTOCOMPLETE_SECTION_HEADER_IDENTIFIER];
        }

        auto* header_text = Ladybird::string_to_ns_string(row_model.text);
        auto header_height = autocomplete_text_field_height(autocomplete_section_header_font());
        [view setFrame:NSMakeRect(0, 0, NSWidth(table_view.bounds), [self tableView:table_view heightOfRow:row])];
        [view.text_field setStringValue:header_text];
        [view.text_field setTextColor:[NSColor tertiaryLabelColor]];
        [view.text_field setFrame:NSMakeRect(
                                      SECTION_HEADER_HORIZONTAL_PADDING,
                                      floor((NSHeight(view.bounds) - header_height) / 2.f),
                                      NSWidth(view.bounds) - (SECTION_HEADER_HORIZONTAL_PADDING * 2),
                                      header_height)];
        return view;
    }

    AutocompleteSuggestionView* view = (AutocompleteSuggestionView*)[table_view makeViewWithIdentifier:AUTOCOMPLETE_IDENTIFIER owner:self];

    if (view == nil) {
        view = [[AutocompleteSuggestionView alloc] initWithFrame:NSZeroRect];

        NSImageView* icon_view = [[NSImageView alloc] initWithFrame:NSZeroRect];
        [icon_view setImageScaling:NSImageScaleProportionallyDown];
        [view addSubview:icon_view];
        [view setIcon_view:icon_view];

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

    auto const& suggestion = m_suggestions[row_model.suggestion_index];
    auto* suggestion_text = Ladybird::string_to_ns_string(suggestion.text);
    auto* title_text = suggestion.title.has_value() ? Ladybird::string_to_ns_string(*suggestion.title) : nil;
    auto* secondary_text = suggestion.subtitle.has_value() ? Ladybird::string_to_ns_string(*suggestion.subtitle) : suggestion_text;
    auto* favicon = [self.suggestion_icons objectForKey:suggestion_text];
    auto* icon = suggestion.source == WebView::AutocompleteSuggestionSource::LiteralURL
        ? literal_url_suggestion_icon()
        : suggestion.source == WebView::AutocompleteSuggestionSource::Search ? search_suggestion_icon()
                                                                             : favicon;

    [view setFrame:NSMakeRect(0, 0, NSWidth(table_view.bounds), [self tableView:table_view heightOfRow:row])];

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
    [view.icon_view setContentTintColor:suggestion.source != WebView::AutocompleteSuggestionSource::History ? [NSColor secondaryLabelColor] : nil];
    [view.icon_view setHidden:(icon == nil)];

    if (title_text != nil) {
        CGFloat text_block_height = primary_text_height + CELL_LABEL_VERTICAL_SPACING + secondary_text_height;
        CGFloat text_block_origin_y = floor((NSHeight(view.bounds) - text_block_height) / 2.f);

        [view.title_text_field setHidden:NO];
        [view.title_text_field setStringValue:title_text];
        [view.title_text_field setTextColor:[NSColor textColor]];
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

        [view.url_text_field setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
        [view.url_text_field setTextColor:[NSColor textColor]];
        [view.url_text_field setFrame:NSMakeRect(
                                          text_origin_x,
                                          floor((NSHeight(view.bounds) - primary_text_height) / 2.f),
                                          text_width,
                                          primary_text_height)];
    }

    [view.url_text_field setStringValue:(title_text != nil ? secondary_text : suggestion_text)];
    return view;
}

@end
