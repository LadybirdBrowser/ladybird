/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/LadybirdWebViewBridge.h>

#import <UI/LadybirdWebView.h>
#import <UI/SearchPanel.h>
#import <UI/Tab.h>
#import <Utilities/Conversions.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const SEARCH_FIELD_HEIGHT = 30;
static constexpr CGFloat const SEARCH_FIELD_WIDTH = 300;

@interface SearchPanel () <NSSearchFieldDelegate>
{
    CaseSensitivity m_case_sensitivity;
}

@property (nonatomic, strong) NSSearchField* search_field;
@property (nonatomic, strong) NSButton* search_match_case;
@property (nonatomic, strong) NSTextField* result_label;

@end

@implementation SearchPanel

- (instancetype)init
{
    if (self = [super init]) {
        self.search_field = [[NSSearchField alloc] init];
        [self.search_field setPlaceholderString:@"Search"];
        [self.search_field setDelegate:self];

        auto* search_previous = [NSButton buttonWithImage:[NSImage imageNamed:NSImageNameGoLeftTemplate]
                                                   target:self
                                                   action:@selector(findPreviousMatch:)];
        [search_previous setToolTip:@"Find Previous Match"];
        [search_previous setBordered:NO];

        auto* search_next = [NSButton buttonWithImage:[NSImage imageNamed:NSImageNameGoRightTemplate]
                                               target:self
                                               action:@selector(findNextMatch:)];
        [search_next setToolTip:@"Find Next Match"];
        [search_next setBordered:NO];

        self.search_match_case = [NSButton checkboxWithTitle:@"Match Case"
                                                      target:self
                                                      action:@selector(find:)];
        [self.search_match_case setState:NSControlStateValueOff];
        m_case_sensitivity = CaseSensitivity::CaseInsensitive;

        self.result_label = [NSTextField labelWithString:@""];
        [self.result_label setHidden:YES];

        auto* search_done = [NSButton buttonWithTitle:@"Done"
                                               target:self
                                               action:@selector(cancelSearch:)];
        [search_done setToolTip:@"Close Search Bar"];
        [search_done setBezelStyle:NSBezelStyleAccessoryBarAction];

        [self addView:self.search_field inGravity:NSStackViewGravityLeading];
        [self addView:search_previous inGravity:NSStackViewGravityLeading];
        [self addView:search_next inGravity:NSStackViewGravityLeading];
        [self addView:self.search_match_case inGravity:NSStackViewGravityLeading];
        [self addView:self.result_label inGravity:NSStackViewGravityLeading];
        [self addView:search_done inGravity:NSStackViewGravityTrailing];

        [self setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
        [self setEdgeInsets:NSEdgeInsets { 0, 8, 0, 8 }];

        [[self heightAnchor] constraintEqualToConstant:SEARCH_FIELD_HEIGHT].active = YES;
        [[self.search_field widthAnchor] constraintEqualToConstant:SEARCH_FIELD_WIDTH].active = YES;
    }

    return self;
}

#pragma mark - Public methods

- (void)find:(id)sender
{
    [self setHidden:NO];
    [self setSearchTextFromPasteBoard];

    [self.window makeFirstResponder:self.search_field];
}

- (void)findNextMatch:(id)sender
{
    if ([self setSearchTextFromPasteBoard]) {
        return;
    }

    [[[self tab] web_view] findInPageNextMatch];
}

- (void)findPreviousMatch:(id)sender
{
    if ([self setSearchTextFromPasteBoard]) {
        return;
    }

    [[[self tab] web_view] findInPagePreviousMatch];
}

- (void)useSelectionForFind:(id)sender
{
    auto selected_text = [[[self tab] web_view] view].selected_text();
    auto* query = Ladybird::string_to_ns_string(selected_text);

    [self setPasteBoardContents:query];

    if (![self isHidden]) {
        [self.search_field setStringValue:query];
        [[[self tab] web_view] findInPage:query caseSensitivity:m_case_sensitivity];

        [self.window makeFirstResponder:self.search_field];
    }
}

- (void)onFindInPageResult:(size_t)current_match_index
           totalMatchCount:(Optional<size_t> const&)total_match_count
{
    if (total_match_count.has_value()) {
        auto* label_text = *total_match_count > 0
            ? [NSString stringWithFormat:@"%zu of %zu matches", current_match_index, *total_match_count]
            : @"Phrase not found";

        auto* label_attributes = @{
            NSFontAttributeName : [NSFont boldSystemFontOfSize:12.0f],
        };

        auto* label_attribute = [[NSAttributedString alloc] initWithString:label_text
                                                                attributes:label_attributes];

        [self.result_label setAttributedStringValue:label_attribute];
        [self.result_label setHidden:NO];
    } else {
        [self.result_label setHidden:YES];
    }
}

#pragma mark - Private methods

- (Tab*)tab
{
    return (Tab*)[self window];
}

- (void)setPasteBoardContents:(NSString*)query
{
    auto* paste_board = [NSPasteboard pasteboardWithName:NSPasteboardNameFind];
    [paste_board clearContents];
    [paste_board setString:query forType:NSPasteboardTypeString];
}

- (BOOL)setSearchTextFromPasteBoard
{
    auto* paste_board = [NSPasteboard pasteboardWithName:NSPasteboardNameFind];
    auto* query = [paste_board stringForType:NSPasteboardTypeString];

    if (query) {
        auto case_sensitivity = [self.search_match_case state] == NSControlStateValueOff
            ? CaseSensitivity::CaseInsensitive
            : CaseSensitivity::CaseSensitive;

        if (case_sensitivity != m_case_sensitivity || ![[self.search_field stringValue] isEqual:query]) {
            [self.search_field setStringValue:query];
            m_case_sensitivity = case_sensitivity;

            [[[self tab] web_view] findInPage:query caseSensitivity:m_case_sensitivity];
            return YES;
        }
    }

    return NO;
}

- (void)cancelSearch:(id)sender
{
    [self setHidden:YES];
}

#pragma mark - NSSearchFieldDelegate

- (void)controlTextDidChange:(NSNotification*)notification
{
    auto* query = [self.search_field stringValue];
    [[[self tab] web_view] findInPage:query caseSensitivity:m_case_sensitivity];

    [self setPasteBoardContents:query];
}

- (BOOL)control:(NSControl*)control
               textView:(NSTextView*)text_view
    doCommandBySelector:(SEL)selector
{
    if (selector == @selector(insertNewline:)) {
        NSEvent* event = [[self tab] currentEvent];

        if ((event.modifierFlags & NSEventModifierFlagShift) == 0) {
            [self findNextMatch:nil];
        } else {
            [self findPreviousMatch:nil];
        }

        return YES;
    }

    if (selector == @selector(cancelOperation:)) {
        [self cancelSearch:nil];
        return YES;
    }

    return NO;
}

@end
