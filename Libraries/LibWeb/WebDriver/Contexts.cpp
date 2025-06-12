/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebDriver/Contexts.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-web-window-identifier
static JS::PropertyKey const WEB_WINDOW_IDENTIFIER { "window-fcc6-11e5-b4f8-330a88ab9d7f"_fly_string };

// https://w3c.github.io/webdriver/#dfn-web-frame-identifier
static JS::PropertyKey const WEB_FRAME_IDENTIFIER { "frame-075b-4da1-b6ba-e579c2d3230a"_fly_string };

// https://w3c.github.io/webdriver/#dfn-windowproxy-reference-object
JsonObject window_proxy_reference_object(HTML::WindowProxy const& window)
{
    // 1. Let identifier be the web window identifier if the associated browsing context of window is a top-level browsing context.
    //    Otherwise let it be the web frame identifier.

    // NOTE: We look at the active browsing context's active document's node navigable instead.
    //      Because a Browsing context's top-level traversable is this navigable's top level traversable.
    //      Ref: https://html.spec.whatwg.org/multipage/document-sequences.html#bc-traversable
    auto navigable = window.associated_browsing_context()->active_document()->navigable();

    auto const& identifier = navigable->is_top_level_traversable()
        ? WEB_WINDOW_IDENTIFIER
        : WEB_FRAME_IDENTIFIER;

    // 2. Return a JSON Object initialized with the following properties:
    JsonObject object;

    // identifier
    //    Associated window handle of the window’s browsing context.
    object.set(identifier.as_string(), navigable->traversable_navigable()->window_handle());

    return object;
}

static GC::Ptr<HTML::Navigable> find_navigable_with_handle(StringView handle, bool should_be_top_level)
{
    for (auto navigable : Web::HTML::all_navigables()) {
        if (navigable->is_top_level_traversable() != should_be_top_level)
            continue;

        if (navigable->traversable_navigable()->window_handle() == handle)
            return navigable;
    }

    return {};
}

// https://w3c.github.io/webdriver/#dfn-represents-a-web-frame
bool represents_a_web_frame(JS::Value value)
{
    // An ECMAScript Object represents a web frame if it has a web frame identifier own property.
    if (!value.is_object())
        return false;

    auto result = value.as_object().has_own_property(WEB_WINDOW_IDENTIFIER);
    return !result.is_error() && result.value();
}

// https://w3c.github.io/webdriver/#dfn-deserialize-a-web-frame
ErrorOr<GC::Ref<HTML::WindowProxy>, WebDriver::Error> deserialize_web_frame(JS::Object const& object)
{
    // 1. If object has no own property web frame identifier, return error with error code invalid argument.
    auto property = object.get(WEB_FRAME_IDENTIFIER);
    if (property.is_error() || !property.value().is_string())
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Object is not a web frame"sv);

    // 2. Let reference be the result of getting the web frame identifier property from object.
    auto reference = property.value().as_string().utf8_string();

    // 3. Let browsing context be the browsing context whose window handle is reference, or null if no such browsing
    //    context exists.
    auto navigable = find_navigable_with_handle(reference, false);

    // 4. If browsing context is null or a top-level browsing context, return error with error code no such frame.
    // NOTE: We filtered on the top-level browsing context condition in the previous step.
    if (!navigable)
        return WebDriver::Error::from_code(WebDriver::ErrorCode::NoSuchFrame, "Could not locate frame"sv);

    // 5. Return success with data browsing context's associated window.
    return *navigable->active_window_proxy();
}

// https://w3c.github.io/webdriver/#dfn-represents-a-web-frame
bool represents_a_web_window(JS::Value value)
{
    // An ECMAScript Object represents a web window if it has a web window identifier own property.
    if (!value.is_object())
        return false;

    auto result = value.as_object().has_own_property(WEB_WINDOW_IDENTIFIER);
    return !result.is_error() && result.value();
}

// https://w3c.github.io/webdriver/#dfn-deserialize-a-web-frame
ErrorOr<GC::Ref<HTML::WindowProxy>, WebDriver::Error> deserialize_web_window(JS::Object const& object)
{
    // 1. If object has no own property web window identifier, return error with error code invalid argument.
    auto property = object.get(WEB_WINDOW_IDENTIFIER);
    if (property.is_error() || !property.value().is_string())
        return WebDriver::Error::from_code(WebDriver::ErrorCode::InvalidArgument, "Object is not a web window"sv);

    // 2. Let reference be the result of getting the web window identifier property from object.
    auto reference = property.value().as_string().utf8_string();

    // 3. Let browsing context be the browsing context whose window handle is reference, or null if no such browsing
    //    context exists.
    auto navigable = find_navigable_with_handle(reference, true);

    // 4. If browsing context is null or not a top-level browsing context, return error with error code no such window.
    // NOTE: We filtered on the top-level browsing context condition in the previous step.
    if (!navigable)
        return WebDriver::Error::from_code(WebDriver::ErrorCode::NoSuchWindow, "Could not locate window"sv);

    // 5. Return success with data browsing context's associated window.
    return *navigable->active_window_proxy();
}

// https://w3c.github.io/webdriver/#dfn-no-longer-open
ErrorOr<void, WebDriver::Error> ensure_browsing_context_is_open(GC::Ptr<HTML::BrowsingContext> browsing_context)
{
    // A browsing context is said to be no longer open if its navigable has been destroyed.
    if (!browsing_context || browsing_context->has_navigable_been_destroyed())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv);
    return {};
}

}
