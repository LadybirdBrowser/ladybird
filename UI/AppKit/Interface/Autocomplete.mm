/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/Autocomplete.h>
#import <Utilities/Conversions.h>

static NSString* const AUTOCOMPLETE_IDENTIFIER = @"Autocomplete";
static constexpr auto MAX_NUMBER_OF_ROWS = 8uz;
static constexpr CGFloat const POPOVER_PADDING = 6;
static constexpr CGFloat const MINIMUM_WIDTH = 100;

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
    Vector<String> m_suggestions;
}

@property (nonatomic, weak) id<AutocompleteObserver> observer;
@property (nonatomic, weak) NSToolbarItem* toolbar_item;

@property (nonatomic, strong) AutocompleteWindow* popup_window;
@property (nonatomic, strong) NSView* content_view;
@property (nonatomic, strong) NSScrollView* scroll_view;
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

        self.table_view = [[NSTableView alloc] initWithFrame:NSZeroRect];
        [self.table_view setAction:@selector(selectSuggestion:)];
        [self.table_view setBackgroundColor:[NSColor clearColor]];
        [self.table_view setHeaderView:nil];
        [self.table_view setIntercellSpacing:NSMakeSize(0, 5)];
        [self.table_view setRefusesFirstResponder:YES];
        [self.table_view setRowSizeStyle:NSTableViewRowSizeStyleDefault];
        [self.table_view addTableColumn:column];
        [self.table_view setDataSource:self];
        [self.table_view setDelegate:self];
        [self.table_view setTarget:self];

        self.scroll_view = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        [self.scroll_view setBorderType:NSNoBorder];
        [self.scroll_view setDrawsBackground:NO];
        [self.scroll_view setHasVerticalScroller:YES];
        [self.scroll_view setDocumentView:self.table_view];

        self.content_view = [[NSView alloc] initWithFrame:NSZeroRect];
        [self.content_view setWantsLayer:YES];
        [self.content_view.layer setBackgroundColor:[NSColor windowBackgroundColor].CGColor];
        [self.content_view.layer setCornerRadius:8];
        [self.content_view addSubview:self.scroll_view];

        self.popup_window = [[AutocompleteWindow alloc] initWithContentRect:NSZeroRect
                                                                  styleMask:NSWindowStyleMaskBorderless
                                                                    backing:NSBackingStoreBuffered
                                                                      defer:NO];
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

- (void)showWithSuggestions:(Vector<String>)suggestions
{
    m_suggestions = move(suggestions);
    [self.table_view reloadData];

    if (m_suggestions.is_empty())
        [self close];
    else
        [self show];
}

- (BOOL)close
{
    if (!self.popup_window.isVisible)
        return NO;

    if (auto* parent_window = [self.toolbar_item.view window])
        [parent_window removeChildWindow:self.popup_window];

    [self.popup_window orderOut:nil];
    return YES;
}

- (Optional<String>)selectedSuggestion
{
    if (!self.popup_window.isVisible || self.table_view.numberOfRows == 0)
        return {};

    auto row = [self.table_view selectedRow];
    if (row < 0)
        return {};

    return m_suggestions[row];
}

- (BOOL)selectNextSuggestion
{
    if (self.table_view.numberOfRows == 0)
        return NO;

    if (!self.popup_window.isVisible) {
        [self show];
        [self selectRow:0];
        return YES;
    }

    [self selectRow:[self.table_view selectedRow] + 1];
    return YES;
}

- (BOOL)selectPreviousSuggestion
{
    if (self.table_view.numberOfRows == 0)
        return NO;

    if (!self.popup_window.isVisible) {
        [self show];
        [self selectRow:self.table_view.numberOfRows - 1];
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
    auto* toolbar_view = self.toolbar_item.view;
    auto* parent_window = [toolbar_view window];
    if (parent_window == nil)
        return;

    auto visible_row_count = min(m_suggestions.size(), MAX_NUMBER_OF_ROWS);
    auto table_height = (self.table_view.rowHeight + self.table_view.intercellSpacing.height) * visible_row_count;
    auto width = max<CGFloat>(toolbar_view.frame.size.width, MINIMUM_WIDTH);
    auto content_size = NSMakeSize(width, table_height + (POPOVER_PADDING * 2));

    [self.content_view setFrame:NSMakeRect(0, 0, content_size.width, content_size.height)];
    [self.scroll_view setFrame:NSInsetRect(self.content_view.bounds, 0, POPOVER_PADDING)];
    [self.scroll_view setHasVerticalScroller:m_suggestions.size() > MAX_NUMBER_OF_ROWS];
    [self.table_view deselectAll:nil];
    [self.table_view scrollRowToVisible:0];

    auto anchor_rect = [toolbar_view convertRect:toolbar_view.bounds toView:nil];
    auto popup_rect = [parent_window convertRectToScreen:anchor_rect];
    popup_rect.origin.y -= content_size.height;
    popup_rect.size = content_size;

    [self.popup_window setFrame:popup_rect display:NO];

    if (!self.popup_window.isVisible)
        [parent_window addChildWindow:self.popup_window ordered:NSWindowAbove];

    [self.popup_window orderFront:nil];
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

        [view addSubview:text_field];
        [view setTextField:text_field];
        [view setIdentifier:AUTOCOMPLETE_IDENTIFIER];
    }

    [view.textField setStringValue:Ladybird::string_to_ns_string(m_suggestions[row])];
    return view;
}

@end
