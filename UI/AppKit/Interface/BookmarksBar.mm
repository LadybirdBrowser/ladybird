/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>

#import <Interface/BookmarksBar.h>
#import <Interface/Menu.h>
#import <Utilities/Conversions.h>
#import <objc/runtime.h>

#if !__has_feature(objc_arc)
#    error "This project requires ARC"
#endif

static constexpr CGFloat const BOOKMARK_BUTTON_MAX_WIDTH = 150;
static constexpr CGFloat const BOOKMARK_ITEM_SPACING = 2;
static constexpr CGFloat const BOOKMARK_LEADING_INSET = 8;
static constexpr CGFloat const OVERFLOW_TRAILING_INSET = 4;

static char BOOKMARK_FOLDER_KEY = 0;

@interface BookmarksBar ()

@property (nonatomic, strong) NSStackView* bookmark_items;
@property (nonatomic, strong) NSButton* overflow_button;
@property (nonatomic, strong) NSMenu* overflow_menu;

@end

@implementation BookmarksBar

@synthesize overflow_menu = _overflow_menu;

- (instancetype)init
{
    if (self = [super init]) {
        self.bookmark_items = [[NSStackView alloc] init];
        [self.bookmark_items setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
        [self.bookmark_items setSpacing:BOOKMARK_ITEM_SPACING];
        [self.bookmark_items setEdgeInsets:NSEdgeInsets { 0, BOOKMARK_LEADING_INSET, 0, 0 }];
        [self.bookmark_items setAlignment:NSLayoutAttributeCenterY];
        [self.bookmark_items setTranslatesAutoresizingMaskIntoConstraints:NO];

        [self.bookmark_items setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                                      forOrientation:NSLayoutConstraintOrientationHorizontal];
        [self.bookmark_items setClippingResistancePriority:NSLayoutPriorityDefaultLow
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];

        self.overflow_button = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"chevron.right" accessibilityDescription:@""]
                                                  target:self
                                                  action:@selector(openOverflowMenu:)];
        [self.overflow_button setBezelStyle:NSBezelStyleAccessoryBarAction];
        [self.overflow_button setShowsBorderOnlyWhileMouseInside:YES];
        [self.overflow_button setTranslatesAutoresizingMaskIntoConstraints:NO];
        [self.overflow_button setHidden:YES];

        [self.overflow_button setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                                       forOrientation:NSLayoutConstraintOrientationHorizontal];

        [self addSubview:self.bookmark_items];
        [self addSubview:self.overflow_button];

        [NSLayoutConstraint activateConstraints:@[
            [[self.bookmark_items leadingAnchor] constraintEqualToAnchor:[self leadingAnchor]],
            [[self.bookmark_items topAnchor] constraintEqualToAnchor:[self topAnchor]],
            [[self.bookmark_items bottomAnchor] constraintEqualToAnchor:[self bottomAnchor]],

            [[self.overflow_button trailingAnchor] constraintEqualToAnchor:[self trailingAnchor]
                                                                  constant:-OVERFLOW_TRAILING_INSET],
            [[self.overflow_button centerYAnchor] constraintEqualToAnchor:[self centerYAnchor]],

            [[self.bookmark_items trailingAnchor] constraintLessThanOrEqualToAnchor:[self.overflow_button leadingAnchor]
                                                                           constant:-BOOKMARK_ITEM_SPACING],
        ]];

        [self setClipsToBounds:YES];

        [self rebuild];
    }

    return self;
}

- (void)rebuild
{
    [self.bookmark_items setSubviews:@[]];

    auto set_button_properties = [](NSButton* button, StringView title) {
        [button setTitle:Ladybird::string_to_ns_string(title)];
        [button setImagePosition:NSImageLeading];

        [button setBezelStyle:NSBezelStyleAccessoryBarAction];
        [button setShowsBorderOnlyWhileMouseInside:YES];

        [button setFont:[NSFont systemFontOfSize:12]];
        [button setControlSize:NSControlSizeRegular];

        [[button cell] setLineBreakMode:NSLineBreakByTruncatingTail];
        [[button widthAnchor] constraintLessThanOrEqualToConstant:BOOKMARK_BUTTON_MAX_WIDTH].active = YES;
    };

    for (auto const& item : WebView::Application::the().bookmarks_menu().items()) {
        auto* button = item.visit(
            [&](NonnullRefPtr<WebView::Action> const& bookmark) -> NSButton* {
                if (bookmark->id() != WebView::ActionID::BookmarkItem)
                    return nil;

                auto* button = Ladybird::create_application_button(bookmark);
                set_button_properties(button, bookmark->text());

                return button;
            },
            [&](NonnullRefPtr<WebView::Menu> const& folder) -> NSButton* {
                auto* button = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"folder" accessibilityDescription:@""]
                                                  target:self
                                                  action:@selector(openFolder:)];
                set_button_properties(button, folder->title());

                auto* submenu = Ladybird::create_application_menu(folder);
                objc_setAssociatedObject(button, &BOOKMARK_FOLDER_KEY, submenu, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

                return button;
            },
            [](WebView::Separator) -> NSButton* {
                return nil;
            });

        if (button) {
            [self.bookmark_items addView:button inGravity:NSStackViewGravityLeading];
        }
    }

    [self setNeedsLayout:YES];
}

- (NSMenu*)overflow_menu
{
    if (!_overflow_menu) {
        _overflow_menu = [[NSMenu alloc] init];

        NSArray<NSView*>* buttons = [self.bookmark_items views];
        size_t button_index = 0;

        for (auto const& item : WebView::Application::the().bookmarks_menu().items()) {
            auto is_bookmark_item = item.visit(
                [](NonnullRefPtr<WebView::Action> const& action) { return action->id() == WebView::ActionID::BookmarkItem; },
                [](NonnullRefPtr<WebView::Menu> const&) { return true; },
                [](WebView::Separator) { return false; });

            if (!is_bookmark_item)
                continue;

            if (button_index < [buttons count] && [buttons[button_index] isHidden]) {
                item.visit(
                    [&](NonnullRefPtr<WebView::Action> const& action) {
                        [_overflow_menu addItem:Ladybird::create_application_menu_item(action)];
                    },
                    [&](NonnullRefPtr<WebView::Menu> const& folder) {
                        auto* folder_item = [[NSMenuItem alloc] initWithTitle:Ladybird::string_to_ns_string(folder->title())
                                                                       action:nil
                                                                keyEquivalent:@""];

                        auto* submenu = Ladybird::create_application_menu(folder);
                        [folder_item setSubmenu:submenu];

                        [_overflow_menu addItem:folder_item];
                    },
                    [](WebView::Separator) {});
            }

            ++button_index;
        }
    }

    return _overflow_menu;
}

- (void)openOverflowMenu:(NSButton*)sender
{
    if ([self.overflow_menu numberOfItems] > 0) {
        [self.overflow_menu popUpMenuPositioningItem:nil
                                          atLocation:NSMakePoint(0, [sender bounds].size.height)
                                              inView:sender];
    }
}

- (void)openFolder:(NSButton*)sender
{
    NSMenu* folder = objc_getAssociatedObject(sender, &BOOKMARK_FOLDER_KEY);
    if (!folder)
        return;

    [folder popUpMenuPositioningItem:nil
                          atLocation:NSMakePoint(0, [sender bounds].size.height)
                              inView:sender];
}

- (void)layout
{
    [super layout];

    auto overflow_width = [self.overflow_button fittingSize].width + OVERFLOW_TRAILING_INSET + BOOKMARK_ITEM_SPACING;
    auto total_width = [self bounds].size.width;

    // First pass: check if any buttons overflow without reserving space for the overflow button.
    auto used_width = BOOKMARK_LEADING_INSET;
    for (NSView* button in [self.bookmark_items views]) {
        used_width += [button fittingSize].width + BOOKMARK_ITEM_SPACING;
    }

    auto has_overflow = used_width > total_width;
    auto available_width = has_overflow ? total_width - overflow_width : total_width;

    // Second pass: hide buttons that don't fully fit with overflow button.
    used_width = BOOKMARK_LEADING_INSET;
    for (NSView* button in [self.bookmark_items views]) {
        used_width += [button fittingSize].width + BOOKMARK_ITEM_SPACING;
        [button setHidden:(used_width > available_width)];
    }

    [self.overflow_button setHidden:!has_overflow];
    self.overflow_menu = nil;
}

@end
