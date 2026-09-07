/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibTest/TestCase.h>

#import <Interface/SelectDropdown.h>

// These tests pop up a real menu and drive it with key events posted to the application's event queue. The menu's
// tracking loop takes those like hardware ones — with the application inactive, the window never shown, and no
// application run loop, as under ctest. But a tracking menu also follows the real pointer, whichever application is
// active: a move clears the menu's keyboard highlight, and a click dismisses the menu. So a run whose highlight or
// menu the pointer took away is run again, up to three times in all, rather than failed on the spot.

namespace {

struct Report {
    int calls { 0 };
    Optional<u32> selected_item_id;
};

// The state of one run of steps against one menu.
struct Run {
    size_t next_step { 0 };
    bool lost_to_the_pointer { false };
};

// A step runs on every tick, 50ms apart, until it returns true.
using Step = Function<bool()>;

void record_reports(SelectDropdown* dropdown, Report& report)
{
    [dropdown setOnClosed:[&report](Optional<u32> const& selected_item_id) {
        ++report.calls;
        report.selected_item_id = selected_item_id;
    }];
}

Vector<Web::HTML::SelectItem> three_options()
{
    Vector<Web::HTML::SelectItem> items;
    items.append(Web::HTML::SelectItemOption { .id = 1, .selected = true, .label = "Alpha"_utf16 });
    items.append(Web::HTML::SelectItemOption { .id = 2, .label = "Bravo"_utf16 });
    items.append(Web::HTML::SelectItemOption { .id = 3, .label = "Charlie"_utf16 });
    return items;
}

// The window the menu pops up for. It's never shown; the menu doesn't need it to be. The test binary has no
// application object of its own, and the menu's tracking loop and the posted key events need one.
NSWindow* make_window()
{
    [NSApplication sharedApplication];
    auto* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                               styleMask:NSWindowStyleMaskTitled
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    window.releasedWhenClosed = NO;
    return window;
}

enum class PostAt {
    Back,
    Front,
};

// A key that acts on the highlighted item goes to the front of the queue: A pointer move already queued behind it
// can't clear the highlight first.
void post_key(NSWindow* window, unsigned short key_code, NSString* characters, PostAt post_at = PostAt::Back)
{
    auto make_event = [&](NSEventType type) {
        return [NSEvent keyEventWithType:type
                                location:NSZeroPoint
                           modifierFlags:0
                               timestamp:[[NSProcessInfo processInfo] systemUptime]
                            windowNumber:window.windowNumber
                                 context:nil
                              characters:characters
             charactersIgnoringModifiers:characters
                               isARepeat:NO
                                 keyCode:key_code];
    };
    [NSApp postEvent:make_event(NSEventTypeKeyDown) atStart:post_at == PostAt::Front];
    [NSApp postEvent:make_event(NSEventTypeKeyUp) atStart:NO];
}

void press_down(NSWindow* window)
{
    post_key(window, 125, [NSString stringWithFormat:@"%C", static_cast<unichar>(NSDownArrowFunctionKey)]);
}

void press_return(NSWindow* window)
{
    post_key(window, 36, @"\r", PostAt::Front);
}

void press_escape(NSWindow* window)
{
    post_key(window, 53, @"\x1b", PostAt::Front);
}

Step close_without_reporting(SelectDropdown* dropdown)
{
    return [dropdown] {
        [dropdown closeWithoutReporting];
        return true;
    };
}

// Escape with nothing highlighted. The pointer hovering over the menu would highlight an item; the dismissal is then
// still checked, just without that precondition.
Step escape_with_nothing_highlighted(SelectDropdown* dropdown, NSWindow* window)
{
    return [=] {
        if (dropdown.menu.highlightedItem != nil)
            warnln("An item is highlighted (is the pointer over the menu?); checking the dismissal anyway");
        press_escape(window);
        return true;
    };
}

// Presses Down until an item is highlighted — the one at the index, or any item when none is given — and then runs
// `then` on that same tick, before anything else can move the highlight. The menu acts on a key well within a tick,
// so once the highlight has moved, the next press goes out; a highlight that hasn't moved in four ticks is taken as
// cleared by the pointer, and Down goes out again — the menu then starts over from the top. After two seconds of
// that, a run that wanted a particular item gives up, to be retried; one that wanted any item carries on without the
// highlight, since what it checks is the dismissal, which doesn't depend on it.
Step highlight_then(SelectDropdown* dropdown, NSWindow* window, Optional<NSInteger> index, Run& run, Function<void()> then)
{
    struct State {
        int ticks { 0 };
        int ticks_since_press { 0 };
        NSMenuItem* highlighted_at_press { nil };
    };
    auto* run_pointer = &run;
    return [=, then = move(then), state = State {}]() mutable {
        auto* highlighted = dropdown.menu.highlightedItem;
        auto* wanted = index.has_value() ? [dropdown.menu itemAtIndex:index.value()] : highlighted;
        if (highlighted != nil && highlighted == wanted) {
            then();
            return true;
        }
        if (++state.ticks > 40) {
            if (index.has_value()) {
                run_pointer->lost_to_the_pointer = true;
                [dropdown.menu cancelTracking];
            } else {
                warnln("No item stayed highlighted for two seconds (is the pointer moving?); checking the dismissal without one");
                then();
            }
            return true;
        }
        bool press_was_taken = state.ticks_since_press > 0 && highlighted != state.highlighted_at_press;
        if (state.ticks_since_press == 0 || press_was_taken || state.ticks_since_press >= 4) {
            state.highlighted_at_press = highlighted;
            press_down(window);
            state.ticks_since_press = 0;
        }
        ++state.ticks_since_press;
        return false;
    };
}

Step highlight_any_item_then_escape(SelectDropdown* dropdown, NSWindow* window, Run& run)
{
    return highlight_then(dropdown, window, {}, run, [window] { press_escape(window); });
}

Step highlight_any_item_then_cancel_tracking(SelectDropdown* dropdown, NSWindow* window, Run& run)
{
    return highlight_then(dropdown, window, {}, run, [dropdown] { [dropdown.menu cancelTracking]; });
}

Step choose_item(SelectDropdown* dropdown, NSWindow* window, NSInteger index, Run& run)
{
    return highlight_then(dropdown, window, index, run, [window] { press_return(window); });
}

// Pops the menu up for the window's content view, and runs the steps from a timer in the tracking loop's mode until
// the menu closes. Returns false when the run was lost to the pointer, or the menu went away before the steps were
// done.
bool open_and_interact(SelectDropdown* dropdown, NSWindow* window, Vector<Web::HTML::SelectItem> const& items, Vector<Step>& steps, Run& run)
{
    run = {};
    int idle_ticks = 0;
    auto tick = [&] {
        if (run.lost_to_the_pointer)
            return;
        if (run.next_step < steps.size()) {
            if (steps[run.next_step]())
                ++run.next_step;
            return;
        }
        // Every step is done and the menu is still up: Three seconds of that is a failure, not a retry.
        if (++idle_ticks == 60) {
            FAIL("The menu is still open three seconds after the last step");
            [dropdown.menu cancelTracking];
        }
    };
    auto* timer = [NSTimer timerWithTimeInterval:0.05 repeats:YES block:^(NSTimer*) { tick(); }];
    [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSEventTrackingRunLoopMode];
    [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSDefaultRunLoopMode];

    auto* event = [NSEvent mouseEventWithType:NSEventTypeRightMouseUp
                                     location:NSMakePoint(10, 10)
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:window.windowNumber
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:1.0];
    [dropdown openWithEvent:event forView:window.contentView minimumWidth:100 items:items];
    [timer invalidate];

    // A key the menu never got to would otherwise land in the next menu's tracking loop.
    [NSApp discardEventsMatchingMask:NSEventMaskAny beforeEvent:nil];

    return !run.lost_to_the_pointer && run.next_step == steps.size();
}

enum class Expecting {
    NoChoice,
    AChoice,
};

// Runs the steps against a fresh menu — and again, three attempts in all, whenever the pointer got in the way: a run
// lost to it, or a menu that closed without the expected choice because the pointer cleared the highlight just before
// Return landed.
template<typename MakeSteps>
bool run_settling(SelectDropdown* dropdown, NSWindow* window, Vector<Web::HTML::SelectItem> const& items, Report& report, Expecting expecting, MakeSteps make_steps)
{
    for (int attempt = 1; attempt <= 3; ++attempt) {
        report = {};
        Run run;
        auto steps = make_steps(run);
        bool completed = open_and_interact(dropdown, window, items, steps, run);
        if (completed && (expecting == Expecting::NoChoice || report.selected_item_id.has_value()))
            return true;
        warnln("Attempt {}: the pointer took the menu or its highlight away; trying again", attempt);
    }
    return false;
}

}

TEST_CASE(escape_with_an_item_highlighted_reports_no_choice)
{
    auto* window = make_window();
    auto* dropdown = [[SelectDropdown alloc] init];
    Report report;
    record_reports(dropdown, report);

    EXPECT(run_settling(dropdown, window, three_options(), report, Expecting::NoChoice, [&](Run& run) {
        Vector<Step> steps;
        steps.append(highlight_any_item_then_escape(dropdown, window, run));
        return steps;
    }));

    EXPECT_EQ(report.calls, 1);
    EXPECT(!report.selected_item_id.has_value());
}

TEST_CASE(escape_with_nothing_highlighted_reports_no_choice)
{
    auto* window = make_window();
    auto* dropdown = [[SelectDropdown alloc] init];
    Report report;
    record_reports(dropdown, report);

    EXPECT(run_settling(dropdown, window, three_options(), report, Expecting::NoChoice, [&](Run&) {
        Vector<Step> steps;
        steps.append(escape_with_nothing_highlighted(dropdown, window));
        return steps;
    }));

    EXPECT_EQ(report.calls, 1);
    EXPECT(!report.selected_item_id.has_value());
}

TEST_CASE(canceling_tracking_with_an_item_highlighted_reports_no_choice)
{
    auto* window = make_window();
    auto* dropdown = [[SelectDropdown alloc] init];
    Report report;
    record_reports(dropdown, report);

    // The application deactivating is one of the ways a menu goes away from the outside; cancelTracking stands in for
    // all of them.
    EXPECT(run_settling(dropdown, window, three_options(), report, Expecting::NoChoice, [&](Run& run) {
        Vector<Step> steps;
        steps.append(highlight_any_item_then_cancel_tracking(dropdown, window, run));
        return steps;
    }));

    EXPECT_EQ(report.calls, 1);
    EXPECT(!report.selected_item_id.has_value());
}

TEST_CASE(choosing_an_item_reports_its_id)
{
    auto* window = make_window();
    auto* dropdown = [[SelectDropdown alloc] init];
    Report report;
    record_reports(dropdown, report);

    EXPECT(run_settling(dropdown, window, three_options(), report, Expecting::AChoice, [&](Run& run) {
        Vector<Step> steps;
        steps.append(choose_item(dropdown, window, 2, run));
        return steps;
    }));

    EXPECT_EQ(report.calls, 1);
    EXPECT_EQ(report.selected_item_id.value_or(0), 3u);
}

TEST_CASE(choosing_an_item_inside_a_group_reports_its_id)
{
    auto* window = make_window();
    auto* dropdown = [[SelectDropdown alloc] init];
    Report report;
    record_reports(dropdown, report);

    Vector<Web::HTML::SelectItem> items;
    items.append(Web::HTML::SelectItemOption { .id = 1, .selected = true, .label = "Top"_utf16 });
    items.append(Web::HTML::SelectItemSeparator {});
    Vector<Web::HTML::SelectItemOption> grouped_options;
    grouped_options.append(Web::HTML::SelectItemOption { .id = 2, .label = "First"_utf16 });
    grouped_options.append(Web::HTML::SelectItemOption { .id = 3, .label = "Second"_utf16 });
    items.append(Web::HTML::SelectItemOptionGroup { .label = "Group"_utf16, .items = move(grouped_options) });

    // The menu holds: Top, a separator, the group's disabled title, First, Second.
    EXPECT(run_settling(dropdown, window, items, report, Expecting::AChoice, [&](Run& run) {
        Vector<Step> steps;
        steps.append(choose_item(dropdown, window, 4, run));
        return steps;
    }));

    EXPECT_EQ(static_cast<int>(dropdown.menu.numberOfItems), 5);
    EXPECT_EQ(report.calls, 1);
    EXPECT_EQ(report.selected_item_id.value_or(0), 3u);
}

TEST_CASE(closing_without_reporting_reports_nothing)
{
    auto* window = make_window();
    auto* dropdown = [[SelectDropdown alloc] init];
    Report report;
    record_reports(dropdown, report);

    EXPECT(run_settling(dropdown, window, three_options(), report, Expecting::NoChoice, [&](Run&) {
        Vector<Step> steps;
        steps.append(close_without_reporting(dropdown));
        return steps;
    }));
    EXPECT_EQ(report.calls, 0);

    // The next menu reports again as normal.
    EXPECT(run_settling(dropdown, window, three_options(), report, Expecting::NoChoice, [&](Run&) {
        Vector<Step> steps;
        steps.append([window] {
            press_escape(window);
            return true;
        });
        return steps;
    }));
    EXPECT_EQ(report.calls, 1);
    EXPECT(!report.selected_item_id.has_value());
}
