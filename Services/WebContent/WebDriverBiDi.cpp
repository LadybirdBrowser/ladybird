/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibGC/Root.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/BrowsingContext.h>
#include <LibWeb/WebDriver/BiDi/Script.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/WebDriverBiDi.h>

namespace WebContent {

static Web::WebDriver::Error no_such_frame()
{
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchFrame, "Browsing context not found"sv);
}

static PageClient* page_client_of(Web::HTML::Navigable const& navigable)
{
    return as_if<PageClient>(const_cast<Web::HTML::Navigable&>(navigable).page().client());
}

void set_webdriver_bidi_session_state(Web::WebDriver::BiDiSessionState state)
{
    // NB: Events of the navigables this process hosts go to the page hosting them, which reports them to the browser
    //     process.
    if (state.is_active && !Web::WebDriver::BiDi::is_active()) {
        Web::WebDriver::BiDi::set_event_sink({
            .on_event = [](Web::HTML::Navigable const& navigable, String const& name, JsonValue params) {
                if (auto* page_client = page_client_of(navigable))
                    page_client->webdriver_bidi_event(navigable.id(), name, move(params)); },
            .on_navigation_event = [](Web::HTML::Navigable const& navigable, StringView name, Optional<Utf16String> const& navigation_id, URL::URL const& url) {
                if (auto* page_client = page_client_of(navigable))
                    page_client->webdriver_bidi_navigation_event(navigable.id(), name, navigation_id, url); },
        });
    }

    Web::WebDriver::BiDi::set_session_state(move(state));
}

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-create
static Web::WebDriver::Response create_a_new_top_level_traversable(Web::HTML::Navigable& navigable)
{
    // 11. Let traversable be the result of trying to create a new top-level traversable steps with null, empty string,
    //     null, and false, and setting the associated user context for the newly created top-level traversable to user
    //     context.
    // NB: The window open steps create it without an opener, the way WebDriver's New Window command does.
    auto* local_navigable = as_if<Web::HTML::LocalNavigable>(navigable);
    if (!local_navigable || !local_navigable->active_window())
        return no_such_frame();

    auto& active_window = *local_navigable->active_window();
    Web::HTML::TemporaryExecutionContext execution_context { active_window.document()->relevant_settings_object() };
    auto opened_window = active_window.window_open_steps_internal(u"about:blank"sv, u""sv, u"noopener"sv);
    if (opened_window.is_error() || !opened_window.value().navigable)
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnsupportedOperation, "Unable to create a top-level traversable"sv);

    auto traversable = opened_window.value().navigable->traversable_navigable();
    auto const* local_traversable = as_if<Web::HTML::LocalTraversableNavigable>(*traversable);
    if (!local_traversable)
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnsupportedOperation, "Unable to create a top-level traversable"sv);

    JsonObject result;
    result.set("context"sv, local_traversable->window_handle().to_utf8());
    return JsonValue { move(result) };
}

void run_webdriver_bidi_command(PageClient& page_client, u64 command_id, Web::HTML::CrossProcessId navigable_id, String const& method, JsonValue params_value)
{
    auto on_complete = [page_client = GC::make_root(page_client), command_id](Web::WebDriver::Response response) {
        page_client->webdriver_command_complete(command_id, move(response));
    };

    auto navigable = page_client.page().navigable_with_id(navigable_id);
    if (!navigable) {
        on_complete(no_such_frame());
        return;
    }
    if (!params_value.is_object()) {
        on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Command parameters must be an object"sv));
        return;
    }
    auto const& params = params_value.as_object();

    if (method == "script.evaluate"sv)
        return Web::WebDriver::BiDi::evaluate(*navigable, params, move(on_complete));
    if (method == "script.callFunction"sv)
        return Web::WebDriver::BiDi::call_function(*navigable, params, move(on_complete));
    if (method == "script.disown"sv)
        return on_complete(Web::WebDriver::BiDi::disown(*navigable, params));
    if (method == "script.getRealms"sv)
        return on_complete(Web::WebDriver::BiDi::get_realms(*navigable, params));
    if (method == "browsingContext.captureScreenshot"sv)
        return Web::WebDriver::BiDi::capture_screenshot(*navigable, params, move(on_complete));
    if (method == "browsingContext.locateNodes"sv)
        return on_complete(Web::WebDriver::BiDi::locate_nodes(*navigable, params));
    if (method == "browsingContext.create"sv)
        return on_complete(create_a_new_top_level_traversable(*navigable));

    on_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownCommand, "Command is not supported"sv));
}

}
