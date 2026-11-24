/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

TEST_CASE(form_validation_message)
{
    Test::HeadlessTestEnvironment environment { "TestFormValidationMessage"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
    timer->start();
    bool loaded = false;
    view->on_load_finish = [&](auto const&) { loaded = true; };
    Core::EventLoop::current().spin_until([&] { return loaded || timed_out; });
    EXPECT(loaded);
    if (!loaded)
        return;

    Vector<Utf16String> messages;
    Vector<Gfx::IntPoint> positions;
    bool finished = false;
    view->on_title_change = [&](auto const& title) { finished = title == "finished"_utf16; };
    view->on_show_validation_message = [&](auto position, Utf16String const& text) {
        positions.append(position);
        messages.append(text);
    };
    view->load_html(R"HTML(<!doctype html>
        <form><input id="email" type="email" required style="position:absolute;left:10px;top:20px;height:30px;box-sizing:border-box"></form>
        <form id="secondForm"><input id="hidesOnFocus" required><input id="nextField" required></form>
        <script>
            email.checkValidity();
            email.addEventListener("invalid", event => event.preventDefault(), { once: true });
            email.reportValidity();
            email.form.requestSubmit();
            class CustomField extends HTMLElement {
                static formAssociated = true;
                connectedCallback() {
                    this.attachInternals().setValidity({ customError: true }, "Hidden anchor", this.firstElementChild);
                }
            }
            customElements.define("custom-field", CustomField);
            email.insertAdjacentHTML("beforebegin", '<custom-field tabindex="0"><span hidden></span></custom-field>');
            email.form.requestSubmit();
            email.setCustomValidity("Choose another address");
            email.reportValidity();
            hidesOnFocus.setCustomValidity("Hidden during focus");
            nextField.setCustomValidity("Next field");
            hidesOnFocus.onfocus = () => hidesOnFocus.hidden = true;
            secondForm.reportValidity();
            setTimeout(() => document.title = "finished", 0);
        </script>)HTML"sv);
    Core::EventLoop::current().spin_until([&] { return finished || timed_out; });
    EXPECT(finished);
    EXPECT(messages == (Vector<Utf16String> { "Complete this required field."_utf16, "Complete this required field."_utf16, "Choose another address"_utf16 }));
    EXPECT(positions == (Vector<Gfx::IntPoint> { { 10, 50 }, { 10, 50 }, { 10, 50 } }));
}
