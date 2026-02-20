/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <LibWebView/Autocomplete.h>

#import <Interface/Autocomplete.h>
#import <Utilities/Conversions.h>

static NSString* const AUTOCOMPLETE_IDENTIFIER = @"Autocomplete";
static constexpr auto MAX_NUMBER_OF_ROWS = 8uz;
static constexpr auto POPOVER_PADDING = 6uz;

static bool is_likely_host(StringView candidate)
{
    return candidate.contains('.') || candidate.contains(':') || candidate.equals_ignoring_ascii_case("localhost"sv);
}

struct SuggestionParts {
    String prefix;
    String host;
    String suffix;
    bool has_host { false };
};

static SuggestionParts split_suggestion(String const& suggestion)
{
    auto suggestion_view = suggestion.bytes_as_string_view();
    auto scheme_separator_index = suggestion_view.find("://"sv);

    size_t host_start = 0;
    String prefix;

    if (scheme_separator_index.has_value()) {
        host_start = *scheme_separator_index + 3;
        prefix = MUST(String::from_utf8(suggestion_view.substring_view(0, host_start)));
    }

    auto host_end = suggestion_view.find('/', host_start).value_or(suggestion_view.length());
    if (host_end <= host_start)
        return {};

    auto host = MUST(String::from_utf8(suggestion_view.substring_view(host_start, host_end - host_start)));
    if (!is_likely_host(host.bytes_as_string_view()))
        return {};

    return {
        .prefix = move(prefix),
        .host = move(host),
        .suffix = MUST(String::from_utf8(suggestion_view.substring_view(host_end))),
        .has_host = true,
    };
}

static bool is_rebuild_placeholder(StringView suggestion)
{
    return suggestion == WebView::Autocomplete::local_index_rebuild_placeholder_text();
}

static Optional<String> normalized_display_title(Optional<String> const& title)
{
    if (!title.has_value())
        return {};

    auto trimmed_title = title->bytes_as_string_view().trim_whitespace();
    if (trimmed_title.is_empty())
        return {};

    return MUST(String::from_utf8(trimmed_title));
}

static NSAttributedString* create_attributed_suggestion(WebView::AutocompleteSuggestion const& suggestion)
{
    if (is_rebuild_placeholder(suggestion.text.bytes_as_string_view())) {
        auto font_size = [NSFont systemFontSize];
        auto* italic_attributes = @{
            NSFontAttributeName : [NSFont systemFontOfSize:font_size],
            NSObliquenessAttributeName : @0.25,
        };
        return [[NSAttributedString alloc] initWithString:Ladybird::string_to_ns_string(suggestion.text)
                                               attributes:italic_attributes];
    }

    auto parts = split_suggestion(suggestion.text);
    if (!parts.has_host) {
        auto font_size = [NSFont systemFontSize];
        auto* normal_attributes = @ {
            NSFontAttributeName : [NSFont systemFontOfSize:font_size],
        };
        auto* attributed = [[NSMutableAttributedString alloc] initWithString:Ladybird::string_to_ns_string(suggestion.text)
                                                                  attributes:normal_attributes];

        auto title = normalized_display_title(suggestion.title);
        if (title.has_value()) {
            auto* secondary_attributes = @ {
                NSFontAttributeName : [NSFont systemFontOfSize:font_size],
                NSForegroundColorAttributeName : [NSColor systemGrayColor],
            };
            [attributed appendAttributedString:[[NSAttributedString alloc] initWithString:@" \u2014 "
                                                                               attributes:secondary_attributes]];
            [attributed appendAttributedString:[[NSAttributedString alloc] initWithString:Ladybird::string_to_ns_string(*title)
                                                                               attributes:secondary_attributes]];
        }

        return attributed;
    }

    auto font_size = [NSFont systemFontSize];
    auto* normal_attributes = @ {
        NSFontAttributeName : [NSFont systemFontOfSize:font_size],
    };
    auto* bold_attributes = @ {
        NSFontAttributeName : [NSFont boldSystemFontOfSize:font_size],
    };

    auto* attributed = [[NSMutableAttributedString alloc] init];
    [attributed appendAttributedString:[[NSAttributedString alloc] initWithString:Ladybird::string_to_ns_string(parts.prefix)
                                                                       attributes:normal_attributes]];
    [attributed appendAttributedString:[[NSAttributedString alloc] initWithString:Ladybird::string_to_ns_string(parts.host)
                                                                       attributes:bold_attributes]];
    [attributed appendAttributedString:[[NSAttributedString alloc] initWithString:Ladybird::string_to_ns_string(parts.suffix)
                                                                       attributes:normal_attributes]];

    auto title = normalized_display_title(suggestion.title);
    if (title.has_value()) {
        auto* secondary_attributes = @ {
            NSFontAttributeName : [NSFont systemFontOfSize:font_size],
            NSForegroundColorAttributeName : [NSColor systemGrayColor],
        };
        [attributed appendAttributedString:[[NSAttributedString alloc] initWithString:@" \u2014 "
                                                                           attributes:secondary_attributes]];
        [attributed appendAttributedString:[[NSAttributedString alloc] initWithString:Ladybird::string_to_ns_string(*title)
                                                                           attributes:secondary_attributes]];
    }

    return attributed;
}

@interface Autocomplete () <NSTableViewDataSource, NSTableViewDelegate>
{
    Vector<WebView::AutocompleteSuggestion> m_suggestions;
}

@property (nonatomic, weak) id<AutocompleteObserver> observer;
@property (nonatomic, weak) NSToolbarItem* toolbar_item;

@property (nonatomic, strong) NSTableView* table_view;

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

        self.table_view = [[NSTableView alloc] init];
        [self.table_view setAction:@selector(selectSuggestion:)];
        [self.table_view setBackgroundColor:[NSColor clearColor]];
        [self.table_view setIntercellSpacing:NSMakeSize(0, 5)];
        [self.table_view setHeaderView:nil];
        [self.table_view setRefusesFirstResponder:YES];
        [self.table_view setRowSizeStyle:NSTableViewRowSizeStyleDefault];
        [self.table_view addTableColumn:column];
        [self.table_view setDataSource:self];
        [self.table_view setDelegate:self];
        [self.table_view setTarget:self];

        auto* scroll_view = [[NSScrollView alloc] init];
        [scroll_view setHasVerticalScroller:YES];
        [scroll_view setDocumentView:self.table_view];
        [scroll_view setDrawsBackground:NO];

        auto* content_view = [[NSView alloc] init];
        [content_view addSubview:scroll_view];

        auto* controller = [[NSViewController alloc] init];
        [controller setView:content_view];

        [self setAnimates:NO];
        [self setBehavior:NSPopoverBehaviorTransient];
        [self setContentViewController:controller];
        [self setValue:[NSNumber numberWithBool:YES] forKeyPath:@"shouldHideAnchor"];
    }

    return self;
}

#pragma mark - Public methods

- (void)showWithSuggestions:(Vector<WebView::AutocompleteSuggestion>)suggestions
{
    m_suggestions = move(suggestions);
    [self.table_view reloadData];

    if (m_suggestions.is_empty()) {
        [self close];
    } else {
        [self show];
    }
}

- (BOOL)close
{
    if (!self.isShown)
        return NO;

    [super close];
    return YES;
}

- (Optional<String>)selectedSuggestion
{
    if (!self.isShown || self.table_view.numberOfRows == 0)
        return {};

    auto row = [self.table_view selectedRow];
    if (row < 0)
        return {};

    return m_suggestions[row].text;
}

- (BOOL)selectNextSuggestion
{
    if (self.table_view.numberOfRows == 0)
        return NO;

    if (!self.isShown) {
        [self show];
        return YES;
    }

    [self selectRow:[self.table_view selectedRow] + 1];
    return YES;
}

- (BOOL)selectPreviousSuggestion
{
    if (self.table_view.numberOfRows == 0)
        return NO;

    if (!self.isShown) {
        [self show];
        return YES;
    }

    [self selectRow:[self.table_view selectedRow] - 1];
    return YES;
}

- (void)selectSuggestion:(id)sender
{
    if (auto suggestion = [self selectedSuggestion]; suggestion.has_value())
        [self.observer onSelectedSuggestion:suggestion.release_value()];
}

#pragma mark - Private methods

- (void)show
{
    auto height = (self.table_view.rowHeight + self.table_view.intercellSpacing.height) * min(self.table_view.numberOfRows, MAX_NUMBER_OF_ROWS);
    auto frame = NSMakeRect(0, 0, [[self.toolbar_item view] frame].size.width, height);

    [self.table_view.enclosingScrollView setFrame:NSInsetRect(frame, 0, POPOVER_PADDING)];
    [self setContentSize:frame.size];

    [self.table_view deselectAll:nil];
    [self.table_view scrollRowToVisible:0];

    [self showRelativeToToolbarItem:self.toolbar_item];

    auto* window = [self.toolbar_item.view window];
    auto* first_responder = [window firstResponder];

    [self showRelativeToRect:self.toolbar_item.view.frame
                      ofView:self.toolbar_item.view
               preferredEdge:NSRectEdgeMaxY];

    if (first_responder)
        [window makeFirstResponder:first_responder];
}

- (void)selectRow:(NSInteger)row
{
    if (row < 0)
        row = self.table_view.numberOfRows - 1;
    else if (row >= self.table_view.numberOfRows)
        row = 0;

    [self.table_view selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    [self.table_view scrollRowToVisible:[self.table_view selectedRow]];
}

#pragma mark - NSTableViewDataSource

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
    return static_cast<NSInteger>(m_suggestions.size());
}

#pragma mark - NSTableViewDelegate

- (NSView*)tableView:(NSTableView*)table_view
    viewForTableColumn:(NSTableColumn*)table_column
                   row:(NSInteger)row
{
    NSTableCellView* view = [table_view makeViewWithIdentifier:AUTOCOMPLETE_IDENTIFIER owner:self];

    if (view == nil) {
        view = [[NSTableCellView alloc] initWithFrame:NSZeroRect];

        NSTextField* text_field = [[NSTextField alloc] initWithFrame:NSZeroRect];
        [text_field setBezeled:NO];
        [text_field setDrawsBackground:NO];
        [text_field setEditable:NO];
        [text_field setSelectable:NO];
        [text_field setLineBreakMode:NSLineBreakByTruncatingTail];
        [text_field setUsesSingleLineMode:YES];

        [view addSubview:text_field];
        [view setTextField:text_field];
        [view setIdentifier:AUTOCOMPLETE_IDENTIFIER];
    }

    [view.textField setAttributedStringValue:create_attributed_suggestion(m_suggestions[row])];
    return view;
}

@end
