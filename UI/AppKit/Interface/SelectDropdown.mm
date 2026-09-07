/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#import <Interface/SelectDropdown.h>
#import <Utilities/Conversions.h>

@interface SelectDropdown ()
{
    Function<void(Optional<u32> const&)> m_on_closed;
}

@property (nonatomic, strong, readwrite) NSMenu* menu;
@property (nonatomic, strong) NSMenuItem* chosen_item;
@property (nonatomic, assign) BOOL tracking;
@property (nonatomic, assign) BOOL suppress_close_report;

@end

@implementation SelectDropdown

- (instancetype)init
{
    if (self = [super init]) {
        self.menu = [[NSMenu alloc] initWithTitle:@"Select Dropdown"];
    }

    return self;
}

- (void)setOnClosed:(Function<void(Optional<u32> const&)>)onClosed
{
    m_on_closed = move(onClosed);
}

- (void)openWithEvent:(NSEvent*)event forView:(NSView*)view minimumWidth:(CGFloat)minimumWidth items:(Vector<Web::HTML::SelectItem> const&)items
{
    self.chosen_item = nil;
    self.suppress_close_report = NO;
    [self.menu removeAllItems];
    self.menu.minimumWidth = minimumWidth;

    auto add_menu_item = [self](Web::HTML::SelectItemOption const& item_option, bool in_option_group) {
        auto label = in_option_group ? Utf16String::formatted("    {}", item_option.label) : item_option.label;
        NSMenuItem* menuItem = [[NSMenuItem alloc]
            initWithTitle:Ladybird::utf16_string_to_ns_string(label)
                   action:item_option.disabled ? nil : @selector(itemChosen:)
            keyEquivalent:@""];
        menuItem.target = self;
        menuItem.representedObject = [NSNumber numberWithUnsignedInt:item_option.id];
        menuItem.state = item_option.selected ? NSControlStateValueOn : NSControlStateValueOff;
        [self.menu addItem:menuItem];
    };

    for (auto const& item : items) {
        if (item.has<Web::HTML::SelectItemOptionGroup>()) {
            auto const& item_option_group = item.get<Web::HTML::SelectItemOptionGroup>();
            NSMenuItem* subtitle = [[NSMenuItem alloc]
                initWithTitle:Ladybird::utf16_string_to_ns_string(item_option_group.label)
                       action:nil
                keyEquivalent:@""];
            [self.menu addItem:subtitle];

            for (auto const& item_option : item_option_group.items)
                add_menu_item(item_option, true);
        }

        if (item.has<Web::HTML::SelectItemOption>())
            add_menu_item(item.get<Web::HTML::SelectItemOption>(), false);

        if (item.has<Web::HTML::SelectItemSeparator>())
            [self.menu addItem:[NSMenuItem separatorItem]];
    }

    // NB: This returns once the menu has closed and the chosen item's action (if any) has run — so this is the one
    //     place that sees every way the menu can go away. menuDidClose: can't tell a cancel from a choice: It fires
    //     before the action, with the highlighted item still set either way.
    self.tracking = YES;
    [NSMenu popUpContextMenu:self.menu withEvent:event forView:view];
    self.tracking = NO;

    if (self.suppress_close_report) {
        self.suppress_close_report = NO;
        return;
    }

    if (!m_on_closed)
        return;

    if (self.chosen_item != nil)
        m_on_closed([[self.chosen_item representedObject] unsignedIntValue]);
    else
        m_on_closed({});
}

- (void)closeWithoutReporting
{
    if (!self.tracking)
        return;

    self.suppress_close_report = YES;
    [self.menu cancelTracking];
}

- (void)itemChosen:(NSMenuItem*)menuItem
{
    self.chosen_item = menuItem;
}

@end
