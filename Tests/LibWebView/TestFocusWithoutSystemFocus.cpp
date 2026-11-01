/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/Utf16String.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

// Focusing the area that already has focus does nothing, whether or not the page has system focus. A page whose focus
// handler focuses its own target again must not recurse when the window is in the background.
TEST_CASE(focus_already_focused_area_without_system_focus)
{
    Test::HeadlessTestEnvironment environment { "TestFocusWithoutSystemFocus"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    Optional<Utf16String> title;
    view->on_title_change = [&](Utf16String const& new_title) { title = new_title; };

    // Each focus handler focuses its own target again, at most once more, so a target that fires focus again for a
    // focus() on itself counts two events.
    view->load_html(R"html(<!DOCTYPE html>
<div id="first" tabindex="0">first</div>
<div id="second" tabindex="0">second</div>
<iframe id="frame" srcdoc="<div id=inner tabindex=0>inner</div>"></iframe>
<div id="host"></div>
<div id="foreground_host"></div>
<div id="background_host"></div>
<script>
    const counts = {};
    function count_and_refocus(name, target, refocus) {
        target.addEventListener("focus", () => {
            counts[name] = (counts[name] ?? 0) + 1;
            if (counts[name] < 2)
                refocus();
        });
    }
    const frame = document.getElementById("frame");
    let frames_loaded = 0;
    frame.onload = () => {
        const inner = frame.contentDocument.getElementById("inner");
        count_and_refocus("inner", inner, () => inner.focus());
        count_and_refocus("window", frame.contentWindow, () => frame.contentWindow.focus());
        ++frames_loaded;
    };
    // A shadow host that delegates focus, with an iframe in its shadow tree.
    for (const id of ["foreground_host", "background_host"]) {
        const root = document.getElementById(id).attachShadow({ mode: "open", delegatesFocus: true });
        root.innerHTML = "<input id=outer><iframe srcdoc='<input id=deep>'></iframe>";
        root.querySelector("iframe").onload = () => ++frames_loaded;
    }
    function focus_deep_then_host(id) {
        const host = document.getElementById(id);
        host.shadowRoot.querySelector("iframe").contentDocument.getElementById("deep").focus();
        host.focus();
        const active = host.shadowRoot.activeElement;
        return active ? active.id || active.localName : "nothing";
    }
    const host = document.getElementById("host");
    host.attachShadow({ mode: "open", delegatesFocus: true }).innerHTML = "<input id=one><input id=two>";
    function focused_in_host() {
        return host.shadowRoot.activeElement?.id ?? "nothing";
    }
    for (const name of ["first", "second"]) {
        const target = document.getElementById(name);
        count_and_refocus(name, target, () => target.focus());
    }
    function focus_and_report(focus) {
        for (const name in counts)
            delete counts[name];
        focus();
        const fired = Object.entries(counts).map(([name, count]) => `${name}:${count}`).sort().join(" ");
        document.title = `${fired || "none"} ${document.hasFocus()}`;
    }
    function report_when_frame_ready() {
        if (frames_loaded === 3)
            document.title = "frame ready";
        else
            setTimeout(report_when_frame_ready, 10);
    }
    report_when_frame_ready();
</script>)html"sv);
    Core::EventLoop::current().spin_until([&]() { return title == "frame ready"sv; });

    auto focus_and_report = [&](StringView focus) {
        title.clear();
        view->run_javascript(MUST(String::formatted("focus_and_report(() => {})", focus)));
        Core::EventLoop::current().spin_until([&]() { return title.has_value(); });
        return title->to_utf8();
    };

    auto run_and_report = [&](StringView script) {
        title.clear();
        view->run_javascript(MUST(String::from_utf8(script)));
        Core::EventLoop::current().spin_until([&]() { return title.has_value(); });
        return title->to_utf8();
    };

    // A shadow host that delegates focus picks the same delegate without system focus as with it, when focus is in an
    // iframe in its shadow tree.
    auto foreground_host_delegate = run_and_report("document.title = 'foreground ' + focus_deep_then_host('foreground_host')"sv);

    // With system focus, a target gets focus once, and its handler's focus() does nothing.
    VERIFY(focus_and_report("first.focus()"sv) == "first:1 true"sv);

    view->set_has_system_focus(false);

    // Without system focus, focusing the target that keeps focus fires nothing.
    VERIFY(focus_and_report("first.focus()"sv) == "none false"sv);

    // Moving focus to another target fires its focus event once, and its handler's focus() does nothing.
    VERIFY(focus_and_report("second.focus()"sv) == "second:1 false"sv);
    VERIFY(focus_and_report("second.focus()"sv) == "none false"sv);

    // The same holds in a same-process iframe, whose window gets focus along with the element.
    VERIFY(focus_and_report("frame.contentDocument.getElementById('inner').focus()"sv) == "inner:1 window:1 false"sv);
    VERIFY(focus_and_report("frame.contentDocument.getElementById('inner').focus()"sv) == "none false"sv);

    // And for the iframe's viewport: Moving focus to it fires its window's focus once, and focusing it again fires nothing.
    VERIFY(focus_and_report("frame.contentWindow.focus()"sv) == "window:1 false"sv);
    VERIFY(focus_and_report("frame.contentWindow.focus()"sv) == "none false"sv);

    // A shadow host that delegates focus keeps the delegate that has focus.
    size_t host_step = 0;
    auto focused_in_host_after = [&](StringView focus) {
        title.clear();
        ++host_step;
        view->run_javascript(MUST(String::formatted("{}; document.title = '{}: ' + focused_in_host()", focus, host_step)));
        Core::EventLoop::current().spin_until([&]() { return title.has_value(); });
        return title->to_utf8();
    };
    VERIFY(focused_in_host_after("host.shadowRoot.getElementById('two').focus()"sv) == "1: two"sv);
    VERIFY(focused_in_host_after("host.focus()"sv) == "2: two"sv);

    auto background_host_delegate = run_and_report("document.title = 'background ' + focus_deep_then_host('background_host')"sv);
    VERIFY(foreground_host_delegate.bytes_as_string_view().substring_view(11) == background_host_delegate.bytes_as_string_view().substring_view(11));

    outln("PASS: Focusing the area that keeps focus fires no focus events without system focus");
}
