/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValue.h>
#include <LibWeb/Cookie/Cookie.h>
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/DOM/NodeIterator.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebDriver/ExecuteScript.h>
#include <LibWeb/WebDriver/Screenshot.h>
#include <WebContent/WebDriverConnection.h>

namespace WebContent {

// https://w3c.github.io/webdriver/#dfn-serialized-cookie
static JsonValue serialize_cookie(Web::Cookie::Cookie const& cookie)
{
    JsonObject serialized_cookie;
    serialized_cookie.set("name"sv, cookie.name.to_byte_string());
    serialized_cookie.set("value"sv, cookie.value.to_byte_string());
    serialized_cookie.set("path"sv, cookie.path.to_byte_string());
    serialized_cookie.set("domain"sv, cookie.domain.to_byte_string());
    serialized_cookie.set("secure"sv, cookie.secure);
    serialized_cookie.set("httpOnly"sv, cookie.http_only);
    serialized_cookie.set("expiry"sv, cookie.expiry_time.seconds_since_epoch());
    serialized_cookie.set("sameSite"sv, Web::Cookie::same_site_to_string(cookie.same_site));

    return serialized_cookie;
}

static JsonValue serialize_rect(Gfx::IntRect const& rect)
{
    JsonObject serialized_rect = {};
    serialized_rect.set("x", rect.x());
    serialized_rect.set("y", rect.y());
    serialized_rect.set("width", rect.width());
    serialized_rect.set("height", rect.height());

    return serialized_rect;
}

static Gfx::IntRect compute_window_rect(Web::Page const& page)
{
    return {
        page.window_position().x(),
        page.window_position().y(),
        page.window_size().width(),
        page.window_size().height()
    };
}

// https://w3c.github.io/webdriver/#dfn-calculate-the-absolute-position
static Gfx::IntPoint calculate_absolute_position_of_element(Web::Page const& page, JS::NonnullGCPtr<Web::Geometry::DOMRect> rect)
{
    // 1. Let rect be the value returned by calling getBoundingClientRect().

    // 2. Let window be the associated window of current top-level browsing context.
    auto const* window = page.top_level_browsing_context().active_window();

    // 3. Let x be (scrollX of window + rect’s x coordinate).
    auto x = (window ? static_cast<int>(window->scroll_x()) : 0) + static_cast<int>(rect->x());

    // 4. Let y be (scrollY of window + rect’s y coordinate).
    auto y = (window ? static_cast<int>(window->scroll_y()) : 0) + static_cast<int>(rect->y());

    // 5. Return a pair of (x, y).
    return { x, y };
}

static Gfx::IntRect calculate_absolute_rect_of_element(Web::Page const& page, Web::DOM::Element const& element)
{
    auto bounding_rect = element.get_bounding_client_rect();
    auto coordinates = calculate_absolute_position_of_element(page, bounding_rect);

    return {
        coordinates.x(),
        coordinates.y(),
        static_cast<int>(bounding_rect->width()),
        static_cast<int>(bounding_rect->height())
    };
}

// https://w3c.github.io/webdriver/#dfn-get-or-create-a-web-element-reference
static ByteString get_or_create_a_web_element_reference(Web::DOM::Node const& element)
{
    // FIXME: 1. For each known element of the current browsing context’s list of known elements:
    // FIXME:     1. If known element equals element, return success with known element’s web element reference.
    // FIXME: 2. Add element to the list of known elements of the current browsing context.
    // FIXME: 3. Return success with the element’s web element reference.

    return ByteString::number(element.unique_id());
}

// https://w3c.github.io/webdriver/#dfn-web-element-reference-object
static JsonObject web_element_reference_object(Web::DOM::Node const& element)
{
    // https://w3c.github.io/webdriver/#dfn-web-element-identifier
    static ByteString const web_element_identifier = "element-6066-11e4-a52e-4f735466cecf"sv;

    // 1. Let identifier be the web element identifier.
    auto identifier = web_element_identifier;

    // 2. Let reference be the result of get or create a web element reference given element.
    auto reference = get_or_create_a_web_element_reference(element);

    // 3. Return a JSON Object initialized with a property with name identifier and value reference.
    JsonObject object;
    object.set("name"sv, identifier);
    object.set("value"sv, reference);
    return object;
}

// https://w3c.github.io/webdriver/#dfn-get-a-known-connected-element
static ErrorOr<Web::DOM::Element*, Web::WebDriver::Error> get_known_connected_element(StringView element_id)
{
    // NOTE: The whole concept of "connected elements" is not implemented yet. See get_or_create_a_web_element_reference().
    //       For now the element is only represented by its ID.
    auto element = element_id.to_number<int>();
    if (!element.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Element ID is not an integer");

    auto* node = Web::DOM::Node::from_unique_id(*element);

    if (!node || !node->is_element())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, ByteString::formatted("Could not find element with ID: {}", element_id));

    return static_cast<Web::DOM::Element*>(node);
}

// https://w3c.github.io/webdriver/#dfn-get-or-create-a-shadow-root-reference
static ByteString get_or_create_a_shadow_root_reference(Web::DOM::ShadowRoot const& shadow_root)
{
    // FIXME: 1. For each known shadow root of the current browsing context’s list of known shadow roots:
    // FIXME:     1. If known shadow root equals shadow root, return success with known shadow root’s shadow root reference.
    // FIXME: 2. Add shadow to the list of known shadow roots of the current browsing context.
    // FIXME: 3. Return success with the shadow’s shadow root reference.

    return ByteString::number(shadow_root.unique_id());
}

// https://w3c.github.io/webdriver/#dfn-shadow-root-reference-object
static JsonObject shadow_root_reference_object(Web::DOM::ShadowRoot const& shadow_root)
{
    // https://w3c.github.io/webdriver/#dfn-shadow-root-identifier
    static ByteString const shadow_root_identifier = "shadow-6066-11e4-a52e-4f735466cecf"sv;

    // 1. Let identifier be the shadow root identifier.
    auto identifier = shadow_root_identifier;

    // 2. Let reference be the result of get or create a shadow root reference given shadow root.
    auto reference = get_or_create_a_shadow_root_reference(shadow_root);

    // 3. Return a JSON Object initialized with a property with name identifier and value reference.
    JsonObject object;
    object.set("name"sv, move(identifier));
    object.set("value"sv, move(reference));
    return object;
}

// https://w3c.github.io/webdriver/#dfn-get-a-known-shadow-root
static ErrorOr<Web::DOM::ShadowRoot*, Web::WebDriver::Error> get_known_shadow_root(StringView shadow_id)
{
    // NOTE: The whole concept of "known shadow roots" is not implemented yet. See get_or_create_a_shadow_root_reference().
    //       For now the shadow root is only represented by its ID.
    auto shadow_root = shadow_id.to_number<int>();
    if (!shadow_root.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Shadow ID is not an integer");

    auto* node = Web::DOM::Node::from_unique_id(*shadow_root);

    if (!node || !node->is_shadow_root())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, ByteString::formatted("Could not find shadow root with ID: {}", shadow_id));

    return static_cast<Web::DOM::ShadowRoot*>(node);
}

// https://w3c.github.io/webdriver/#dfn-scrolls-into-view
static ErrorOr<void> scroll_element_into_view(Web::DOM::Element& element)
{
    // 1. Let options be the following ScrollIntoViewOptions:
    Web::DOM::ScrollIntoViewOptions options {};
    // Logical scroll position "block"
    //     "end"
    options.block = Web::Bindings::ScrollLogicalPosition::End;
    // Logical scroll position "inline"
    //     "nearest"
    options.inline_ = Web::Bindings::ScrollLogicalPosition::Nearest;

    // 2. Run Function.[[Call]](scrollIntoView, options) with element as the this value.
    TRY(element.scroll_into_view(options));

    return {};
}

template<typename PropertyType = ByteString>
static ErrorOr<PropertyType, Web::WebDriver::Error> get_property(JsonValue const& payload, StringView key)
{
    if (!payload.is_object())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload is not a JSON object");

    auto property = payload.as_object().get(key);

    if (!property.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("No property called '{}' present", key));

    if constexpr (IsSame<PropertyType, ByteString>) {
        if (!property->is_string())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not a String", key));
        return property->as_string();
    } else if constexpr (IsSame<PropertyType, bool>) {
        if (!property->is_bool())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not a Boolean", key));
        return property->as_bool();
    } else if constexpr (IsSame<PropertyType, u32>) {
        if (auto maybe_u32 = property->get_u32(); maybe_u32.has_value())
            return *maybe_u32;
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not a Number", key));
    } else if constexpr (IsSame<PropertyType, JsonArray const*>) {
        if (!property->is_array())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not an Array", key));
        return &property->as_array();
    } else if constexpr (IsSame<PropertyType, JsonObject const*>) {
        if (!property->is_object())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not an Object", key));
        return &property->as_object();
    } else {
        static_assert(DependentFalse<PropertyType>, "get_property invoked with unknown property type");
        VERIFY_NOT_REACHED();
    }
}

// https://w3c.github.io/webdriver/#dfn-container
static Optional<Web::DOM::Element&> container_for_element(Web::DOM::Element& element)
{
    auto first_element_reached_by_traversing_the_tree_in_reverse_order = [](Web::DOM::Element& element, auto filter) -> Optional<Web::DOM::Element&> {
        auto node_iterator = element.document().create_node_iterator(element, to_underlying(Web::DOM::NodeFilter::WhatToShow::SHOW_ALL), nullptr);

        auto current_node = node_iterator->previous_node();
        while (current_node.has_value() && current_node.value() != nullptr && current_node.value()->is_element()) {
            if (filter(current_node.value()))
                return static_cast<Web::DOM::Element&>(*current_node.release_value());
        }

        return {};
    };

    // An element’s container is:
    // -> option element in a valid element context
    // -> optgroup element in a valid element context
    // FIXME: Determine if the element is in a valid element context. (https://html.spec.whatwg.org/#concept-element-contexts)
    if (is<Web::HTML::HTMLOptionElement>(element) || is<Web::HTML::HTMLOptGroupElement>(element)) {
        // The element’s element context, which is determined by:
        // 1. Let datalist parent be the first datalist element reached by traversing the tree in reverse order from element, or undefined if the root of the tree is reached.
        auto datalist_parent = first_element_reached_by_traversing_the_tree_in_reverse_order(element, [](auto& node) { return is<Web::HTML::HTMLDataListElement>(*node); });

        // 2. Let select parent be the first select element reached by traversing the tree in reverse order from element, or undefined if the root of the tree is reached.
        auto select_parent = first_element_reached_by_traversing_the_tree_in_reverse_order(element, [](auto& node) { return is<Web::HTML::HTMLSelectElement>(*node); });

        // 3. If datalist parent is undefined, the element context is select parent. Otherwise, the element context is datalist parent.
        if (!datalist_parent.has_value())
            return select_parent;
        return datalist_parent;
    }
    // -> option element in an invalid element context
    else if (is<Web::HTML::HTMLOptionElement>(element)) {
        // The element does not have a container.
        return {};
    }
    // -> Otherwise
    else {
        // The container is the element itself.
        return element;
    }
}

template<typename T>
static bool fire_an_event(FlyString name, Optional<Web::DOM::Element&> target)
{
    // FIXME: This is supposed to call the https://dom.spec.whatwg.org/#concept-event-fire DOM algorithm,
    //        but that doesn't seem to be implemented elsewhere. So, we'll ad-hack it for now. :^)

    if (!target.has_value())
        return false;

    auto event = T::create(target->realm(), name);
    return target->dispatch_event(event);
}

ErrorOr<NonnullRefPtr<WebDriverConnection>> WebDriverConnection::connect(Web::PageClient& page_client, ByteString const& webdriver_ipc_path)
{
    dbgln_if(WEBDRIVER_DEBUG, "Trying to connect to {}", webdriver_ipc_path);
    auto socket = TRY(Core::LocalSocket::connect(webdriver_ipc_path));

    // Allow pop-ups, or otherwise /window/new won't be able to open a new tab.
    page_client.page().set_should_block_pop_ups(false);

    dbgln_if(WEBDRIVER_DEBUG, "Connected to WebDriver");
    return adopt_nonnull_ref_or_enomem(new (nothrow) WebDriverConnection(move(socket), page_client));
}

WebDriverConnection::WebDriverConnection(NonnullOwnPtr<Core::LocalSocket> socket, Web::PageClient& page_client)
    : IPC::ConnectionToServer<WebDriverClientEndpoint, WebDriverServerEndpoint>(*this, move(socket))
    , m_page_client(page_client)
{
}

// https://w3c.github.io/webdriver/#dfn-close-the-session
void WebDriverConnection::close_session()
{
    // 1. Set the webdriver-active flag to false.
    set_is_webdriver_active(false);

    // 2. An endpoint node must close any top-level browsing contexts associated with the session, without prompting to unload.
    m_page_client->page().top_level_traversable()->close_top_level_traversable();
}

void WebDriverConnection::set_page_load_strategy(Web::WebDriver::PageLoadStrategy const& page_load_strategy)
{
    m_page_load_strategy = page_load_strategy;
}

void WebDriverConnection::set_unhandled_prompt_behavior(Web::WebDriver::UnhandledPromptBehavior const& unhandled_prompt_behavior)
{
    m_unhandled_prompt_behavior = unhandled_prompt_behavior;
}

void WebDriverConnection::set_strict_file_interactability(bool strict_file_interactability)
{
    m_strict_file_interactability = strict_file_interactability;
}

void WebDriverConnection::set_is_webdriver_active(bool is_webdriver_active)
{
    m_page_client->page().set_is_webdriver_active(is_webdriver_active);
}

// 9.1 Get Timeouts, https://w3c.github.io/webdriver/#dfn-get-timeouts
Messages::WebDriverClient::GetTimeoutsResponse WebDriverConnection::get_timeouts()
{
    // 1. Let timeouts be the timeouts object for session’s timeouts configuration
    auto timeouts = Web::WebDriver::timeouts_object(m_timeouts_configuration);

    // 2. Return success with data timeouts.
    return timeouts;
}

// 9.2 Set Timeouts, https://w3c.github.io/webdriver/#dfn-set-timeouts
Messages::WebDriverClient::SetTimeoutsResponse WebDriverConnection::set_timeouts(JsonValue const& payload)
{
    // 1. Let timeouts be the result of trying to JSON deserialize as a timeouts configuration the request’s parameters.
    auto timeouts = TRY(Web::WebDriver::json_deserialize_as_a_timeouts_configuration(payload));

    // 2. Make the session timeouts the new timeouts.
    m_timeouts_configuration = move(timeouts);

    // 3. Return success with data null.
    return JsonValue {};
}

// 10.1 Navigate To, https://w3c.github.io/webdriver/#navigate-to
Messages::WebDriverClient::NavigateToResponse WebDriverConnection::navigate_to(JsonValue const& payload)
{
    dbgln_if(WEBDRIVER_DEBUG, "WebDriverConnection::navigate_to {}", payload);

    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Let url be the result of getting the property url from the parameters argument.
    if (!payload.is_object() || !payload.as_object().has_string("url"sv))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload doesn't have a string `url`"sv);
    URL::URL url(payload.as_object().get_byte_string("url"sv).value());

    // FIXME: 3. If url is not an absolute URL or is not an absolute URL with fragment or not a local scheme, return error with error code invalid argument.

    // 4. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 5. Let current URL be the current top-level browsing context’s active document’s URL.
    auto const& current_url = m_page_client->page().top_level_browsing_context().active_document()->url();
    // FIXME: 6. If current URL and url do not have the same absolute URL:
    // FIXME:     a. If timer has not been started, start a timer. If this algorithm has not completed before timer reaches the session’s session page load timeout in milliseconds, return an error with error code timeout.

    // 7. Navigate the current top-level browsing context to url.
    m_page_client->page().load(url);

    // 8. If url is special except for file and current URL and URL do not have the same absolute URL:
    if (url.is_special() && url.scheme() != "file"sv && current_url != url) {
        // a. Try to wait for navigation to complete.
        TRY(wait_for_navigation_to_complete());

        // FIXME: b. Try to run the post-navigation checks.
    }

    // FIXME: 9. Set the current browsing context with the current top-level browsing context.
    // FIXME: 10. If the current top-level browsing context contains a refresh state pragma directive of time 1 second or less, wait until the refresh timeout has elapsed, a new navigate has begun, and return to the first step of this algorithm.

    // 11. Return success with data null.
    return JsonValue {};
}

// 10.2 Get Current URL, https://w3c.github.io/webdriver/#get-current-url
Messages::WebDriverClient::GetCurrentUrlResponse WebDriverConnection::get_current_url()
{
    dbgln_if(WEBDRIVER_DEBUG, "WebDriverConnection::get_current_url");

    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let url be the serialization of the current top-level browsing context’s active document’s document URL.
    auto url = m_page_client->page().top_level_browsing_context().active_document()->url().to_byte_string();

    // 4. Return success with data url.
    return url;
}

// 10.3 Back, https://w3c.github.io/webdriver/#dfn-back
Messages::WebDriverClient::BackResponse WebDriverConnection::back()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Traverse the history by a delta –1 for the current browsing context.
    m_page_client->page_did_request_navigate_back();

    // FIXME: 4. If the previous step completed results in a pageHide event firing, wait until pageShow event fires or for the session page load timeout milliseconds to pass, whichever occurs sooner.
    // FIXME: 5. If the previous step completed by the session page load timeout being reached, and user prompts have been handled, return error with error code timeout.

    // 6. Return success with data null.
    return JsonValue {};
}

// 10.4 Forward, https://w3c.github.io/webdriver/#dfn-forward
Messages::WebDriverClient::ForwardResponse WebDriverConnection::forward()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Traverse the history by a delta 1 for the current browsing context.
    m_page_client->page_did_request_navigate_forward();

    // FIXME: 4. If the previous step completed results in a pageHide event firing, wait until pageShow event fires or for the session page load timeout milliseconds to pass, whichever occurs sooner.
    // FIXME: 5. If the previous step completed by the session page load timeout being reached, and user prompts have been handled, return error with error code timeout.

    // 6. Return success with data null.
    return JsonValue {};
}

// 10.5 Refresh, https://w3c.github.io/webdriver/#dfn-refresh
Messages::WebDriverClient::RefreshResponse WebDriverConnection::refresh()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Initiate an overridden reload of the current top-level browsing context’s active document.
    m_page_client->page_did_request_refresh();

    // FIXME: 4. If url is special except for file:
    // FIXME:     1. Try to wait for navigation to complete.
    // FIXME:     2. Try to run the post-navigation checks.
    // FIXME: 5. Set the current browsing context with current top-level browsing context.

    // 6. Return success with data null.
    return JsonValue {};
}

// 10.6 Get Title, https://w3c.github.io/webdriver/#dfn-get-title
Messages::WebDriverClient::GetTitleResponse WebDriverConnection::get_title()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let title be the initial value of the title IDL attribute of the current top-level browsing context's active document.
    auto title = m_page_client->page().top_level_browsing_context().active_document()->title();

    // 4. Return success with data title.
    return title.to_byte_string();
}

// 11.1 Get Window Handle, https://w3c.github.io/webdriver/#get-window-handle
Messages::WebDriverClient::GetWindowHandleResponse WebDriverConnection::get_window_handle()
{
    return m_page_client->page().top_level_traversable()->window_handle();
}

// 11.2 Close Window, https://w3c.github.io/webdriver/#dfn-close-window
Messages::WebDriverClient::CloseWindowResponse WebDriverConnection::close_window()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Close the current top-level browsing context.
    m_page_client->page().top_level_traversable()->close_top_level_traversable();

    return JsonValue {};
}

// 11.3 Switch to Window, https://w3c.github.io/webdriver/#dfn-switch-to-window
Messages::WebDriverClient::SwitchToWindowResponse WebDriverConnection::switch_to_window()
{
    // 5. Update any implementation-specific state that would result from the user selecting the current
    //    browsing context for interaction, without altering OS-level focus.
    m_page_client->page_did_request_activate_tab();

    return JsonValue {};
}

// 11.5 New Window, https://w3c.github.io/webdriver/#dfn-new-window
Messages::WebDriverClient::NewWindowResponse WebDriverConnection::new_window(JsonValue const&)
{
    // 1. If the implementation does not support creating new top-level browsing contexts, return error with error code unsupported operation.

    // 2. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 3. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // FIXME: 4. Let type hint be the result of getting the property "type" from the parameters argument.

    // 5. Create a new top-level browsing context by running the window open steps with url set to "about:blank",
    //    target set to the empty string, and features set to "noopener" and the user agent configured to create a new
    //    browsing context. This must be done without invoking the focusing steps for the created browsing context. If
    //    type hint has the value "tab", and the implementation supports multiple browsing context in the same OS
    //    window, the new browsing context should share an OS window with the current browsing context. If type hint
    //    is "window", and the implementation supports multiple browsing contexts in separate OS windows, the
    //    created browsing context should be in a new OS window. In all other cases the details of how the browsing
    //    context is presented to the user are implementation defined.
    auto [navigable, window_type] = m_page_client->page().top_level_traversable()->choose_a_navigable("_blank"sv, Web::HTML::TokenizedFeature::NoOpener::Yes, Web::HTML::ActivateTab::No);

    // 6. Let handle be the associated window handle of the newly created window.
    auto handle = navigable->traversable_navigable()->window_handle();

    // 7. Let type be "tab" if the newly created window shares an OS-level window with the current browsing context, or "window" otherwise.
    auto type = "tab"sv;

    // 8. Let result be a new JSON Object initialized with:
    JsonObject result;
    result.set("handle"sv, JsonValue { handle });
    result.set("type"sv, JsonValue { type });

    // 9. Return success with data result.
    return result;
}

// 11.8.1 Get Window Rect, https://w3c.github.io/webdriver/#dfn-get-window-rect
Messages::WebDriverClient::GetWindowRectResponse WebDriverConnection::get_window_rect()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Return success with data set to the WindowRect object for the current top-level browsing context.
    return serialize_rect(compute_window_rect(m_page_client->page()));
}

// 11.8.2 Set Window Rect, https://w3c.github.io/webdriver/#dfn-set-window-rect
Messages::WebDriverClient::SetWindowRectResponse WebDriverConnection::set_window_rect(JsonValue const& payload)
{
    if (!payload.is_object())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload is not a JSON object");

    auto const& properties = payload.as_object();

    auto resolve_property = [](auto name, auto const& property, i32 min, i32 max) -> ErrorOr<Optional<i32>, Web::WebDriver::Error> {
        if (property.is_null())
            return Optional<i32> {};
        auto value = property.template get_integer<i32>();
        if (!value.has_value())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' is not a Number", name));
        if (*value < min)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' value {} exceeds the minimum allowed value {}", name, *value, min));
        if (*value > max)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Property '{}' value {} exceeds the maximum allowed value {}", name, *value, max));

        return value;
    };

    // 1. Let width be the result of getting a property named width from the parameters argument, else let it be null.
    auto width_property = properties.get("width"sv).value_or(JsonValue());

    // 2. Let height be the result of getting a property named height from the parameters argument, else let it be null.
    auto height_property = properties.get("height"sv).value_or(JsonValue());

    // 3. Let x be the result of getting a property named x from the parameters argument, else let it be null.
    auto x_property = properties.get("x"sv).value_or(JsonValue());

    // 4. Let y be the result of getting a property named y from the parameters argument, else let it be null.
    auto y_property = properties.get("y"sv).value_or(JsonValue());

    // 5. If width or height is neither null nor a Number from 0 to 2^31 − 1, return error with error code invalid argument.
    auto width = TRY(resolve_property("width"sv, width_property, 0, NumericLimits<i32>::max()));
    auto height = TRY(resolve_property("height"sv, height_property, 0, NumericLimits<i32>::max()));

    // 6. If x or y is neither null nor a Number from −(2^31) to 2^31 − 1, return error with error code invalid argument.
    auto x = TRY(resolve_property("x"sv, x_property, NumericLimits<i32>::min(), NumericLimits<i32>::max()));
    auto y = TRY(resolve_property("y"sv, y_property, NumericLimits<i32>::min(), NumericLimits<i32>::max()));

    // 7. If the remote end does not support the Set Window Rect command for the current top-level browsing context for any reason, return error with error code unsupported operation.

    // 8. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 9. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // FIXME: 10. Fully exit fullscreen.

    // 11. Restore the window.
    restore_the_window();

    Gfx::IntRect window_rect;

    // 11. If width and height are not null:
    if (width.has_value() && height.has_value()) {
        // a. Set the width, in CSS pixels, of the operating system window containing the current top-level browsing context, including any browser chrome and externally drawn window decorations to a value that is as close as possible to width.
        // b. Set the height, in CSS pixels, of the operating system window containing the current top-level browsing context, including any browser chrome and externally drawn window decorations to a value that is as close as possible to height.
        auto size = m_page_client->page_did_request_resize_window({ *width, *height });
        window_rect.set_size(size);
    } else {
        window_rect.set_size(m_page_client->page().window_size().to_type<int>());
    }

    // 12. If x and y are not null:
    if (x.has_value() && y.has_value()) {
        // a. Run the implementation-specific steps to set the position of the operating system level window containing the current top-level browsing context to the position given by the x and y coordinates.
        auto position = m_page_client->page_did_request_reposition_window({ *x, *y });
        window_rect.set_location(position);
    } else {
        window_rect.set_location(m_page_client->page().window_position().to_type<int>());
    }

    // 14. Return success with data set to the WindowRect object for the current top-level browsing context.
    return serialize_rect(window_rect);
}

// 11.8.3 Maximize Window, https://w3c.github.io/webdriver/#dfn-maximize-window
Messages::WebDriverClient::MaximizeWindowResponse WebDriverConnection::maximize_window()
{
    // 1. If the remote end does not support the Maximize Window command for the current top-level browsing context for any reason, return error with error code unsupported operation.

    // 2. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 3. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // FIXME: 4. Fully exit fullscreen.

    // 5. Restore the window.
    restore_the_window();

    // 6. Maximize the window of the current top-level browsing context.
    auto window_rect = maximize_the_window();

    // 7. Return success with data set to the WindowRect object for the current top-level browsing context.
    return serialize_rect(window_rect);
}

// 11.8.4 Minimize Window, https://w3c.github.io/webdriver/#minimize-window
Messages::WebDriverClient::MinimizeWindowResponse WebDriverConnection::minimize_window()
{
    // 1. If the remote end does not support the Minimize Window command for the current top-level browsing context for any reason, return error with error code unsupported operation.

    // 2. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 3. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // FIXME: 4. Fully exit fullscreen.

    // 5. Iconify the window.
    auto window_rect = iconify_the_window();

    // 6. Return success with data set to the WindowRect object for the current top-level browsing context.
    return serialize_rect(window_rect);
}

// 11.8.5 Fullscreen Window, https://w3c.github.io/webdriver/#dfn-fullscreen-window
Messages::WebDriverClient::FullscreenWindowResponse WebDriverConnection::fullscreen_window()
{
    // 1. If the remote end does not support fullscreen return error with error code unsupported operation.

    // 2. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 3. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 4. Restore the window.
    restore_the_window();

    // 5. FIXME: Call fullscreen an element with the current top-level browsing context’s active document’s document element.
    //           As described in https://fullscreen.spec.whatwg.org/#fullscreen-an-element
    //    NOTE: What we do here is basically `requestFullscreen(options)` with options["navigationUI"]="show"
    auto rect = m_page_client->page_did_request_fullscreen_window();

    // 6. Return success with data set to the WindowRect object for the current top-level browsing context.
    return serialize_rect(rect);
}

// Extension Consume User Activation, https://html.spec.whatwg.org/multipage/interaction.html#user-activation-user-agent-automation
Messages::WebDriverClient::ConsumeUserActivationResponse WebDriverConnection::consume_user_activation()
{
    // FIXME: This should probably be in the spec steps
    // If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 1. Let window be current browsing context's active window.
    auto* window = m_page_client->page().top_level_browsing_context().active_window();

    // 2. Let consume be true if window has transient activation; otherwise false.
    bool consume = window->has_transient_activation();

    // 3. If consume is true, then consume user activation of window.
    if (consume)
        window->consume_user_activation();

    // 4. Return success with data consume.
    return consume;
}

// 12.3.2 Find Element, https://w3c.github.io/webdriver/#dfn-find-element
Messages::WebDriverClient::FindElementResponse WebDriverConnection::find_element(JsonValue const& payload)
{
    // 1. Let location strategy be the result of getting a property called "using".
    auto location_strategy_string = TRY(get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Location strategy '{}' is invalid", location_strategy_string));

    // 3. Let selector be the result of getting a property called "value".
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(get_property(payload, "value"sv));

    // 5. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 6. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    auto start_node_getter = [this]() -> StartNodeGetter::ReturnType {
        // 7. Let start node be the current browsing context’s document element.
        auto* start_node = m_page_client->page().top_level_browsing_context().active_document();

        // 8. If start node is null, return error with error code no such element.
        if (!start_node)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "document element does not exist"sv);

        return start_node;
    };

    // 9. Let result be the result of trying to Find with start node, location strategy, and selector.
    auto result = TRY(find(move(start_node_getter), *location_strategy, selector));

    // 10. If result is empty, return error with error code no such element. Otherwise, return the first element of result.
    if (result.is_empty())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "The requested element does not exist"sv);

    return result.take(0);
}

// 12.3.3 Find Elements, https://w3c.github.io/webdriver/#dfn-find-elements
Messages::WebDriverClient::FindElementsResponse WebDriverConnection::find_elements(JsonValue const& payload)
{
    // 1. Let location strategy be the result of getting a property called "using".
    auto location_strategy_string = TRY(get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Location strategy '{}' is invalid", location_strategy_string));

    // 3. Let selector be the result of getting a property called "value".
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(get_property(payload, "value"sv));

    // 5. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 6. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    auto start_node_getter = [this]() -> StartNodeGetter::ReturnType {
        // 7. Let start node be the current browsing context’s document element.
        auto* start_node = m_page_client->page().top_level_browsing_context().active_document();

        // 8. If start node is null, return error with error code no such element.
        if (!start_node)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "document element does not exist"sv);

        return start_node;
    };

    // 9. Return the result of trying to Find with start node, location strategy, and selector.
    return TRY(find(move(start_node_getter), *location_strategy, selector));
}

// 12.3.4 Find Element From Element, https://w3c.github.io/webdriver/#dfn-find-element-from-element
Messages::WebDriverClient::FindElementFromElementResponse WebDriverConnection::find_element_from_element(JsonValue const& payload, String const& element_id)
{
    // 1. Let location strategy be the result of getting a property called "using".
    auto location_strategy_string = TRY(get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Location strategy '{}' is invalid", location_strategy_string));

    // 3. Let selector be the result of getting a property called "value".
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(get_property(payload, "value"sv));

    // 5. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 6. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    auto start_node_getter = [&]() -> StartNodeGetter::ReturnType {
        // 7. Let start node be the result of trying to get a known connected element with url variable element id.
        return TRY(get_known_connected_element(element_id));
    };

    // 8. Let result be the value of trying to Find with start node, location strategy, and selector.
    auto result = TRY(find(move(start_node_getter), *location_strategy, selector));

    // 9. If result is empty, return error with error code no such element. Otherwise, return the first element of result.
    if (result.is_empty())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "The requested element does not exist"sv);

    return result.take(0);
}

// 12.3.5 Find Elements From Element, https://w3c.github.io/webdriver/#dfn-find-elements-from-element
Messages::WebDriverClient::FindElementsFromElementResponse WebDriverConnection::find_elements_from_element(JsonValue const& payload, String const& element_id)
{
    // 1. Let location strategy be the result of getting a property called "using".
    auto location_strategy_string = TRY(get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Location strategy '{}' is invalid", location_strategy_string));

    // 3. Let selector be the result of getting a property called "value".
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(get_property(payload, "value"sv));

    // 5. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 6. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    auto start_node_getter = [&]() -> StartNodeGetter::ReturnType {
        // 7. Let start node be the result of trying to get a known connected element with url variable element id.
        return TRY(get_known_connected_element(element_id));
    };

    // 8. Return the result of trying to Find with start node, location strategy, and selector.
    return TRY(find(move(start_node_getter), *location_strategy, selector));
}

// 12.3.6 Find Element From Shadow Root, https://w3c.github.io/webdriver/#find-element-from-shadow-root
Messages::WebDriverClient::FindElementFromShadowRootResponse WebDriverConnection::find_element_from_shadow_root(JsonValue const& payload, String const& shadow_id)
{
    // 1. Let location strategy be the result of getting a property called "using".
    auto location_strategy_string = TRY(get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Location strategy '{}' is invalid", location_strategy_string));

    // 3. Let selector be the result of getting a property called "value".
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(get_property(payload, "value"sv));

    // 5. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 6. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    auto start_node_getter = [&]() -> StartNodeGetter::ReturnType {
        // 7. Let start node be the result of trying to get a known shadow root with url variable shadow id.
        return TRY(get_known_shadow_root(shadow_id));
    };

    // 8. Let result be the value of trying to Find with start node, location strategy, and selector.
    auto result = TRY(find(move(start_node_getter), *location_strategy, selector));

    // 9. If result is empty, return error with error code no such element. Otherwise, return the first element of result.
    if (result.is_empty())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "The requested element does not exist"sv);

    return result.take(0);
}

// 12.3.7 Find Elements From Shadow Root, https://w3c.github.io/webdriver/#find-elements-from-shadow-root
Messages::WebDriverClient::FindElementsFromShadowRootResponse WebDriverConnection::find_elements_from_shadow_root(JsonValue const& payload, String const& shadow_id)
{
    // 1. Let location strategy be the result of getting a property called "using".
    auto location_strategy_string = TRY(get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, ByteString::formatted("Location strategy '{}' is invalid", location_strategy_string));

    // 3. Let selector be the result of getting a property called "value".
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(get_property(payload, "value"sv));

    // 5. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 6. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    auto start_node_getter = [&]() -> StartNodeGetter::ReturnType {
        // 7. Let start node be the result of trying to get a known shadow root with url variable shadow id.
        return TRY(get_known_shadow_root(shadow_id));
    };

    // 8. Return the result of trying to Find with start node, location strategy, and selector.
    return TRY(find(move(start_node_getter), *location_strategy, selector));
}

// 12.3.8 Get Active Element, https://w3c.github.io/webdriver/#get-active-element
Messages::WebDriverClient::GetActiveElementResponse WebDriverConnection::get_active_element()
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let active element be the active element of the current browsing context’s document element.
    auto* active_element = m_page_client->page().top_level_browsing_context().active_document()->active_element();

    // 4. If active element is a non-null element, return success with data set to web element reference object for active element.
    //    Otherwise, return error with error code no such element.
    if (active_element)
        return ByteString::number(active_element->unique_id());

    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "The current document does not have an active element"sv);
}

// 12.3.9 Get Element Shadow Root, https://w3c.github.io/webdriver/#get-element-shadow-root
Messages::WebDriverClient::GetElementShadowRootResponse WebDriverConnection::get_element_shadow_root(String const& element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Let shadow root be element's shadow root.
    auto shadow_root = element->shadow_root();

    // 5. If shadow root is null, return error with error code no such shadow root.
    if (!shadow_root)
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchShadowRoot, ByteString::formatted("Element with ID '{}' does not have a shadow root", element_id));

    // 6. Let serialized be the shadow root reference object for shadow root.
    auto serialized = shadow_root_reference_object(*shadow_root);

    // 7. Return success with data serialized.
    return serialized;
}

// 12.4.1 Is Element Selected, https://w3c.github.io/webdriver/#dfn-is-element-selected
Messages::WebDriverClient::IsElementSelectedResponse WebDriverConnection::is_element_selected(String const& element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Let selected be the value corresponding to the first matching statement:
    bool selected = false;

    // element is an input element with a type attribute in the Checkbox- or Radio Button state
    if (is<Web::HTML::HTMLInputElement>(*element)) {
        // -> The result of element’s checkedness.
        auto& input = static_cast<Web::HTML::HTMLInputElement&>(*element);
        using enum Web::HTML::HTMLInputElement::TypeAttributeState;

        if (input.type_state() == Checkbox || input.type_state() == RadioButton)
            selected = input.checked();
    }
    // element is an option element
    else if (is<Web::HTML::HTMLOptionElement>(*element)) {
        // -> The result of element’s selectedness.
        selected = static_cast<Web::HTML::HTMLOptionElement&>(*element).selected();
    }
    // Otherwise
    //   -> False.

    // 5. Return success with data selected.
    return selected;
}

// 12.4.2 Get Element Attribute, https://w3c.github.io/webdriver/#dfn-get-element-attribute
Messages::WebDriverClient::GetElementAttributeResponse WebDriverConnection::get_element_attribute(String const& element_id, String const& name)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Let result be the result of the first matching condition:
    Optional<ByteString> result;

    // -> If name is a boolean attribute
    if (Web::HTML::is_boolean_attribute(name)) {
        // "true" (string) if the element has the attribute, otherwise null.
        if (element->has_attribute(name))
            result = "true"sv;
    }
    // -> Otherwise
    else {
        // The result of getting an attribute by name name.
        if (auto attr = element->get_attribute(name); attr.has_value())
            result = attr->to_byte_string();
    }

    // 5. Return success with data result.
    if (result.has_value())
        return result.release_value();
    return JsonValue {};
}

// 12.4.3 Get Element Property, https://w3c.github.io/webdriver/#dfn-get-element-property
Messages::WebDriverClient::GetElementPropertyResponse WebDriverConnection::get_element_property(String const& element_id, String const& name)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    Optional<ByteString> result;

    // 4. Let property be the result of calling the Object.[[GetProperty]](name) on element.
    if (auto property_or_error = element->get(name.to_byte_string()); !property_or_error.is_throw_completion()) {
        auto property = property_or_error.release_value();

        // 5. Let result be the value of property if not undefined, or null.
        if (!property.is_undefined()) {
            if (auto string_or_error = property.to_byte_string(element->vm()); !string_or_error.is_error())
                result = string_or_error.release_value();
        }
    }

    // 6. Return success with data result.
    if (result.has_value())
        return result.release_value();
    return JsonValue {};
}

// 12.4.4 Get Element CSS Value, https://w3c.github.io/webdriver/#dfn-get-element-css-value
Messages::WebDriverClient::GetElementCssValueResponse WebDriverConnection::get_element_css_value(String const& element_id, String const& name)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Let computed value be the result of the first matching condition:
    ByteString computed_value;

    // -> current browsing context’s active document’s type is not "xml"
    if (!m_page_client->page().top_level_browsing_context().active_document()->is_xml_document()) {
        // computed value of parameter property name from element’s style declarations. property name is obtained from url variables.
        if (auto property = Web::CSS::property_id_from_string(name); property.has_value()) {
            if (auto* computed_values = element->computed_css_values())
                computed_value = computed_values->property(property.value())->to_string().to_byte_string();
        }
    }
    // -> Otherwise
    else {
        // "" (empty string)
        computed_value = ByteString::empty();
    }

    // 5. Return success with data computed value.
    return computed_value;
}

// 12.4.5 Get Element Text, https://w3c.github.io/webdriver/#dfn-get-element-text
Messages::WebDriverClient::GetElementTextResponse WebDriverConnection::get_element_text(String const& element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Let rendered text be the result of performing implementation-specific steps whose result is exactly the same as the result of a Function.[[Call]](null, element) with bot.dom.getVisibleText as the this value.
    auto rendered_text = element->text_content();

    // 5. Return success with data rendered text.
    return rendered_text.value_or(String {}).to_byte_string();
}

// 12.4.6 Get Element Tag Name, https://w3c.github.io/webdriver/#dfn-get-element-tag-name
Messages::WebDriverClient::GetElementTagNameResponse WebDriverConnection::get_element_tag_name(String const& element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Let qualified name be the result of getting element’s tagName IDL attribute.
    auto qualified_name = element->tag_name();

    // 5. Return success with data qualified name.
    return MUST(JsonValue::from_string(qualified_name));
}

// 12.4.7 Get Element Rect, https://w3c.github.io/webdriver/#dfn-get-element-rect
Messages::WebDriverClient::GetElementRectResponse WebDriverConnection::get_element_rect(String const& element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Calculate the absolute position of element and let it be coordinates.
    // 5. Let rect be element’s bounding rectangle.
    auto rect = calculate_absolute_rect_of_element(m_page_client->page(), *element);

    // 6. Let body be a new JSON Object initialized with:
    // "x"
    //     The first value of coordinates.
    // "y"
    //     The second value of coordinates.
    // "width"
    //     Value of rect’s width dimension.
    // "height"
    //     Value of rect’s height dimension.
    auto body = serialize_rect(rect);

    // 7. Return success with data body.
    return body;
}

// 12.4.8 Is Element Enabled, https://w3c.github.io/webdriver/#dfn-is-element-enabled
Messages::WebDriverClient::IsElementEnabledResponse WebDriverConnection::is_element_enabled(String const& element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Let enabled be a boolean initially set to true if the current browsing context’s active document’s type is not "xml".
    // 5. Otherwise, let enabled to false and jump to the last step of this algorithm.
    bool enabled = !m_page_client->page().top_level_browsing_context().active_document()->is_xml_document();

    // 6. Set enabled to false if a form control is disabled.
    if (enabled && is<Web::HTML::FormAssociatedElement>(*element)) {
        auto& form_associated_element = dynamic_cast<Web::HTML::FormAssociatedElement&>(*element);
        enabled = form_associated_element.enabled();
    }

    // 7. Return success with data enabled.
    return enabled;
}

// 12.4.9 Get Computed Role, https://w3c.github.io/webdriver/#dfn-get-computed-role
Messages::WebDriverClient::GetComputedRoleResponse WebDriverConnection::get_computed_role(String const& element_id)
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Let role be the result of computing the WAI-ARIA role of element.
    auto role = element->role_or_default();

    // 5. Return success with data role.
    if (role.has_value())
        return Web::ARIA::role_name(*role);
    return ""sv;
}

// 12.4.10 Get Computed Label, https://w3c.github.io/webdriver/#get-computed-label
Messages::WebDriverClient::GetComputedLabelResponse WebDriverConnection::get_computed_label(String const& element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Let label be the result of a Accessible Name and Description Computation for the Accessible Name of the element.
    auto label = element->accessible_name(element->document()).release_value_but_fixme_should_propagate_errors();

    // 5. Return success with data label.
    return label.to_byte_string();
}

// 12.5.1 Element Click, https://w3c.github.io/webdriver/#element-click
Messages::WebDriverClient::ElementClickResponse WebDriverConnection::element_click(String const& element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known element with element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. If the element is an input element in the file upload state return error with error code invalid argument.
    if (is<Web::HTML::HTMLInputElement>(*element)) {
        // -> The result of element’s checkedness.
        auto& input = static_cast<Web::HTML::HTMLInputElement&>(*element);
        using enum Web::HTML::HTMLInputElement::TypeAttributeState;

        if (input.type_state() == FileUpload)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Clicking on an input element in the file upload state is not supported"sv);
    }

    // 5. Scroll into view the element’s container.
    auto element_container = container_for_element(*element);
    auto scroll_or_error = scroll_element_into_view(*element_container);
    if (scroll_or_error.is_error())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, scroll_or_error.error().string_literal());

    // FIXME: 6. If element’s container is still not in view, return error with error code element not interactable.

    // FIXME: 7. If element’s container is obscured by another element, return error with error code element click intercepted.

    // 8. Matching on element:
    // -> option element
    if (is<Web::HTML::HTMLOptionElement>(*element)) {
        auto& option_element = static_cast<Web::HTML::HTMLOptionElement&>(*element);

        // 1. Let parent node be the element’s container.
        auto parent_node = element_container;

        // 2. Fire a mouseOver event at parent node.
        fire_an_event<Web::UIEvents::MouseEvent>(Web::UIEvents::EventNames::mouseover, parent_node);

        // 3. Fire a mouseMove event at parent node.
        fire_an_event<Web::UIEvents::MouseEvent>(Web::UIEvents::EventNames::mousemove, parent_node);

        // 4. Fire a mouseDown event at parent node.
        fire_an_event<Web::UIEvents::MouseEvent>(Web::UIEvents::EventNames::mousedown, parent_node);

        // 5. Run the focusing steps on parent node.
        Web::HTML::run_focusing_steps(parent_node.has_value() ? &*parent_node : nullptr);

        // 6. If element is not disabled:
        if (!option_element.is_actually_disabled()) {
            // 1. Fire an input event at parent node.
            fire_an_event<Web::DOM::Event>(Web::HTML::EventNames::input, parent_node);

            // 2. Let previous selectedness be equal to element selectedness.
            auto previous_selectedness = option_element.selected();

            // 3. If element’s container has the multiple attribute, toggle the element’s selectedness state
            //    by setting it to the opposite value of its current selectedness.
            if (parent_node.has_value() && parent_node->has_attribute(Web::HTML::AttributeNames::multiple)) {
                option_element.set_selected(!option_element.selected());
            }
            //    Otherwise, set the element’s selectedness state to true.
            else {
                option_element.set_selected(true);
            }

            // 4. If previous selectedness is false, fire a change event at parent node.
            if (!previous_selectedness) {
                fire_an_event<Web::DOM::Event>(Web::HTML::EventNames::change, parent_node);
            }
        }
        // 7. Fire a mouseUp event at parent node.
        fire_an_event<Web::UIEvents::MouseEvent>(Web::UIEvents::EventNames::mouseup, parent_node);

        // 8. Fire a click event at parent node.
        fire_an_event<Web::UIEvents::MouseEvent>(Web::UIEvents::EventNames::click, parent_node);
    }
    // -> Otherwise
    else {
        // FIXME: 1. Let input state be the result of get the input state given current session and current top-level browsing context.

        // FIXME: 2. Let actions options be a new actions options with the is element origin steps set to represents a web element, and the get element origin steps set to get a WebElement origin.

        // FIXME: 3. Let input id be a the result of generating a UUID.

        // FIXME: 4. Let source be the result of create an input source with input state, and "pointer".

        // FIXME: 5. Add an input source with input state, input id and source.

        // FIXME: 6. Let click point be the element’s in-view center point.

        // FIXME: 7. Let pointer move action be an action object constructed with arguments input id, "pointer", and "pointerMove".

        // FIXME: 8. Set a property x to 0 on pointer move action.

        // FIXME: 9. Set a property y to 0 on pointer move action.

        // FIXME: 10. Set a property origin to element on pointer move action.

        // FIXME: 11. Let pointer down action be an action object constructed with arguments input id, "pointer", and "pointerDown".

        // FIXME: 12. Set a property button to 0 on pointer down action.

        // FIXME: 13. Let pointer up action be an action object constructed with arguments input id, "mouse", and "pointerUp" as arguments.

        // FIXME: 14. Set a property button to 0 on pointer up action.

        // FIXME: 15. Let actions be the list «pointer move action, pointer down action, pointer move action».

        // FIXME: 16. Dispatch a list of actions with input state, actions, current browsing context, and actions options.

        // FIXME: 17. Remove an input source with input state and input id.
    }

    // FIXME: 9. Wait until the user agent event loop has spun enough times to process the DOM events generated by the previous step.
    // FIXME: 10. Perform implementation-defined steps to allow any navigations triggered by the click to start.
    // FIXME: 11. Try to wait for navigation to complete.
    // FIXME: 12. Try to run the post-navigation checks.
    // FIXME: 13. Return success with data null.

    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnsupportedOperation, "Click not implemented"sv);
}

// 13.1 Get Page Source, https://w3c.github.io/webdriver/#dfn-get-page-source
Messages::WebDriverClient::GetSourceResponse WebDriverConnection::get_source()
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    auto* document = m_page_client->page().top_level_browsing_context().active_document();
    Optional<ByteString> source;

    // 3. Let source be the result of invoking the fragment serializing algorithm on a fictional node whose only child is the document element providing true for the require well-formed flag. If this causes an exception to be thrown, let source be null.
    if (auto result = document->serialize_fragment(Web::DOMParsing::RequireWellFormed::Yes); !result.is_error())
        source = result.release_value().to_byte_string();

    // 4. Let source be the result of serializing to string the current browsing context active document, if source is null.
    if (!source.has_value())
        source = MUST(document->serialize_fragment(Web::DOMParsing::RequireWellFormed::No)).to_byte_string();

    // 5. Return success with data source.
    return source.release_value();
}

// 13.2.1 Execute Script, https://w3c.github.io/webdriver/#dfn-execute-script
Messages::WebDriverClient::ExecuteScriptResponse WebDriverConnection::execute_script(JsonValue const& payload)
{
    // 1. Let body and arguments be the result of trying to extract the script arguments from a request with argument parameters.
    auto const& [body, arguments] = TRY(extract_the_script_arguments_from_a_request(payload));

    // 2. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 3. Handle any user prompts, and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 4., 5.1-5.3.
    auto result = Web::WebDriver::execute_script(m_page_client->page(), body, move(arguments), m_timeouts_configuration.script_timeout);
    dbgln_if(WEBDRIVER_DEBUG, "Executing script returned: {}", result.value);

    switch (result.type) {
    // 6. If promise is still pending and the session script timeout is reached, return error with error code script timeout.
    case Web::WebDriver::ExecuteScriptResultType::Timeout:
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ScriptTimeoutError, "Script timed out");
    // 7. Upon fulfillment of promise with value v, let result be a JSON clone of v, and return success with data result.
    case Web::WebDriver::ExecuteScriptResultType::PromiseResolved:
        return move(result.value);
    // 8. Upon rejection of promise with reason r, let result be a JSON clone of r, and return error with error code javascript error and data result.
    case Web::WebDriver::ExecuteScriptResultType::PromiseRejected:
    case Web::WebDriver::ExecuteScriptResultType::JavaScriptError:
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::JavascriptError, "Script returned an error", move(result.value));
    case Web::WebDriver::ExecuteScriptResultType::BrowsingContextDiscarded:
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::StaleElementReference, "Browsing context has been discarded", move(result.value));
    }

    VERIFY_NOT_REACHED();
}

// 13.2.2 Execute Async Script, https://w3c.github.io/webdriver/#dfn-execute-async-script
Messages::WebDriverClient::ExecuteAsyncScriptResponse WebDriverConnection::execute_async_script(JsonValue const& payload)
{
    // 1. Let body and arguments by the result of trying to extract the script arguments from a request with argument parameters.
    auto const& [body, arguments] = TRY(extract_the_script_arguments_from_a_request(payload));

    // 2. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 3. Handle any user prompts, and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 4., 5.1-5.11.
    auto result = Web::WebDriver::execute_async_script(m_page_client->page(), body, move(arguments), m_timeouts_configuration.script_timeout);
    dbgln_if(WEBDRIVER_DEBUG, "Executing async script returned: {}", result.value);

    switch (result.type) {
    // 6. If promise is still pending and the session script timeout is reached, return error with error code script timeout.
    case Web::WebDriver::ExecuteScriptResultType::Timeout:
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ScriptTimeoutError, "Script timed out");
    // 7. Upon fulfillment of promise with value v, let result be a JSON clone of v, and return success with data result.
    case Web::WebDriver::ExecuteScriptResultType::PromiseResolved:
        return move(result.value);
    // 8. Upon rejection of promise with reason r, let result be a JSON clone of r, and return error with error code javascript error and data result.
    case Web::WebDriver::ExecuteScriptResultType::PromiseRejected:
    case Web::WebDriver::ExecuteScriptResultType::JavaScriptError:
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::JavascriptError, "Script returned an error", move(result.value));
    case Web::WebDriver::ExecuteScriptResultType::BrowsingContextDiscarded:
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::StaleElementReference, "Browsing context has been discarded", move(result.value));
    }

    VERIFY_NOT_REACHED();
}

// 14.1 Get All Cookies, https://w3c.github.io/webdriver/#dfn-get-all-cookies
Messages::WebDriverClient::GetAllCookiesResponse WebDriverConnection::get_all_cookies()
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts, and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let cookies be a new JSON List.
    JsonArray cookies;

    // 4. For each cookie in all associated cookies of the current browsing context’s active document:
    auto* document = m_page_client->page().top_level_browsing_context().active_document();

    for (auto const& cookie : m_page_client->page_did_request_all_cookies(document->url())) {
        // 1. Let serialized cookie be the result of serializing cookie.
        auto serialized_cookie = serialize_cookie(cookie);

        // 2. Append serialized cookie to cookies
        TRY(cookies.append(move(serialized_cookie)));
    }

    // 5. Return success with data cookies.
    return cookies;
}

// 14.2 Get Named Cookie, https://w3c.github.io/webdriver/#dfn-get-named-cookie
Messages::WebDriverClient::GetNamedCookieResponse WebDriverConnection::get_named_cookie(String const& name)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts, and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. If the url variable name is equal to a cookie’s cookie name amongst all associated cookies of the current browsing context’s active document, return success with the serialized cookie as data.
    auto* document = m_page_client->page().top_level_browsing_context().active_document();

    if (auto cookie = m_page_client->page_did_request_named_cookie(document->url(), name); cookie.has_value()) {
        auto serialized_cookie = serialize_cookie(*cookie);
        return serialized_cookie;
    }

    // 4. Otherwise, return error with error code no such cookie.
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchCookie, ByteString::formatted("Cookie '{}' not found", name));
}

// 14.3 Add Cookie, https://w3c.github.io/webdriver/#dfn-adding-a-cookie
Messages::WebDriverClient::AddCookieResponse WebDriverConnection::add_cookie(JsonValue const& payload)
{
    // 1. Let data be the result of getting a property named cookie from the parameters argument.
    auto const& data = *TRY(get_property<JsonObject const*>(payload, "cookie"sv));

    // 2. If data is not a JSON Object with all the required (non-optional) JSON keys listed in the table for cookie conversion, return error with error code invalid argument.
    // NOTE: This validation is performed in subsequent steps.

    // 3. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 4. Handle any user prompts, and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // FIXME: 5. If the current browsing context’s document element is a cookie-averse Document object, return error with error code invalid cookie domain.

    // 6. If cookie name or cookie value is null, cookie domain is not equal to the current browsing context’s active document’s domain, cookie secure only or cookie HTTP only are not boolean types, or cookie expiry time is not an integer type, or it less than 0 or greater than the maximum safe integer, return error with error code invalid argument.
    // NOTE: This validation is either performed in subsequent steps, or is performed by the CookieJar (namely domain matching).

    // 7. Create a cookie in the cookie store associated with the active document’s address using cookie name name, cookie value value, and an attribute-value list of the following cookie concepts listed in the table for cookie conversion from data:
    Web::Cookie::ParsedCookie cookie {};
    cookie.name = MUST(String::from_byte_string(TRY(get_property(data, "name"sv))));
    cookie.value = MUST(String::from_byte_string(TRY(get_property(data, "value"sv))));

    // Cookie path
    //     The value if the entry exists, otherwise "/".
    if (data.has("path"sv))
        cookie.path = MUST(String::from_byte_string(TRY(get_property(data, "path"sv))));
    else
        cookie.path = "/"_string;

    // Cookie domain
    //     The value if the entry exists, otherwise the current browsing context’s active document’s URL domain.
    // NOTE: The otherwise case is handled by the CookieJar
    if (data.has("domain"sv))
        cookie.domain = MUST(String::from_byte_string(TRY(get_property(data, "domain"sv))));

    // Cookie secure only
    //     The value if the entry exists, otherwise false.
    if (data.has("secure"sv))
        cookie.secure_attribute_present = TRY(get_property<bool>(data, "secure"sv));

    // Cookie HTTP only
    //     The value if the entry exists, otherwise false.
    if (data.has("httpOnly"sv))
        cookie.http_only_attribute_present = TRY(get_property<bool>(data, "httpOnly"sv));

    // Cookie expiry time
    //     The value if the entry exists, otherwise leave unset to indicate that this is a session cookie.
    if (data.has("expiry"sv)) {
        // NOTE: less than 0 or greater than safe integer are handled by the JSON parser
        auto expiry = TRY(get_property<u32>(data, "expiry"sv));
        cookie.expiry_time_from_expires_attribute = UnixDateTime::from_seconds_since_epoch(expiry);
    }

    // Cookie same site
    //     The value if the entry exists, otherwise leave unset to indicate that no same site policy is defined.
    if (data.has("sameSite"sv)) {
        auto same_site = TRY(get_property(data, "sameSite"sv));
        cookie.same_site_attribute = Web::Cookie::same_site_from_string(same_site);
    }

    auto* document = m_page_client->page().top_level_browsing_context().active_document();
    m_page_client->page_did_set_cookie(document->url(), cookie, Web::Cookie::Source::Http);

    // If there is an error during this step, return error with error code unable to set cookie.
    // NOTE: This probably should only apply to the actual setting of the cookie in the Browser, which cannot fail in our case.

    // 8. Return success with data null.
    return JsonValue {};
}

// 14.4 Delete Cookie, https://w3c.github.io/webdriver/#dfn-delete-cookie
Messages::WebDriverClient::DeleteCookieResponse WebDriverConnection::delete_cookie(String const& name)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts, and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Delete cookies using the url variable name parameter as the filter argument.
    delete_cookies(name);

    // 4. Return success with data null.
    return JsonValue {};
}

// 14.5 Delete All Cookies, https://w3c.github.io/webdriver/#dfn-delete-all-cookies
Messages::WebDriverClient::DeleteAllCookiesResponse WebDriverConnection::delete_all_cookies()
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts, and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Delete cookies, giving no filtering argument.
    delete_cookies();

    // 4. Return success with data null.
    return JsonValue {};
}

// 15.8 Release Actions, https://w3c.github.io/webdriver/#release-actions
Messages::WebDriverClient::ReleaseActionsResponse WebDriverConnection::release_actions()
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // FIXME: 2. Let input state be the result of get the input state with current session and current top-level browsing context.

    // FIXME: 3. Let actions options be a new actions options with the is element origin steps set to represents a web element, and the get element origin steps set to get a WebElement origin.

    // FIXME: 4. Let undo actions be input state’s input cancel list in reverse order.

    // FIXME: 5. Try to dispatch tick actions with arguments undo actions, 0, current browsing context, and actions options.

    // FIXME: 6. Reset the input state with current session and current top-level browsing context.

    // 7. Return success with data null.
    return JsonValue {};
}

// 16.1 Dismiss Alert, https://w3c.github.io/webdriver/#dismiss-alert
Messages::WebDriverClient::DismissAlertResponse WebDriverConnection::dismiss_alert()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. If there is no current user prompt, return error with error code no such alert.
    if (!m_page_client->page().has_pending_dialog())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchAlert, "No user dialog is currently open"sv);

    // 3. Dismiss the current user prompt.
    m_page_client->page().dismiss_dialog();

    // 4. Return success with data null.
    return JsonValue {};
}

// 16.2 Accept Alert, https://w3c.github.io/webdriver/#accept-alert
Messages::WebDriverClient::AcceptAlertResponse WebDriverConnection::accept_alert()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. If there is no current user prompt, return error with error code no such alert.
    if (!m_page_client->page().has_pending_dialog())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchAlert, "No user dialog is currently open"sv);

    // 3. Accept the current user prompt.
    m_page_client->page().accept_dialog();

    // 4. Return success with data null.
    return JsonValue {};
}

// 16.3 Get Alert Text, https://w3c.github.io/webdriver/#get-alert-text
Messages::WebDriverClient::GetAlertTextResponse WebDriverConnection::get_alert_text()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. If there is no current user prompt, return error with error code no such alert.
    if (!m_page_client->page().has_pending_dialog())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchAlert, "No user dialog is currently open"sv);

    // 3. Let message be the text message associated with the current user prompt, or otherwise be null.
    auto const& message = m_page_client->page().pending_dialog_text();

    // 4. Return success with data message.
    if (message.has_value())
        return message->to_byte_string();
    return JsonValue {};
}

// 16.4 Send Alert Text, https://w3c.github.io/webdriver/#send-alert-text
Messages::WebDriverClient::SendAlertTextResponse WebDriverConnection::send_alert_text(JsonValue const& payload)
{
    // 1. Let text be the result of getting the property "text" from parameters.
    // 2. If text is not a String, return error with error code invalid argument.
    auto text = TRY(get_property(payload, "text"sv));

    // 3. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 4. If there is no current user prompt, return error with error code no such alert.
    if (!m_page_client->page().has_pending_dialog())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchAlert, "No user dialog is currently open"sv);

    // 5. Run the substeps of the first matching current user prompt:
    switch (m_page_client->page().pending_dialog()) {
    // -> alert
    // -> confirm
    case Web::Page::PendingDialog::Alert:
    case Web::Page::PendingDialog::Confirm:
        // Return error with error code element not interactable.
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ElementNotInteractable, "Only prompt dialogs may receive text"sv);

    // -> prompt
    case Web::Page::PendingDialog::Prompt:
        // Do nothing.
        break;

    // -> Otherwise
    default:
        // Return error with error code unsupported operation.
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnsupportedOperation, "Unknown dialog type"sv);
    }

    // 6. Perform user agent dependent steps to set the value of current user prompt’s text field to text.
    m_page_client->page_did_request_set_prompt_text(TRY(String::from_byte_string(text)));

    // 7. Return success with data null.
    return JsonValue {};
}

// 17.1 Take Screenshot, https://w3c.github.io/webdriver/#take-screenshot
Messages::WebDriverClient::TakeScreenshotResponse WebDriverConnection::take_screenshot()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. When the user agent is next to run the animation frame callbacks:
    //     a. Let root rect be the current top-level browsing context’s document element’s rectangle.
    //     b. Let screenshot result be the result of trying to call draw a bounding box from the framebuffer, given root rect as an argument.
    //     c. Let canvas be a canvas element of screenshot result’s data.
    //     d. Let encoding result be the result of trying encoding a canvas as Base64 canvas.
    //     e. Let encoded string be encoding result’s data.
    auto* document = m_page_client->page().top_level_browsing_context().active_document();
    auto root_rect = calculate_absolute_rect_of_element(m_page_client->page(), *document->document_element());

    auto encoded_string = TRY(Web::WebDriver::capture_element_screenshot(
        [&](auto const& rect, auto& bitmap) { m_page_client->paint(rect.template to_type<Web::DevicePixels>(), bitmap); },
        m_page_client->page(),
        *document->document_element(),
        root_rect));

    // 3. Return success with data encoded string.
    return encoded_string;
}

// 17.2 Take Element Screenshot, https://w3c.github.io/webdriver/#dfn-take-element-screenshot
Messages::WebDriverClient::TakeElementScreenshotResponse WebDriverConnection::take_element_screenshot(String const& element_id)
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_open_top_level_browsing_context());

    // 2. Handle any user prompts and return its value if it is an error.
    TRY(handle_any_user_prompts());

    // 3. Let element be the result of trying to get a known connected element with url variable element id.
    auto* element = TRY(get_known_connected_element(element_id));

    // 4. Scroll into view the element.
    (void)scroll_element_into_view(*element);

    // 5. When the user agent is next to run the animation frame callbacks:
    //     a. Let element rect be element’s rectangle.
    //     b. Let screenshot result be the result of trying to call draw a bounding box from the framebuffer, given element rect as an argument.
    //     c. Let canvas be a canvas element of screenshot result’s data.
    //     d. Let encoding result be the result of trying encoding a canvas as Base64 canvas.
    //     e. Let encoded string be encoding result’s data.
    auto element_rect = calculate_absolute_rect_of_element(m_page_client->page(), *element);

    auto encoded_string = TRY(Web::WebDriver::capture_element_screenshot(
        [&](auto const& rect, auto& bitmap) { m_page_client->paint(rect.template to_type<Web::DevicePixels>(), bitmap); },
        m_page_client->page(),
        *element,
        element_rect));

    // 6. Return success with data encoded string.
    return encoded_string;
}

// 18.1 Print Page, https://w3c.github.io/webdriver/#dfn-print-page
Messages::WebDriverClient::PrintPageResponse WebDriverConnection::print_page()
{
    // FIXME: Actually implement this :^)
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnsupportedOperation, "Print not implemented"sv);
}

// https://w3c.github.io/webdriver/#dfn-no-longer-open
Messages::WebDriverClient::EnsureTopLevelBrowsingContextIsOpenResponse WebDriverConnection::ensure_top_level_browsing_context_is_open()
{
    // A browsing context is said to be no longer open if its navigable has been destroyed.
    if (m_page_client->page().top_level_browsing_context().has_navigable_been_destroyed())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv);
    return JsonValue {};
}

// https://w3c.github.io/webdriver/#dfn-no-longer-open
ErrorOr<void, Web::WebDriver::Error> WebDriverConnection::ensure_open_top_level_browsing_context()
{
    TRY(ensure_top_level_browsing_context_is_open().take_response());
    return {};
}

// https://w3c.github.io/webdriver/#dfn-handle-any-user-prompts
ErrorOr<void, Web::WebDriver::Error> WebDriverConnection::handle_any_user_prompts()
{
    // 1. If there is no current user prompt, abort these steps and return success.
    if (!m_page_client->page().has_pending_dialog())
        return {};

    // 2. Perform the following substeps based on the current session’s user prompt handler:
    switch (m_unhandled_prompt_behavior) {
    // -> dismiss state
    case Web::WebDriver::UnhandledPromptBehavior::Dismiss:
        // Dismiss the current user prompt.
        m_page_client->page().dismiss_dialog();
        break;

    // -> accept state
    case Web::WebDriver::UnhandledPromptBehavior::Accept:
        // Accept the current user prompt.
        m_page_client->page().accept_dialog();
        break;

    // -> dismiss and notify state
    case Web::WebDriver::UnhandledPromptBehavior::DismissAndNotify:
        // Dismiss the current user prompt.
        m_page_client->page().dismiss_dialog();

        // Return an annotated unexpected alert open error.
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnexpectedAlertOpen, "A user dialog is open"sv);

    // -> accept and notify state
    case Web::WebDriver::UnhandledPromptBehavior::AcceptAndNotify:
        // Accept the current user prompt.
        m_page_client->page().accept_dialog();

        // Return an annotated unexpected alert open error.
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnexpectedAlertOpen, "A user dialog is open"sv);

    // -> ignore state
    case Web::WebDriver::UnhandledPromptBehavior::Ignore:
        // Return an annotated unexpected alert open error.
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnexpectedAlertOpen, "A user dialog is open"sv);
    }

    // 3. Return success.
    return {};
}

// https://w3c.github.io/webdriver/#dfn-waiting-for-the-navigation-to-complete
ErrorOr<void, Web::WebDriver::Error> WebDriverConnection::wait_for_navigation_to_complete()
{
    // 1. If the current session has a page loading strategy of none, return success with data null.
    if (m_page_load_strategy == Web::WebDriver::PageLoadStrategy::None)
        return {};

    // 2. If the current browsing context is no longer open, return success with data null.
    if (m_page_client->page().top_level_browsing_context().has_navigable_been_destroyed())
        return {};

    // FIXME: 3. Start a timer. If this algorithm has not completed before timer reaches the session’s session page load timeout in milliseconds, return an error with error code timeout.

    // 4. If there is an ongoing attempt to navigate the current browsing context that has not yet matured, wait for navigation to mature.
    Web::Platform::EventLoopPlugin::the().spin_until([&] {
        return m_page_client->page().top_level_traversable()->ongoing_navigation() == Empty {};
    });

    // 5. Let readiness target be the document readiness state associated with the current session’s page loading strategy, which can be found in the table of page load strategies.
    auto readiness_target = [this]() {
        switch (m_page_load_strategy) {
        case Web::WebDriver::PageLoadStrategy::Normal:
            return Web::HTML::DocumentReadyState::Complete;
        case Web::WebDriver::PageLoadStrategy::Eager:
            return Web::HTML::DocumentReadyState::Interactive;
        default:
            VERIFY_NOT_REACHED();
        };
    }();

    // 6. Wait for the current browsing context’s document readiness state to reach readiness target,
    // FIXME: or for the session page load timeout to pass, whichever occurs sooner.
    Web::Platform::EventLoopPlugin::the().spin_until([&]() {
        return m_page_client->page().top_level_browsing_context().active_document()->readiness() == readiness_target;
    });

    // FIXME: 7. If the previous step completed by the session page load timeout being reached and the browser does not have an active user prompt, return error with error code timeout.

    // 8. Return success with data null.
    return {};
}

// https://w3c.github.io/webdriver/#dfn-restore-the-window
void WebDriverConnection::restore_the_window()
{
    // To restore the window, given an operating system level window with an associated top-level browsing context, run implementation-specific steps to restore or unhide the window to the visible screen.
    m_page_client->page_did_request_restore_window();

    // Do not return from this operation until the visibility state of the top-level browsing context’s active document has reached the visible state, or until the operation times out.
    // FIXME: Implement timeouts.
    Web::Platform::EventLoopPlugin::the().spin_until([this]() {
        auto state = m_page_client->page().top_level_traversable()->system_visibility_state();
        return state == Web::HTML::VisibilityState::Visible;
    });
}

// https://w3c.github.io/webdriver/#dfn-maximize-the-window
Gfx::IntRect WebDriverConnection::maximize_the_window()
{
    // To maximize the window, given an operating system level window with an associated top-level browsing context, run the implementation-specific steps to transition the operating system level window into the maximized window state.
    auto rect = m_page_client->page_did_request_maximize_window();

    // Return when the window has completed the transition, or within an implementation-defined timeout.
    return rect;
}

// https://w3c.github.io/webdriver/#dfn-iconify-the-window
Gfx::IntRect WebDriverConnection::iconify_the_window()
{
    // To iconify the window, given an operating system level window with an associated top-level browsing context, run implementation-specific steps to iconify, minimize, or hide the window from the visible screen.
    auto rect = m_page_client->page_did_request_minimize_window();

    // Do not return from this operation until the visibility state of the top-level browsing context’s active document has reached the hidden state, or until the operation times out.
    // FIXME: Implement timeouts.
    Web::Platform::EventLoopPlugin::the().spin_until([this]() {
        auto state = m_page_client->page().top_level_traversable()->system_visibility_state();
        return state == Web::HTML::VisibilityState::Hidden;
    });

    return rect;
}

// https://w3c.github.io/webdriver/#dfn-find
ErrorOr<JsonArray, Web::WebDriver::Error> WebDriverConnection::find(StartNodeGetter&& start_node_getter, Web::WebDriver::LocationStrategy using_, StringView value)
{
    // 1. Let end time be the current time plus the session implicit wait timeout.
    auto end_time = MonotonicTime::now() + Duration::from_milliseconds(static_cast<i64>(m_timeouts_configuration.implicit_wait_timeout));

    // 2. Let location strategy be equal to using.
    auto location_strategy = using_;

    // 3. Let selector be equal to value.
    auto selector = value;

    ErrorOr<JS::GCPtr<Web::DOM::NodeList>, Web::WebDriver::Error> maybe_elements { nullptr };

    auto try_to_find_element = [&]() -> decltype(maybe_elements) {
        // 4. Let elements returned be the result of trying to call the relevant element location strategy with arguments start node, and selector.
        auto elements = Web::WebDriver::invoke_location_strategy(location_strategy, *TRY(start_node_getter()), selector);

        // 5. If a DOMException, SyntaxError, XPathException, or other error occurs during the execution of the element location strategy, return error invalid selector.
        if (elements.is_error())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidSelector, ByteString::formatted("The location strategy could not finish: {}", elements.error().message));

        return elements.release_value();
    };

    Web::Platform::EventLoopPlugin::the().spin_until([&]() {
        maybe_elements = try_to_find_element();
        if (maybe_elements.is_error())
            return true;

        // 6. If elements returned is empty and the current time is less than end time return to step 4. Otherwise, continue to the next step.
        return maybe_elements.value()->length() != 0 || MonotonicTime::now() >= end_time;
    });

    auto elements = TRY(maybe_elements);
    VERIFY(elements);

    // 7. Let result be an empty JSON List.
    JsonArray result;
    result.ensure_capacity(elements->length());

    // 8. For each element in elements returned, append the web element reference object for element, to result.
    for (size_t i = 0; i < elements->length(); ++i)
        TRY(result.append(web_element_reference_object(*elements->item(i))));

    // 9. Return success with data result.
    return result;
}

// https://w3c.github.io/webdriver/#dfn-extract-the-script-arguments-from-a-request
ErrorOr<WebDriverConnection::ScriptArguments, Web::WebDriver::Error> WebDriverConnection::extract_the_script_arguments_from_a_request(JsonValue const& payload)
{
    auto* window = m_page_client->page().top_level_browsing_context().active_window();
    auto& vm = window->vm();

    // 1. Let script be the result of getting a property named script from the parameters.
    // 2. If script is not a String, return error with error code invalid argument.
    auto script = TRY(get_property(payload, "script"sv));

    // 3. Let args be the result of getting a property named args from the parameters.
    // 4. If args is not an Array return error with error code invalid argument.
    auto const& args = *TRY(get_property<JsonArray const*>(payload, "args"sv));

    // 5. Let arguments be the result of calling the JSON deserialize algorithm with arguments args.
    auto arguments = JS::MarkedVector<JS::Value> { vm.heap() };

    args.for_each([&](auto const& arg) {
        arguments.append(JS::JSONObject::parse_json_value(vm, arg));
    });

    // 6. Return success with data script and arguments.
    return ScriptArguments { move(script), move(arguments) };
}

// https://w3c.github.io/webdriver/#dfn-delete-cookies
void WebDriverConnection::delete_cookies(Optional<StringView> const& name)
{
    // For each cookie among all associated cookies of the current browsing context’s active document, un the substeps of the first matching condition:
    auto* document = m_page_client->page().top_level_browsing_context().active_document();

    for (auto& cookie : m_page_client->page_did_request_all_cookies(document->url())) {
        // -> name is undefined
        // -> name is equal to cookie name
        if (!name.has_value() || name.value() == cookie.name) {
            // Set the cookie expiry time to a Unix timestamp in the past.
            cookie.expiry_time = UnixDateTime::earliest();
            m_page_client->page_did_update_cookie(move(cookie));
        }
        // -> Otherwise
        //    Do nothing.
    }
}

}
