/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#if !defined(AK_OS_MACOS)
#    include <LibCore/Socket.h>
#else
#    include <LibIPC/TransportBootstrapMach.h>
#endif
#include <LibGC/Heap.h>
#include <LibGC/Timer.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibIPC/Transport.h>
#include <LibJS/Runtime/Value.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/CSS.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/HTML/AnimationFrameCallbackDriver.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLFrameElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/NavigationObserver.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HTML/XMLSerializer.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Loader/FileRequest.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebDriver/Actions.h>
#include <LibWeb/WebDriver/Contexts.h>
#include <LibWeb/WebDriver/ElementReference.h>
#include <LibWeb/WebDriver/InputState.h>
#include <LibWeb/WebDriver/JSON.h>
#include <LibWeb/WebDriver/Properties.h>
#include <LibWeb/WebDriver/Screenshot.h>
#include <LibWeb/WebDriver/UserPrompt.h>
#include <LibWebView/HistoryDebug.h>
#include <WebContent/PageClient.h>
#include <WebContent/WebDriverConnection.h>

namespace WebContent {

#define WEBDRIVER_TRY(expression)                                                                    \
    ({                                                                                               \
        /* Ignore -Wshadow to allow nesting the macro. */                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow",                                                             \
            auto&& _temporary_result = (expression));                                                \
        static_assert(!::AK::Detail::IsLvalueReference<decltype(_temporary_result.release_value())>, \
            "Do not return a reference from a fallible expression");                                 \
        if (_temporary_result.is_error()) [[unlikely]] {                                             \
            driver_execution_complete({ _temporary_result.release_error() });                        \
            return;                                                                                  \
        }                                                                                            \
        _temporary_result.release_value();                                                           \
    })

// https://w3c.github.io/webdriver/#dfn-serialized-cookie
static JsonValue serialize_cookie(HTTP::Cookie::Cookie const& cookie)
{
    JsonObject serialized_cookie;
    serialized_cookie.set("name"sv, cookie.name);
    serialized_cookie.set("value"sv, cookie.value);
    serialized_cookie.set("path"sv, cookie.path);
    serialized_cookie.set("domain"sv, cookie.domain);
    serialized_cookie.set("secure"sv, cookie.secure);
    serialized_cookie.set("httpOnly"sv, cookie.http_only);
    if (cookie.persistent)
        serialized_cookie.set("expiry"sv, cookie.expiry_time.seconds_since_epoch()); // Must not be set if omitted when adding a cookie.
    serialized_cookie.set("sameSite"sv, HTTP::Cookie::same_site_to_string(cookie.same_site));

    return serialized_cookie;
}

static JsonValue serialize_rect(Gfx::IntRect const& rect)
{
    JsonObject serialized_rect = {};
    serialized_rect.set("x"sv, rect.x());
    serialized_rect.set("y"sv, rect.y());
    serialized_rect.set("width"sv, rect.width());
    serialized_rect.set("height"sv, rect.height());

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

// https://w3c.github.io/webdriver/#dfn-scrolls-into-view
static void scroll_element_into_view(Web::DOM::Element& element)
{
    // 1. Let options be the following ScrollIntoViewOptions:
    Web::DOM::Element::ScrollIntoViewOptions options {};
    // "behavior"
    //     "instant"
    options.behavior = Web::DOM::Element::ScrollBehavior::Instant;
    // Logical scroll position "block"
    //     "end"
    options.block = Web::DOM::Element::ScrollLogicalPosition::End;
    // Logical scroll position "inline"
    //     "nearest"
    options.inline_ = Web::DOM::Element::ScrollLogicalPosition::Nearest;

    // 2. Run Function.[[Call]](scrollIntoView, options) with element as the this value.
    element.scroll_into_view(options, nullptr);
}

// https://w3c.github.io/webdriver/#dfn-container
static Optional<Web::DOM::Element&> container_for_element(Web::DOM::Element& element)
{
    auto first_element_reached_by_traversing_the_tree_in_reverse_order = [](Web::DOM::Element& element, auto filter) -> Optional<Web::DOM::Element&> {
        for (auto* current = element.previous_element_in_pre_order(); current; current = current->previous_element_in_pre_order()) {
            if (filter(*current))
                return *current;
        }

        return {};
    };

    // An element’s container is:
    // -> option element in a valid element context
    // -> optgroup element in a valid element context
    // FIXME: Determine if the element is in a valid element context. (https://html.spec.whatwg.org/multipage/dom.html#concept-element-contexts)
    if (is<Web::HTML::HTMLOptionElement>(element) || is<Web::HTML::HTMLOptGroupElement>(element)) {
        // The element’s element context, which is determined by:
        // 1. Let datalist parent be the first datalist element reached by traversing the tree in reverse order from element, or undefined if the root of the tree is reached.
        auto datalist_parent = first_element_reached_by_traversing_the_tree_in_reverse_order(element, [](auto& node) { return is<Web::HTML::HTMLDataListElement>(node); });

        // 2. Let select parent be the first select element reached by traversing the tree in reverse order from element, or undefined if the root of the tree is reached.
        auto select_parent = first_element_reached_by_traversing_the_tree_in_reverse_order(element, [](auto& node) { return is<Web::HTML::HTMLSelectElement>(node); });

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
static bool fire_an_event(Utf16FlyString const& name, Optional<Web::DOM::Element&> target)
{
    // FIXME: This is supposed to call the https://dom.spec.whatwg.org/#concept-event-fire DOM algorithm,
    //        but that doesn't seem to be implemented elsewhere. So, we'll ad-hack it for now. :^)

    if (!target.has_value())
        return false;

    GC::Ref<T> event = [&] {
        if constexpr (IsSame<T, Web::UIEvents::MouseEvent>)
            return T::create(name, {}, 0, 0, 0, 0, Web::HighResolutionTime::unsafe_shared_current_time());
        else
            return T::create(Web::HTML::relevant_global_object(*target), name);
    }();
    return target->dispatch_event(event);
}

NonnullRefPtr<WebDriverConnection> WebDriverConnection::create(PageClient& page_client)
{
    // Allow pop-ups, or otherwise /window/new won't be able to open a new tab.
    page_client.page().set_should_block_pop_ups(false);

    return adopt_ref(*new WebDriverConnection(page_client));
}

WebDriverConnection::WebDriverConnection(PageClient& page_client)
    : m_page_client(page_client)
{
    set_current_top_level_browsing_context(*page_client.page().top_level_traversable()->active_browsing_context());
    page_client.page().set_is_webdriver_active(true);
}

void WebDriverConnection::driver_execution_complete(Web::WebDriver::Response response)
{
    if (!m_current_command_id.has_value())
        return;

    m_pending_window_rect_requests.clear();

    auto command_id = m_current_command_id.release_value();
    m_page_client->webdriver_command_complete(command_id, move(response));
}

void WebDriverConnection::run_command(u64 command_id, String const& name, JsonValue payload, Vector<String> arguments)
{
    VERIFY(!m_current_command_id.has_value());
    m_current_command_id = command_id;

    auto argument = [&](size_t index) {
        return index < arguments.size() ? arguments[index] : String {};
    };

    struct Dispatch {
        enum class Completes : u8 {
            Now,
            Later,
        };
        Completes completes;
        Web::WebDriver::Response response;
    };

    auto asynchronous = [](Web::WebDriver::Response response) -> Dispatch {
        if (response.is_error())
            return { Dispatch::Completes::Now, move(response) };
        return { Dispatch::Completes::Later, JsonValue {} };
    };
    auto synchronous = [](Web::WebDriver::Response response) -> Dispatch {
        return { Dispatch::Completes::Now, move(response) };
    };

    auto result = [&]() -> Dispatch {
        if (name == "get_current_url"sv)
            return asynchronous(get_current_url());
        if (name == "get_title"sv)
            return asynchronous(get_title());
        if (name == "close_window"sv)
            return asynchronous(close_window());
        if (name == "switch_to_window"sv)
            return synchronous(switch_to_window(argument(0)));
        if (name == "new_window"sv)
            return asynchronous(new_window(move(payload)));
        if (name == "switch_to_frame"sv)
            return asynchronous(switch_to_frame(move(payload)));
        if (name == "switch_to_parent_frame"sv)
            return asynchronous(switch_to_parent_frame(move(payload)));
        if (name == "get_window_rect"sv)
            return asynchronous(get_window_rect());
        if (name == "set_window_rect"sv)
            return asynchronous(set_window_rect(move(payload)));
        if (name == "maximize_window"sv)
            return asynchronous(maximize_window());
        if (name == "minimize_window"sv)
            return asynchronous(minimize_window());
        if (name == "fullscreen_window"sv)
            return asynchronous(fullscreen_window());
        if (name == "consume_user_activation"sv)
            return synchronous(consume_user_activation());
        if (name == "crash_current_page"sv) {
            crash_current_page();
            return synchronous(JsonValue {});
        }
        if (name == "set_current_browsing_context_to_top_level"sv) {
            set_current_browsing_context_to_top_level();
            return synchronous(JsonValue {});
        }
        if (name == "find_element"sv)
            return asynchronous(find_element(move(payload)));
        if (name == "find_elements"sv)
            return asynchronous(find_elements(move(payload)));
        if (name == "find_element_from_element"sv)
            return asynchronous(find_element_from_element(move(payload), argument(0)));
        if (name == "find_elements_from_element"sv)
            return asynchronous(find_elements_from_element(move(payload), argument(0)));
        if (name == "find_element_from_shadow_root"sv)
            return asynchronous(find_element_from_shadow_root(move(payload), argument(0)));
        if (name == "find_elements_from_shadow_root"sv)
            return asynchronous(find_elements_from_shadow_root(move(payload), argument(0)));
        if (name == "get_active_element"sv)
            return asynchronous(get_active_element());
        if (name == "get_element_shadow_root"sv)
            return asynchronous(get_element_shadow_root(argument(0)));
        if (name == "is_element_selected"sv)
            return asynchronous(is_element_selected(argument(0)));
        if (name == "get_element_attribute"sv)
            return asynchronous(get_element_attribute(argument(0), argument(1)));
        if (name == "get_element_property"sv)
            return asynchronous(get_element_property(argument(0), argument(1)));
        if (name == "get_element_css_value"sv)
            return asynchronous(get_element_css_value(argument(0), argument(1)));
        if (name == "get_element_text"sv)
            return asynchronous(get_element_text(argument(0)));
        if (name == "get_element_tag_name"sv)
            return asynchronous(get_element_tag_name(argument(0)));
        if (name == "get_element_rect"sv)
            return asynchronous(get_element_rect(argument(0)));
        if (name == "is_element_enabled"sv)
            return asynchronous(is_element_enabled(argument(0)));
        if (name == "get_computed_role"sv)
            return asynchronous(get_computed_role(argument(0)));
        if (name == "get_computed_label"sv)
            return asynchronous(get_computed_label(argument(0)));
        if (name == "element_click"sv)
            return asynchronous(element_click(argument(0)));
        if (name == "element_clear"sv)
            return asynchronous(element_clear(argument(0)));
        if (name == "element_send_keys"sv)
            return asynchronous(element_send_keys(argument(0), move(payload)));
        if (name == "get_source"sv)
            return asynchronous(get_source());
        if (name == "execute_script"sv)
            return asynchronous(execute_script(move(payload)));
        if (name == "execute_async_script"sv)
            return asynchronous(execute_async_script(move(payload)));
        if (name == "get_all_cookies"sv)
            return asynchronous(get_all_cookies());
        if (name == "get_named_cookie"sv)
            return asynchronous(get_named_cookie(argument(0)));
        if (name == "add_cookie"sv)
            return asynchronous(add_cookie(move(payload)));
        if (name == "delete_cookie"sv)
            return asynchronous(delete_cookie(argument(0)));
        if (name == "delete_all_cookies"sv)
            return asynchronous(delete_all_cookies());
        if (name == "perform_actions"sv)
            return asynchronous(perform_actions(move(payload)));
        if (name == "release_actions"sv)
            return asynchronous(release_actions());
        if (name == "dismiss_alert"sv)
            return asynchronous(dismiss_alert());
        if (name == "accept_alert"sv)
            return asynchronous(accept_alert());
        if (name == "get_alert_text"sv)
            return synchronous(get_alert_text());
        if (name == "send_alert_text"sv)
            return synchronous(send_alert_text(move(payload)));
        if (name == "take_screenshot"sv)
            return asynchronous(take_screenshot());
        if (name == "take_element_screenshot"sv)
            return asynchronous(take_element_screenshot(argument(0)));
        if (name == "print_page"sv)
            return synchronous(print_page(move(payload)));
        if (name == "ensure_top_level_browsing_context_is_open"sv)
            return synchronous(ensure_top_level_browsing_context_is_open());
        return synchronous(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownCommand, "Unknown WebDriver command"sv));
    }();

    if (result.completes == Dispatch::Completes::Now)
        driver_execution_complete(move(result.response));
}

void WebDriverConnection::set_session_config(Web::WebDriver::PageLoadStrategy page_load_strategy, bool strict_file_interactability, JsonValue const& timeouts)
{
    m_page_load_strategy = page_load_strategy;
    m_strict_file_interactability = strict_file_interactability;

    m_timeouts_configuration = {};
    if (timeouts.is_object())
        MUST(Web::WebDriver::json_deserialize_as_a_timeouts_configuration_into(timeouts, m_timeouts_configuration));
}

void WebDriverConnection::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_page_client);
    visitor.visit(m_current_browsing_context);
    visitor.visit(m_current_parent_browsing_context);
    visitor.visit(m_current_top_level_browsing_context);
    visitor.visit(m_element_locator);
    visitor.visit(m_action_executor);
    visitor.visit(m_document_observer);
    visitor.visit(m_navigation_observer);
    visitor.visit(m_navigation_timer);
}

void WebDriverConnection::set_current_browsing_context_to_top_level()
{
    set_current_browsing_context(*current_top_level_browsing_context());
}

// 10.2 Get Current URL, https://w3c.github.io/webdriver/#get-current-url
Web::WebDriver::Response WebDriverConnection::get_current_url()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 3. Let url be the serialization of the current top-level browsing context’s active document’s document URL.
        auto url = current_top_level_browsing_context()->active_document()->url();

        // 4. Return success with data url.
        driver_execution_complete({ url.to_string() });
    });

    return JsonValue {};
}

void WebDriverConnection::crash_current_page()
{
    Core::deferred_invoke([] {
        Core::Process::terminate_immediately(1);
    });
}

// 10.6 Get Title, https://w3c.github.io/webdriver/#dfn-get-title
Web::WebDriver::Response WebDriverConnection::get_title()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 3. Let title be the initial value of the title IDL attribute of the current top-level browsing context's active document.
        auto title = current_top_level_browsing_context()->active_document()->title().to_utf8();

        // 4. Return success with data title.
        driver_execution_complete({ move(title) });
    });

    return JsonValue {};
}

// 11.2 Close Window, https://w3c.github.io/webdriver/#dfn-close-window
Web::WebDriver::Response WebDriverConnection::close_window()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 3. Close the current top-level browsing context.
        // FIXME: Spec issue: Closing browsing contexts is no longer a spec concept, we must instead close the top-level
        //        traversable. We must also do so asynchronously, as the implementation will spin the event loop in some
        //        steps. If a user dialog is open in another window within this agent, the event loop will be paused, and
        //        those spins will hang. So we must return control to the client, who can deal with the dialog.
        Web::HTML::queue_a_task(Web::HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(current_top_level_browsing_context()->heap(), [this]() {
            // The page can close this window on its own before this task runs, and a closed
            // context has no document or navigable left to reach its traversable through.
            if (ensure_current_top_level_browsing_context_is_open().is_error())
                return;
            current_top_level_browsing_context()->top_level_traversable()->close_top_level_traversable(Web::HTML::LocalTraversableNavigable::PromptToUnload::No);
        }));

        driver_execution_complete(JsonValue {});
    });

    return JsonValue {};
}

// 11.3 Switch to Window, https://w3c.github.io/webdriver/#dfn-switch-to-window
Web::WebDriver::Response WebDriverConnection::switch_to_window(String handle)
{
    // 4. If handle is equal to the associated window handle for some top-level browsing context, let context be the that
    //    browsing context, and set the current top-level browsing context with session and context.
    //    Otherwise, return error with error code no such window.
    auto handle_utf16 = Utf16String::from_utf8(handle);
    bool found_matching_context = false;

    for (auto navigable : Web::HTML::all_local_navigables()) {
        auto& traversable = as<Web::HTML::LocalTraversableNavigable>(*navigable->top_level_traversable());
        if (!traversable.active_browsing_context())
            continue;

        if (handle_utf16 == traversable.window_handle()) {
            set_current_top_level_browsing_context(*traversable.active_browsing_context());
            found_matching_context = true;
            break;
        }
    }

    if (!found_matching_context)
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv);

    // 5. Update any implementation-specific state that would result from the user selecting the current
    //    browsing context for interaction, without altering OS-level focus.
    current_browsing_context().page().client().page_did_request_activate_tab();

    return JsonValue {};
}

// 11.5 New Window, https://w3c.github.io/webdriver/#dfn-new-window
Web::WebDriver::Response WebDriverConnection::new_window(JsonValue payload)
{
    // 1. If the implementation does not support creating new top-level browsing contexts, return error with error code unsupported operation.

    // 2. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 3. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, payload = move(payload)]() {
        // 4. Let type hint be the result of getting the property "type" from the parameters argument.
        if (!payload.is_object()) {
            driver_execution_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload is not a JSON object"sv));
            return;
        }

        // FIXME: Actually use this value to decide between an OS window or tab.
        auto type_hint = payload.as_object().get("type"sv);
        if (type_hint.has_value() && !type_hint->is_null() && !type_hint->is_string()) {
            driver_execution_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload property `type` is not null or a string"sv));
            return;
        }

        // 5. Create a new top-level browsing context by running the window open steps with url set to "about:blank",
        //    target set to the empty string, and features set to "noopener" and the user agent configured to create a new
        //    browsing context. This must be done without invoking the focusing steps for the created browsing context. If
        //    type hint has the value "tab", and the implementation supports multiple browsing context in the same OS
        //    window, the new browsing context should share an OS window with the current browsing context. If type hint
        //    is "window", and the implementation supports multiple browsing contexts in separate OS windows, the
        //    created browsing context should be in a new OS window. In all other cases the details of how the browsing
        //    context is presented to the user are implementation defined.
        auto* active_window = current_browsing_context().active_window();
        VERIFY(active_window);

        Web::HTML::TemporaryExecutionContext execution_context { active_window->document()->relevant_settings_object() };
        auto [target_navigable, no_opener, window_type] = MUST(active_window->window_open_steps_internal("about:blank"sv, ""sv, "noopener"sv));

        // 6. Let handle be the associated window handle of the newly created window.
        auto handle = target_navigable->traversable_navigable()->window_handle().to_utf8();

        // 7. Let type be "tab" if the newly created window shares an OS-level window with the current browsing context, or "window" otherwise.
        auto type = "tab"sv;

        // 8. Let result be a new JSON Object initialized with:
        JsonObject result;
        result.set("handle"sv, JsonValue { handle });
        result.set("type"sv, JsonValue { type });

        // 9. Return success with data result.
        driver_execution_complete({ move(result) });
    });

    return JsonValue {};
}

// 11.6 Switch To Frame, https://w3c.github.io/webdriver/#dfn-switch-to-frame
Web::WebDriver::Response WebDriverConnection::switch_to_frame(JsonValue payload)
{
    // 1. Let id be the result of getting the property "id" from parameters.
    if (!payload.is_object() || !payload.as_object().has("id"sv))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload doesn't have property `id`"sv);

    auto id = payload.as_object().get("id"sv).release_value();

    // 2. If id is not null, a Number object, or an Object that represents a web element, return error with error code invalid argument.
    if (!id.is_null() && !id.is_number() && !Web::WebDriver::represents_a_web_element(id))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload property `id` is not null, a number, or a web element"sv);

    // 3. Run the substeps of the first matching condition:

    // -> id is null
    if (id.is_null()) {
        // 1. If session's current top-level browsing context is no longer open, return error with error code no such window.
        TRY(ensure_current_top_level_browsing_context_is_open());

        // 2. Try to handle any user prompts with session.
        handle_any_user_prompts([this]() {
            // 3. Set the current browsing context with session and session's current top-level browsing context.
            set_current_browsing_context(*current_top_level_browsing_context());

            driver_execution_complete(JsonValue {});
        });
    }

    // -> id is a Number object
    else if (id.is_number()) {
        // 1. If id is less than 0 or greater than 2^16 – 1, return error with error code invalid argument.
        auto id_value = id.get_integer<u16>();

        if (!id_value.has_value())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Frame ID {} is invalid", id)));

        // 2. If session's current browsing context is no longer open, return error with error code no such window.
        TRY(ensure_current_browsing_context_is_open());

        // 3. Try to handle any user prompts with session.
        handle_any_user_prompts([this, id = *id_value]() {
            Web::HTML::TemporaryExecutionContext execution_context { current_browsing_context().active_document()->relevant_settings_object() };

            // 4. Let window be the associated window of session's current browsing context's active document.
            auto window = current_browsing_context().active_document()->window()->window();

            // 5. If id is not a supported property index of window, return error with error code no such frame.
            auto property = window->get(id);

            if (property.is_error() || !property.value().is_object() || !is<Web::HTML::WindowProxy>(property.value().as_object())) {
                driver_execution_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchFrame, MUST(String::formatted("Frame ID {} not found", id))));
                return;
            }

            // 6. Let child window be the WindowProxy object obtained by calling window.[[GetOwnProperty]] (id).
            auto const& child_window = static_cast<Web::HTML::WindowProxy const&>(property.value().as_object());

            // 7. Set the current browsing context with session and child window's browsing context.
            auto child_browsing_context = child_window.associated_browsing_context();
            if (!child_browsing_context) {
                driver_execution_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchFrame, MUST(String::formatted("Frame ID {} not found", id))));
                return;
            }
            set_current_browsing_context(*child_browsing_context);

            driver_execution_complete(JsonValue {});
        });
    }

    // -> id represents a web element
    else if (id.is_object()) {
        auto element_id = Web::WebDriver::extract_web_element_reference(id.as_object());

        // 1. If session's current browsing context is no longer open, return error with error code no such window.
        TRY(ensure_current_browsing_context_is_open());

        // 2. Try to handle any user prompts with session.
        handle_any_user_prompts([this, element_id = move(element_id)]() {
            // 3. Let element be the result of trying to get a known element with session and id.
            auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

            // 4. If element is not a frame or iframe element, return error with error code no such frame.
            if (!is<Web::HTML::HTMLFrameElement>(*element) && !is<Web::HTML::HTMLIFrameElement>(*element)) {
                driver_execution_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchFrame, "element is not a frame"sv));
                return;
            }

            // 5. Set the current browsing context with session and element's content navigable's active browsing context.
            auto& navigable_container = static_cast<Web::HTML::NavigableContainer&>(*element);
            set_current_browsing_context(*as<Web::HTML::LocalNavigable>(*navigable_container.content_navigable()).active_browsing_context());

            driver_execution_complete(JsonValue {});
        });
    }

    // FIXME: 4. Update any implementation-specific state that would result from the user selecting session's current browsing context for interaction, without altering OS-level focus.

    // 5. Return success with data null
    return JsonValue {};
}

// 11.7 Switch To Parent Frame, https://w3c.github.io/webdriver/#dfn-switch-to-parent-frame
Web::WebDriver::Response WebDriverConnection::switch_to_parent_frame(JsonValue)
{
    // 1. If session's current browsing context is already the top-level browsing context:
    if (GC::Ref { current_browsing_context() } == current_top_level_browsing_context()) {
        // 1. If session's current browsing context is no longer open, return error with error code no such window.
        TRY(ensure_current_browsing_context_is_open());

        // 2. Return success with data null.
        driver_execution_complete(JsonValue {});
        return JsonValue {};
    }

    // 2. If session's current parent browsing context is no longer open, return error with error code no such window.
    TRY(Web::WebDriver::ensure_browsing_context_is_open(current_parent_browsing_context()));

    // 3. Try to handle any user prompts with session.
    handle_any_user_prompts([this]() {
        // 4. If session's current parent browsing context is not null, set the current browsing context with session and
        //    current parent browsing context.
        if (auto parent_browsing_context = current_parent_browsing_context())
            set_current_browsing_context(*parent_browsing_context);

        // FIXME: 5. Update any implementation-specific state that would result from the user selecting session's current browsing context for interaction, without altering OS-level focus.

        // 6. Return success with data null.
        driver_execution_complete(JsonValue {});
    });

    return JsonValue {};
}

// 11.8.1 Get Window Rect, https://w3c.github.io/webdriver/#dfn-get-window-rect
Web::WebDriver::Response WebDriverConnection::get_window_rect()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 3. Return success with data set to the WindowRect object for the current top-level browsing context.
        auto serialized_rect = serialize_rect(compute_window_rect(current_top_level_browsing_context()->page()));
        driver_execution_complete(move(serialized_rect));
    });

    return JsonValue {};
}

// 11.8.2 Set Window Rect, https://w3c.github.io/webdriver/#dfn-set-window-rect
Web::WebDriver::Response WebDriverConnection::set_window_rect(JsonValue payload)
{
    if (!payload.is_object())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Payload is not a JSON object"sv);

    auto const& properties = payload.as_object();

    auto resolve_property = [](auto name, auto const& property, double min, double max) -> ErrorOr<Optional<double>, Web::WebDriver::Error> {
        if (property.is_null())
            return OptionalNone {};

        auto value = property.get_double_with_precision_loss();
        if (!value.has_value())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Property '{}' is not a Number", name)));
        if (*value < min)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Property '{}' value {} exceeds the minimum allowed value {}", name, *value, min)));
        if (*value > max)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Property '{}' value {} exceeds the maximum allowed value {}", name, *value, max)));

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
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 9. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, x, y, width, height]() {
        // 10. Fully exit fullscreen.
        current_top_level_browsing_context()->active_document()->fully_exit_fullscreen();

        // 11. Restore the window.
        restore_the_window(GC::create_function(current_top_level_browsing_context()->heap(), [this, x, y, width, height]() {
            auto& page = current_top_level_browsing_context()->page();

            // 11. If width and height are not null:
            if (width.has_value() && height.has_value()) {
                // a. Set the width, in CSS pixels, of the operating system window containing the current top-level browsing context, including any browser chrome and externally drawn window decorations to a value that is as close as possible to width.
                // b. Set the height, in CSS pixels, of the operating system window containing the current top-level browsing context, including any browser chrome and externally drawn window decorations to a value that is as close as possible to height.
                auto request_id = register_window_rect_request();
                page.client().page_did_request_resize_window({ *width, *height }, request_id);
            }

            // 12. If x and y are not null:
            if (x.has_value() && y.has_value()) {
                // a. Run the implementation-specific steps to set the position of the operating system level window containing the current top-level browsing context to the position given by the x and y coordinates.
                auto request_id = register_window_rect_request();
                page.client().page_did_request_reposition_window({ *x, *y }, request_id);
            }

            if (m_pending_window_rect_requests.is_empty())
                driver_execution_complete(serialize_rect(compute_window_rect(page)));
        }));
    });

    // 14. Return success with data set to the WindowRect object for the current top-level browsing context.
    return JsonValue {};
}

// 11.8.3 Maximize Window, https://w3c.github.io/webdriver/#dfn-maximize-window
Web::WebDriver::Response WebDriverConnection::maximize_window()
{
    // 1. If the remote end does not support the Maximize Window command for the current top-level browsing context for any reason, return error with error code unsupported operation.

    // 2. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 3. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 4. Fully exit fullscreen.
        current_top_level_browsing_context()->active_document()->fully_exit_fullscreen();

        // 5. Restore the window.
        restore_the_window(GC::create_function(current_top_level_browsing_context()->heap(), [this]() {
            // 6. Maximize the window of the current top-level browsing context.
            maximize_the_window();
        }));
    });

    // 7. Return success with data set to the WindowRect object for the current top-level browsing context.
    return JsonValue {};
}

// 11.8.4 Minimize Window, https://w3c.github.io/webdriver/#minimize-window
Web::WebDriver::Response WebDriverConnection::minimize_window()
{
    // 1. If the remote end does not support the Minimize Window command for the current top-level browsing context for any reason, return error with error code unsupported operation.

    // 2. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 3. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 4. Fully exit fullscreen.
        current_top_level_browsing_context()->active_document()->fully_exit_fullscreen();

        // 5. Iconify the window.
        iconify_the_window(GC::create_function(current_top_level_browsing_context()->heap(), [this]() {
            auto& page = current_top_level_browsing_context()->page();
            driver_execution_complete(serialize_rect(compute_window_rect(page)));
        }));
    });

    // 6. Return success with data set to the WindowRect object for the current top-level browsing context.
    return JsonValue {};
}

// 11.8.5 Fullscreen Window, https://w3c.github.io/webdriver/#dfn-fullscreen-window
Web::WebDriver::Response WebDriverConnection::fullscreen_window()
{
    // 1. If the remote end does not support fullscreen return error with error code unsupported operation.

    // 2. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 3. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 4. Restore the window.
        restore_the_window(GC::create_function(current_top_level_browsing_context()->heap(), [this]() {
            auto* document = current_top_level_browsing_context()->active_document();

            Web::HTML::TemporaryExecutionContext execution_context { document->relevant_settings_object(), Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 5. Call fullscreen an element with session's current top-level browsing context's active document's
            //    document element.
            // FIXME: Spec issue: invoking "fullscreen an element" would not actually fullscreen the document.
            //        https://github.com/w3c/webdriver/issues/1888
            auto& realm = document->relevant_settings_object().realm();
            auto promise = Web::WebIDL::create_promise(realm);
            document->document_element()->request_fullscreen(promise, Web::DOM::Element::FullscreenRequester::WebDriver);

            Web::WebIDL::upon_fulfillment(promise, GC::create_function(GC::Heap::the(), [this, document](JS::Value) -> Web::WebIDL::ExceptionOr<JS::Value> {
                driver_execution_complete(serialize_rect(compute_window_rect(document->page())));

                return JS::js_undefined();
            }));

            Web::WebIDL::upon_rejection(promise, GC::create_function(GC::Heap::the(), [this, document](JS::Value) -> Web::WebIDL::ExceptionOr<JS::Value> {
                driver_execution_complete(serialize_rect(compute_window_rect(document->page())));

                return JS::js_undefined();
            }));
        }));
    });

    // 6. Return success with data set to the WindowRect object for the current top-level browsing context.
    return JsonValue {};
}

// Extension Consume User Activation, https://html.spec.whatwg.org/multipage/interaction.html#user-activation-user-agent-automation
Web::WebDriver::Response WebDriverConnection::consume_user_activation()
{
    // FIXME: This should probably be in the spec steps
    // If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 1. Let window be the current browsing context's active window.
    auto* window = current_browsing_context().active_window();

    // 2. Let consume be true if window has transient activation; otherwise false.
    bool consume = window->has_transient_activation();

    // 3. If consume is true, then consume user activation of window.
    if (consume)
        window->consume_user_activation();

    // 4. Return success with data consume.
    return JsonValue { consume };
}

static Web::WebDriver::Response extract_first_element(Web::WebDriver::Response result)
{
    auto array = TRY(result);
    VERIFY(array.is_array());

    if (!array.as_array().is_empty())
        return array.as_array().take(0);

    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "The requested element does not exist"sv);
}

// 12.3.2 Find Element, https://w3c.github.io/webdriver/#dfn-find-element
Web::WebDriver::Response WebDriverConnection::find_element(JsonValue payload)
{
    // 1. Let location strategy be the result of getting a property named "using" from parameters.
    auto location_strategy_string = TRY(Web::WebDriver::get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error
    //    code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Location strategy '{}' is invalid", location_strategy_string)));

    // 3. Let selector be the result of getting a property named "value" from parameters.
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(Web::WebDriver::get_property(payload, "value"sv));

    // 5. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 6. Try to handle any user prompts with session.
    handle_any_user_prompts([this, location_strategy, selector = move(selector)]() mutable {
        auto get_start_node = GC::create_function(GC::Heap::the(), [this]() -> ErrorOr<GC::Ref<Web::DOM::ParentNode>, Web::WebDriver::Error> {
            // 7. Let start node be session's current browsing context's document element.
            auto* start_node = current_browsing_context().active_document();

            // 8. If start node is null, return error with error code no such element.
            if (!start_node)
                return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "document element does not exist"sv);

            return *start_node;
        });

        // 9. Let result be the result of trying to Find with session, start node, location strategy, and selector.
        find(*location_strategy, move(selector), get_start_node, GC::create_function(GC::Heap::the(), [this](Web::WebDriver::Response result) {
            // 10. If result is empty, return error with error code no such element. Otherwise, return the first element of result.
            driver_execution_complete(extract_first_element(move(result)));
        }));
    });

    return JsonValue {};
}

// 12.3.3 Find Elements, https://w3c.github.io/webdriver/#dfn-find-elements
Web::WebDriver::Response WebDriverConnection::find_elements(JsonValue payload)
{
    // 1. Let location strategy be the result of getting a property named "using" from parameters.
    auto location_strategy_string = TRY(Web::WebDriver::get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error
    //    code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Location strategy '{}' is invalid", location_strategy_string)));

    // 3. Let selector be the result of getting a property named "value" from parameters.
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(Web::WebDriver::get_property(payload, "value"sv));

    // 5. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 6. Try to handle any user prompts with session.
    handle_any_user_prompts([this, location_strategy, selector = move(selector)]() mutable {
        auto get_start_node = GC::create_function(GC::Heap::the(), [this]() -> ErrorOr<GC::Ref<Web::DOM::ParentNode>, Web::WebDriver::Error> {
            // 7. Let start node be session's current browsing context's document element.
            auto* start_node = current_browsing_context().active_document();

            // 8. If start node is null, return error with error code no such element.
            if (!start_node)
                return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "document element does not exist"sv);

            return *start_node;
        });

        // 9. Return the result of trying to Find with session, start node, location strategy, and selector.
        find(*location_strategy, move(selector), get_start_node, GC::create_function(GC::Heap::the(), [this](Web::WebDriver::Response result) {
            driver_execution_complete(move(result));
        }));
    });

    return JsonValue {};
}

// 12.3.4 Find Element From Element, https://w3c.github.io/webdriver/#dfn-find-element-from-element
Web::WebDriver::Response WebDriverConnection::find_element_from_element(JsonValue payload, String element_id)
{
    // 1. Let location strategy be the result of getting a property named "using" from parameters.
    auto location_strategy_string = TRY(Web::WebDriver::get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Location strategy '{}' is invalid", location_strategy_string)));

    // 3. Let selector be the result of getting a property named "value" from parameters.
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(Web::WebDriver::get_property(payload, "value"sv));

    // 5. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 6. Try to handle any user prompts with session.
    handle_any_user_prompts([this, element_id, location_strategy, selector = move(selector)]() mutable {
        auto get_start_node = GC::create_function(GC::Heap::the(), [this, element_id]() -> ErrorOr<GC::Ref<Web::DOM::ParentNode>, Web::WebDriver::Error> {
            // 7. Let start node be the result of trying to get a known element with session and URL variables["element id"].
            return Web::WebDriver::get_known_element(current_browsing_context(), element_id);
        });

        // 8. Let result be the value of trying to Find with session, start node, location strategy, and selector.
        find(*location_strategy, move(selector), get_start_node, GC::create_function(GC::Heap::the(), [this](Web::WebDriver::Response result) {
            // 9. If result is empty, return error with error code no such element. Otherwise, return the first element of result.
            driver_execution_complete(extract_first_element(move(result)));
        }));
    });

    return JsonValue {};
}

// 12.3.5 Find Elements From Element, https://w3c.github.io/webdriver/#dfn-find-elements-from-element
Web::WebDriver::Response WebDriverConnection::find_elements_from_element(JsonValue payload, String element_id)
{
    // 1. Let location strategy be the result of getting a property named "using" from parameters.
    auto location_strategy_string = TRY(Web::WebDriver::get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Location strategy '{}' is invalid", location_strategy_string)));

    // 3. Let selector be the result of getting a property named "value" from parameters.
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(Web::WebDriver::get_property(payload, "value"sv));

    // 5. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 6. Try to handle any user prompts with session.
    handle_any_user_prompts([this, element_id, location_strategy, selector = move(selector)]() mutable {
        auto get_start_node = GC::create_function(GC::Heap::the(), [this, element_id]() -> ErrorOr<GC::Ref<Web::DOM::ParentNode>, Web::WebDriver::Error> {
            // 7. Let start node be the result of trying to get a known element with session and URL variables["element id"].
            return Web::WebDriver::get_known_element(current_browsing_context(), element_id);
        });

        // 8. Return the result of trying to Find with session, start node, location strategy, and selector.
        find(*location_strategy, move(selector), get_start_node, GC::create_function(GC::Heap::the(), [this](Web::WebDriver::Response result) {
            driver_execution_complete(move(result));
        }));
    });

    return JsonValue {};
}

// 12.3.6 Find Element From Shadow Root, https://w3c.github.io/webdriver/#find-element-from-shadow-root
Web::WebDriver::Response WebDriverConnection::find_element_from_shadow_root(JsonValue payload, String shadow_id)
{
    // 1. Let location strategy be the result of getting a property called "using".
    auto location_strategy_string = TRY(Web::WebDriver::get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Location strategy '{}' is invalid", location_strategy_string)));

    // 3. Let selector be the result of getting a property called "value".
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(Web::WebDriver::get_property(payload, "value"sv));

    // 5. If the session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 6. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, shadow_id, location_strategy, selector = move(selector)]() mutable {
        auto get_start_node = GC::create_function(GC::Heap::the(), [this, shadow_id]() -> ErrorOr<GC::Ref<Web::DOM::ParentNode>, Web::WebDriver::Error> {
            // 7. Let start node be the result of trying to get a known shadow root with session and URL variables["shadow id"].
            return Web::WebDriver::get_known_shadow_root(current_browsing_context(), shadow_id);
        });

        // 8. Let result be the value of trying to Find with session, start node, location strategy, and selector.
        find(*location_strategy, move(selector), get_start_node, GC::create_function(GC::Heap::the(), [this](Web::WebDriver::Response result) {
            // 9. If result is empty, return error with error code no such element. Otherwise, return the first element of result.
            driver_execution_complete(extract_first_element(move(result)));
        }));
    });

    return JsonValue {};
}

// 12.3.7 Find Elements From Shadow Root, https://w3c.github.io/webdriver/#find-elements-from-shadow-root
Web::WebDriver::Response WebDriverConnection::find_elements_from_shadow_root(JsonValue payload, String shadow_id)
{
    // 1. Let location strategy be the result of getting a property called "using".
    auto location_strategy_string = TRY(Web::WebDriver::get_property(payload, "using"sv));
    auto location_strategy = Web::WebDriver::location_strategy_from_string(location_strategy_string);

    // 2. If location strategy is not present as a keyword in the table of location strategies, return error with error code invalid argument.
    if (!location_strategy.has_value())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("Location strategy '{}' is invalid", location_strategy_string)));

    // 3. Let selector be the result of getting a property called "value".
    // 4. If selector is undefined, return error with error code invalid argument.
    auto selector = TRY(Web::WebDriver::get_property(payload, "value"sv));

    // 5. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 6. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, shadow_id, location_strategy, selector = move(selector)]() mutable {
        auto get_start_node = GC::create_function(GC::Heap::the(), [this, shadow_id]() -> ErrorOr<GC::Ref<Web::DOM::ParentNode>, Web::WebDriver::Error> {
            // 7. Let start node be the result of trying to get a known shadow root with session and URL variables["shadow id"].
            return Web::WebDriver::get_known_shadow_root(current_browsing_context(), shadow_id);
        });

        // 8. Return the result of trying to Find with session, start node, location strategy, and selector.
        find(*location_strategy, move(selector), get_start_node, GC::create_function(GC::Heap::the(), [this](Web::WebDriver::Response result) {
            driver_execution_complete(move(result));
        }));
    });

    return JsonValue {};
}

// 12.3.8 Get Active Element, https://w3c.github.io/webdriver/#get-active-element
Web::WebDriver::Response WebDriverConnection::get_active_element()
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 3. Let active element be the active element of the current browsing context’s document element.
        auto const* active_element = current_browsing_context().active_document()->active_element();

        // 4. If active element is a non-null element, return success with data set to web element reference object for active element.
        //    Otherwise, return error with error code no such element.
        if (active_element) {
            auto serialized = Web::WebDriver::web_element_reference_object(current_browsing_context(), *active_element);
            driver_execution_complete({ move(serialized) });
            return;
        }

        driver_execution_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchElement, "The current document does not have an active element"sv));
    });

    return JsonValue {};
}

// 12.3.9 Get Element Shadow Root, https://w3c.github.io/webdriver/#get-element-shadow-root
Web::WebDriver::Response WebDriverConnection::get_element_shadow_root(String element_id)
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        // 3. Let element be the result of trying to get a known element with session and URL variables[element id].
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Let shadow root be element's shadow root.
        auto shadow_root = element->shadow_root();

        // 5. If shadow root is null, return error with error code no such shadow root.
        if (!shadow_root) {
            driver_execution_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchShadowRoot, MUST(String::formatted("Element with ID '{}' does not have a shadow root", element_id))));
            return;
        }

        // 6. Let serialized be the shadow root reference object for session and shadow root.
        auto serialized = Web::WebDriver::shadow_root_reference_object(current_browsing_context(), *shadow_root);

        // 7. Return success with data serialized.
        driver_execution_complete({ move(serialized) });
    });

    return JsonValue {};
}

// 12.4.1 Is Element Selected, https://w3c.github.io/webdriver/#dfn-is-element-selected
Web::WebDriver::Response WebDriverConnection::is_element_selected(String element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        // 3. Let element be the result of trying to get a known connected element with url variable element id.
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

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
        driver_execution_complete({ selected });
    });

    return JsonValue {};
}

// 12.4.2 Get Element Attribute, https://w3c.github.io/webdriver/#dfn-get-element-attribute
Web::WebDriver::Response WebDriverConnection::get_element_attribute(String element_id, String name)
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Try to handle any user prompts with session.
    handle_any_user_prompts([this, element_id = move(element_id), name]() {
        // 3. Let element be the result of trying to get a known element with session and URL variables' element id.
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Let name be URL variables["name"].
        // 5. Let result be the result of the first matching condition:
        String result {};

        auto const utf16_name = Utf16FlyString::from_utf8(name);

        // -> If name is a boolean attribute
        if (Web::HTML::is_boolean_attribute(utf16_name)) {
            // "true" (string) if the element hasAttribute() with name, otherwise null.
            if (element->has_attribute(utf16_name))
                result = "true"_string;
        }
        // -> Otherwise
        else {
            // The result of getting an attribute by name name.
            if (auto attr = element->get_attribute(utf16_name); attr.has_value())
                result = attr.release_value().to_utf8();
        }

        // 5. Return success with data result.
        driver_execution_complete({ move(result) });
    });

    return JsonValue {};
}

// 12.4.3 Get Element Property, https://w3c.github.io/webdriver/#dfn-get-element-property
Web::WebDriver::Response WebDriverConnection::get_element_property(String element_id, String name)
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Try to handle any user prompts with session.
    handle_any_user_prompts([this, element_id = move(element_id), name]() {
        Web::WebDriver::Response result { JsonValue {} };

        // 3. Let element be the result of trying to get a known element with session and URL variables' element id.
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Let name URL variables["name"].
        // 5. Let property be the result of calling the Object.[[GetProperty]](name) on element.
        Web::HTML::TemporaryExecutionContext execution_context { current_browsing_context().active_document()->relevant_settings_object() };

        auto& realm = Web::HTML::relevant_realm(*current_browsing_context().active_document());
        auto wrapped_element = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, element);
        if (auto property_or_error = wrapped_element->get(Utf16FlyString::from_utf8(name)); !property_or_error.is_throw_completion()) {
            auto property = property_or_error.release_value();

            // 6. Let result be the value of property if not undefined, or null.
            if (!property.is_undefined())
                result = Web::WebDriver::json_clone(current_browsing_context(), property);
        }

        // 7. Return success with data result.
        driver_execution_complete(move(result));
    });

    return JsonValue {};
}

// 12.4.4 Get Element CSS Value, https://w3c.github.io/webdriver/#dfn-get-element-css-value
Web::WebDriver::Response WebDriverConnection::get_element_css_value(String element_id, String name)
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Try to handle any user prompts with session.
    handle_any_user_prompts([this, element_id = move(element_id), name]() {
        // 3. Let element be the result of trying to get a known element with URL variables["element id"].
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Let computed value be the result of the first matching condition:
        String computed_value;

        // -> session's current browsing context's active document's type is not "xml"
        if (auto* document = current_browsing_context().active_document(); !document->is_xml_document()) {
            document->update_style();

            // computed value of parameter URL variables["property name"] from element's style declarations.
            if (auto property = Web::CSS::PropertyNameAndID::from_name(Utf16FlyString::from_utf8(name)); property.has_value()) {
                if (property->is_custom_property()) {
                    if (auto data = element->custom_property_data({}); data) {
                        if (auto const* style_property = data->get(property->name()))
                            computed_value = style_property->value->to_string(Web::CSS::SerializationMode::Normal);
                    }
                } else if (auto computed_values = element->computed_style()) {
                    computed_value = computed_values->computed_style_value(property->id())->to_string(Web::CSS::SerializationMode::Normal);
                }
            }
        }
        // -> Otherwise
        //     "" (empty string)

        // 5. Return success with data computed value.
        driver_execution_complete({ move(computed_value) });
    });

    return JsonValue {};
}

// 12.4.5 Get Element Text, https://w3c.github.io/webdriver/#dfn-get-element-text
Web::WebDriver::Response WebDriverConnection::get_element_text(String element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        // 3. Let element be the result of trying to get a known connected element with url variable element id.
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Let rendered text be the result of performing implementation-specific steps whose result is exactly the
        //    same as the result of a Function.[[Call]](null, element) with bot.dom.getVisibleText as the this value.
        auto rendered_text = Web::WebDriver::element_rendered_text(element);

        // 5. Return success with data rendered text.
        driver_execution_complete({ move(rendered_text) });
    });

    return JsonValue {};
}

// 12.4.6 Get Element Tag Name, https://w3c.github.io/webdriver/#dfn-get-element-tag-name
Web::WebDriver::Response WebDriverConnection::get_element_tag_name(String element_id)
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Try to handle any user prompts with session.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        // 3. Let element be the result of trying to get a known element with URL variables["element id"].
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Let qualified name be the result of getting element's tagName IDL attribute.
        // FIXME: Spec-issue: The tagName attribute is uppercase, but lowercase is used in other engines.
        //        https://github.com/web-platform-tests/wpt/issues/16830
        auto qualified_name = element->local_name();

        // 5. Return success with data qualified name.
        driver_execution_complete({ qualified_name.view().to_utf8_but_should_be_ported_to_utf16() });
    });

    return JsonValue {};
}

// 12.4.7 Get Element Rect, https://w3c.github.io/webdriver/#dfn-get-element-rect
Web::WebDriver::Response WebDriverConnection::get_element_rect(String element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        // 3. Let element be the result of trying to get a known connected element with url variable element id.
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Calculate the absolute position of element and let it be coordinates.
        // 5. Let rect be element’s bounding rectangle.
        auto rect = calculate_absolute_rect_of_element(*element);

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
        driver_execution_complete(move(body));
    });

    return JsonValue {};
}

// 12.4.8 Is Element Enabled, https://w3c.github.io/webdriver/#dfn-is-element-enabled
Web::WebDriver::Response WebDriverConnection::is_element_enabled(String element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        // 3. Let element be the result of trying to get a known connected element with url variable element id.
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Let enabled be a boolean initially set to true if the current browsing context’s active document’s type is not "xml".
        // 5. Otherwise, let enabled to false and jump to the last step of this algorithm.
        bool enabled = !current_browsing_context().active_document()->is_xml_document();

        // 6. Set enabled to false if a form control is disabled.
        if (auto* form_associated_element = as_if<Web::HTML::FormAssociatedElement>(*element); form_associated_element && enabled) {
            enabled = form_associated_element->enabled();
        }

        // 7. Return success with data enabled.
        driver_execution_complete({ enabled });
    });

    return JsonValue {};
}

// 12.4.9 Get Computed Role, https://w3c.github.io/webdriver/#dfn-get-computed-role
Web::WebDriver::Response WebDriverConnection::get_computed_role(String element_id)
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        // 3. Let element be the result of trying to get a known connected element with url variable element id.
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Let role be the result of computing the WAI-ARIA role of element.
        auto role = element->role_or_default();

        // 5. Return success with data role.
        if (role.has_value()) {
            driver_execution_complete({ Web::ARIA::role_name(*role).to_utf8() });
            return;
        }
        driver_execution_complete(JsonValue {});
    });

    return JsonValue {};
}

// 12.4.10 Get Computed Label, https://w3c.github.io/webdriver/#get-computed-label
Web::WebDriver::Response WebDriverConnection::get_computed_label(String element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        // 3. Let element be the result of trying to get a known element with url variable element id.
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Let label be the result of a Accessible Name and Description Computation for the Accessible Name of the element.
        auto label = element->accessible_name(element->document()).release_value_but_fixme_should_propagate_errors();

        // 5. Return success with data label.
        driver_execution_complete({ label.to_utf8() });
    });

    return JsonValue {};
}

// 12.5.1 Element Click, https://w3c.github.io/webdriver/#element-click
Web::WebDriver::Response WebDriverConnection::element_click(String element_id)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts and return its value if it is an error.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        WEBDRIVER_TRY(element_click_impl(element_id));
    });

    return JsonValue {};
}

Web::WebDriver::Response WebDriverConnection::element_click_impl(StringView element_id)
{
    // 3. Let element be the result of trying to get a known element with element id.
    auto element = TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

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
    scroll_element_into_view(*element_container);

    auto paint_tree = Web::WebDriver::pointer_interactable_tree(current_browsing_context(), *element_container);

    // 6. If element’s container is still not in view, return error with error code element not interactable.
    if (!Web::WebDriver::is_element_in_view(paint_tree, *element_container))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ElementNotInteractable, "Could not bring element into view"sv);

    // 7. If element’s container is obscured by another element, return error with error code element click intercepted.
    if (Web::WebDriver::is_element_obscured(paint_tree, *element_container))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ElementClickIntercepted, "Element is obscured by another element"sv);

    auto on_complete = GC::create_function(GC::Heap::the(), [this](Web::WebDriver::Response result) {
        // 9. Wait until the user agent event loop has spun enough times to process the DOM events generated by the
        //    previous step.
        m_action_executor = nullptr;

        // FIXME: 10. Perform implementation-defined steps to allow any navigations triggered by the click to start.

        // 11. Try to wait for navigation to complete.
        wait_for_navigation_to_complete(GC::create_function(GC::Heap::the(), [this, result = move(result)](Web::WebDriver::Response navigation_result) mutable {
            WEBDRIVER_TRY(navigation_result);

            // FIXME: 12. Try to run the post-navigation checks.

            driver_execution_complete(move(result));
        }));
    });

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

        Web::HTML::queue_a_task(Web::HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(GC::Heap::the(), [on_complete]() {
            on_complete->function()(JsonValue {});
        }));
    }
    // -> Otherwise
    else {
        // 1. Let input state be the result of get the input state given current session and current top-level
        //    browsing context.
        auto& input_state = Web::WebDriver::get_input_state(*current_top_level_browsing_context());

        // 2. Let actions options be a new actions options with the is element origin steps set to represents a web
        //    element, and the get element origin steps set to get a WebElement origin.
        Web::WebDriver::ActionsOptions actions_options {
            .is_element_origin = &Web::WebDriver::represents_a_web_element,
            .get_element_origin = &Web::WebDriver::get_web_element_origin,
        };

        // 3. Let input id be a the result of generating a UUID.
        auto input_id = Web::Crypto::generate_random_uuid();

        // 4. Let source be the result of create an input source with input state, and "pointer".
        auto source = Web::WebDriver::create_input_source(input_state, Web::WebDriver::InputSourceType::Pointer, Web::WebDriver::PointerInputSource::Subtype::Mouse);

        // 5. Add an input source with input state, input id and source.
        Web::WebDriver::add_input_source(input_state, input_id, move(source));

        // 6. Let click point be the element’s in-view center point.
        // FIXME: Spec-issue: This parameter is unused. Note that it would not correct to set the mouse move action
        //        position to this click point. The [0,0] specified below is ultimately interpreted as an offset from
        //        the element's center position.
        //        https://github.com/w3c/webdriver/issues/1563

        // 7. Let pointer move action be an action object constructed with arguments input id, "pointer", and "pointerMove".
        Web::WebDriver::ActionObject pointer_move_action { input_id, Web::WebDriver::InputSourceType::Pointer, Web::WebDriver::ActionObject::Subtype::PointerMove };

        // 8. Set a property x to 0 on pointer move action.
        // 9. Set a property y to 0 on pointer move action.
        pointer_move_action.pointer_move_fields().position = { 0, 0 };

        // 10. Set a property origin to element on pointer move action.
        pointer_move_action.pointer_move_fields().origin = Web::WebDriver::get_or_create_a_web_element_reference(current_browsing_context(), *element);

        // 11. Let pointer down action be an action object constructed with arguments input id, "pointer", and "pointerDown".
        Web::WebDriver::ActionObject pointer_down_action { input_id, Web::WebDriver::InputSourceType::Pointer, Web::WebDriver::ActionObject::Subtype::PointerDown };

        // 12. Set a property button to 0 on pointer down action.
        pointer_down_action.pointer_up_down_fields().button = Web::UIEvents::button_code_to_mouse_button(0);

        // 13. Let pointer up action be an action object constructed with arguments input id, "pointer", and "pointerUp" as arguments.
        Web::WebDriver::ActionObject pointer_up_action { input_id, Web::WebDriver::InputSourceType::Pointer, Web::WebDriver::ActionObject::Subtype::PointerUp };

        // 14. Set a property button to 0 on pointer up action.
        pointer_up_action.pointer_up_down_fields().button = Web::UIEvents::button_code_to_mouse_button(0);

        // 15. Let actions be the list «pointer move action, pointer down action, pointer up action».
        Vector actions { move(pointer_move_action), move(pointer_down_action), move(pointer_up_action) };

        // 16. Dispatch a list of actions with input state, actions, current browsing context, and actions options.
        m_action_executor = Web::WebDriver::dispatch_list_of_actions(input_state, move(actions), current_browsing_context(), move(actions_options), GC::create_function(GC::Heap::the(), [on_complete, &input_state, input_id = move(input_id)](Web::WebDriver::Response result) {
            // 17. Remove an input source with input state and input id.
            Web::WebDriver::remove_input_source(input_state, input_id);

            on_complete->function()(move(result));
        }));
    }

    // 13. Return success with data null.
    return JsonValue {};
}

// 12.5.2 Element Clear, https://w3c.github.io/webdriver/#dfn-element-clear
Web::WebDriver::Response WebDriverConnection::element_clear(String element_id)
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Try to handle any user prompts with session.
    handle_any_user_prompts([this, element_id = move(element_id)]() {
        driver_execution_complete(element_clear_impl(element_id));
    });

    return JsonValue {};
}

Web::WebDriver::Response WebDriverConnection::element_clear_impl(StringView element_id)
{
    // https://w3c.github.io/webdriver/#dfn-clear-a-content-editable-element
    auto clear_content_editable_element = [&](Web::DOM::Element& element) {
        // 1. If element's innerHTML IDL attribute is an empty string do nothing and return.
        if (auto result = element.inner_html(); result.is_error() || result.value().is_empty())
            return;

        // 2. Run the focusing steps for element.
        Web::HTML::run_focusing_steps(&element);

        // 3. Set element's innerHTML IDL attribute to an empty string.
        (void)element.set_inner_html(""_utf16);

        // 4. Run the unfocusing steps for the element.
        Web::HTML::run_unfocusing_steps(&element);
    };

    // https://w3c.github.io/webdriver/#dfn-clear-a-resettable-element
    auto clear_resettable_element = [&](Web::DOM::Element& element) {
        auto& form_associated_element = as<Web::HTML::FormAssociatedElement>(element);

        // 1. Let empty be the result of the first matching condition:
        auto empty = [&]() {
            // -> element is an input element whose type attribute is in the File Upload state
            //    True if the list of selected files has a length of 0, and false otherwise
            if (is<Web::HTML::HTMLInputElement>(element)) {
                auto& input_element = static_cast<Web::HTML::HTMLInputElement&>(element);

                if (input_element.type_state() == Web::HTML::HTMLInputElement::TypeAttributeState::FileUpload)
                    return input_element.files()->length() == 0;
            }

            // -> otherwise
            //    True if its value IDL attribute is an empty string, and false otherwise.
            return form_associated_element.form_value().is_empty();
        }();

        // 2. If element is a candidate for constraint validation it satisfies its constraints, and empty is true,
        //    abort these substeps.
        // FIXME: Implement constraint validation.
        if (empty)
            return;

        // 3. Invoke the focusing steps for element.
        Web::HTML::run_focusing_steps(&element);

        // 4. Invoke the clear algorithm for element.
        form_associated_element.clear_algorithm();

        // 5. Invoke the unfocusing steps for the element.
        Web::HTML::run_unfocusing_steps(&element);
    };

    // 3. Let element be the result of trying to get a known element with session and element id.
    auto element = TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

    // 4. If element is not editable, return an error with error code invalid element state.
    if (!Web::WebDriver::is_element_editable(*element))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidElementState, "Element is not editable"sv);

    // 5. Scroll into view the element.
    scroll_element_into_view(*element);

    // FIXME: 6. Let timeout be session's session timeouts' implicit wait timeout.
    // FIXME: 7. Let timer be a new timer.
    // FIXME: 8. If timeout is not null:
    {
        // FIXME: 1. Start the timer with timer and timeout.
    }
    // FIXME: 9. Wait for element to become interactable, or timer's timeout fired flag to be set, whichever occurs first.

    // 10. If element is not interactable, return error with error code element not interactable.
    if (!Web::WebDriver::is_element_interactable(current_browsing_context(), *element))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ElementNotInteractable, "Element is not interactable"sv);

    // 11. Run the substeps of the first matching statement:
    // -> element is a mutable form control element
    if (Web::WebDriver::is_element_mutable_form_control(*element)) {
        // Invoke the steps to clear a resettable element.
        clear_resettable_element(*element);
    }
    // -> element is a mutable element
    else if (Web::WebDriver::is_element_mutable(*element)) {
        // Invoke the steps to clear a content editable element.
        clear_content_editable_element(*element);
    }
    // -> otherwise
    else {
        // Return error with error code invalid element state.
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidElementState, "Element is not editable"sv);
    }

    // 12. Return success with data null.
    return JsonValue {};
}

// 12.5.3 Element Send Keys, https://w3c.github.io/webdriver/#dfn-element-send-keys
Web::WebDriver::Response WebDriverConnection::element_send_keys(String element_id, JsonValue payload)
{
    // 1. Let text be the result of getting a property named "text" from parameters.
    // 2. If text is not a String, return an error with error code invalid argument.
    auto text = TRY(Web::WebDriver::get_property(payload, "text"sv));

    // 3. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 4. Try to handle any user prompts with session.
    handle_any_user_prompts([this, element_id = move(element_id), text = move(text)]() {
        WEBDRIVER_TRY(element_send_keys_impl(element_id, text));
    });

    return JsonValue {};
}

Web::WebDriver::Response WebDriverConnection::element_send_keys_impl(StringView element_id, String const& text)
{
    // 5. Let element be the result of trying to get a known element with session and URL variables[element id].
    auto element = TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

    // 6. Let file be true if element is input element in the file upload state, or false otherwise.
    auto file = is<Web::HTML::HTMLInputElement>(*element) && static_cast<Web::HTML::HTMLInputElement&>(*element).type_state() == Web::HTML::HTMLInputElement::TypeAttributeState::FileUpload;

    // 7. If file is false or the session's strict file interactability, is true run the following substeps:
    if (!file || m_strict_file_interactability) {
        // 1. Scroll into view the element.
        scroll_element_into_view(*element);

        // FIXME: 2. Let timeout be session's session timeouts' implicit wait timeout.
        // FIXME: 3. Let timer be a new timer.
        // FIXME: 4. If timeout is not null:
        {
            // FIXME: 1. Start the timer with timer and timeout.
        }
        // FIXME: 5. Wait for element to become keyboard-interactable, or timer's timeout fired flag to be set, whichever occurs first.

        // 6. If element is not keyboard-interactable, return error with error code element not interactable.
        if (!Web::WebDriver::is_element_keyboard_interactable(*element))
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ElementNotInteractable, "Element is not keyboard-interactable"sv);

        // 7. If element is not the active element run the focusing steps for the element.
        if (!element->is_the_active_element())
            Web::HTML::run_focusing_steps(element);
    }

    // 8. Run the substeps of the first matching condition:

    // -> file is true
    if (file) {
        auto& input_element = static_cast<Web::HTML::HTMLInputElement&>(*element);

        // 1. Let files be the result of splitting text on the newline (\n) character.
        auto files = MUST(text.split('\n'));

        // 2. If files is of 0 length, return an error with error code invalid argument.
        if (files.is_empty())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "File list is empty"sv);

        // 3. Let multiple equal the result of calling hasAttribute() with "multiple" on element.
        auto multiple = input_element.has_attribute(Web::HTML::AttributeNames::multiple);

        // 4. if multiple is false and the length of files is not equal to 1, return an error with error code invalid argument.
        if (!multiple && files.size() != 1)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Element does not accept multiple files"sv);

        // 5. Verify that each file given by the user exists. If any do not, return error with error code invalid argument.
        // 6. Complete implementation specific steps equivalent to setting the selected files on the input element. If
        //    multiple is true files are be appended to element's selected files.
        // NB: Each file is opened in the unsandboxed UI process, because the WebContent sandbox blocks this
        //     process from opening arbitrary paths.
        auto read_files_and_apply_selection = [connection = this, input_element = GC::make_root(input_element)](this auto const& self, Vector<String> paths, size_t index, Vector<Web::HTML::SelectedFile> selected_files) -> void {
            if (index < paths.size()) {
                auto path = paths[index].to_byte_string();

                Web::FileRequest file_request(path, [connection, self, paths = move(paths), index, selected_files = move(selected_files), path](ErrorOr<i32> file_descriptor_or_error) mutable {
                    auto contents_or_error = [&]() -> ErrorOr<ByteBuffer> {
                        auto opened_file = TRY(Core::File::adopt_fd(TRY(file_descriptor_or_error), Core::File::OpenMode::Read));
                        return opened_file->read_until_eof();
                    }();

                    if (contents_or_error.is_error()) {
                        connection->driver_execution_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, MUST(String::formatted("'{}' does not exist", path))));
                        return;
                    }

                    selected_files.append(Web::HTML::SelectedFile {
                        Utf16String::from_utf8(LexicalPath::basename(path)),
                        contents_or_error.release_value() });
                    self(move(paths), index + 1, move(selected_files));
                });

                connection->current_browsing_context().page().client().request_file(move(file_request));
                return;
            }

            input_element->did_select_files(selected_files, Web::HTML::HTMLInputElement::MultipleHandling::Append);

            // 7. Fire these events in order on element:
            //     1. input
            //     2. change
            // NOTE: These events are fired synchronously by `did_select_files`.
            connection->driver_execution_complete(JsonValue {});
        };

        read_files_and_apply_selection(move(files), 0, {});

        // 8. Return success with data null.
        return JsonValue {};
    }
    // -> element is a non-typeable form control
    else if (Web::WebDriver::is_element_non_typeable_form_control(*element)) {
        // 1. If element does not have an own property named value return an error with error code element not interactable
        if (!is<Web::HTML::HTMLInputElement>(*element))
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ElementNotInteractable, "Element does not have a property named 'value'"sv);

        auto& input_element = static_cast<Web::HTML::HTMLInputElement&>(*element);

        // 2. If element is not mutable return an error with error code element not interactable.
        if (!input_element.is_mutable())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ElementNotInteractable, "Element is immutable"sv);

        // 3. Set a property value to text on element.
        MUST(input_element.set_value(Utf16String::from_utf8(text)));

        // FIXME: 4. If element is suffering from bad input return an error with error code invalid argument.

        // 5. Return success with data null.
        driver_execution_complete(JsonValue {});
        return JsonValue {};
    }
    // -> element is content editable
    else if (is<Web::HTML::HTMLElement>(*element) && static_cast<Web::HTML::HTMLElement&>(*element).is_content_editable()) {
        // If element does not currently have focus, set the text insertion caret after any child content.
        auto* document = current_browsing_context().active_document();
        document->set_focused_area(element);
    }
    // -> otherwise
    else if (is<Web::HTML::FormAssociatedTextControlElement>(*element)) {
        Optional<Web::HTML::FormAssociatedTextControlElement&> target;

        if (is<Web::HTML::HTMLInputElement>(*element))
            target = static_cast<Web::HTML::HTMLInputElement&>(*element);
        else if (is<Web::HTML::HTMLTextAreaElement>(*element))
            target = static_cast<Web::HTML::HTMLTextAreaElement&>(*element);

        // NOTE: The spec doesn't dictate this, but these steps only make sense for form-associated text elements.
        if (target.has_value()) {
            // 1. If element does not currently have focus, let current text length be the length of element's API value.
            Optional<Web::WebIDL::UnsignedLong> current_text_length;
            if (!element->is_focused())
                current_text_length = target->relevant_value().length_in_code_units();

            // 2. Set the text insertion caret using set selection range using current text length for both the start
            //    and end parameters.
            (void)target->set_selection_range(current_text_length, current_text_length, {});
        }
    }

    // 9. Let input state be the result of get the input state with session and session's current top-level browsing context.
    auto& input_state = Web::WebDriver::get_input_state(*current_top_level_browsing_context());

    // 10. Let input id be a the result of generating a UUID.
    auto input_id = Web::Crypto::generate_random_uuid();

    // 11. Let source be the result of create an input source with input state, and "key".
    auto source = Web::WebDriver::create_input_source(input_state, Web::WebDriver::InputSourceType::Key, {});

    // 12. Add an input source with input state, input id and source.
    Web::WebDriver::add_input_source(input_state, input_id, move(source));

    // 13. Dispatch actions for a string with arguments input state, input id, and source, text, and session's current browsing context.
    m_action_executor = Web::WebDriver::dispatch_actions_for_a_string(input_state, input_id, source, text, current_browsing_context(), GC::create_function(GC::Heap::the(), [this, &input_state, input_id](Web::WebDriver::Response result) {
        m_action_executor = nullptr;

        // 14. Remove an input source with input state and input id.
        Web::WebDriver::remove_input_source(input_state, input_id);

        driver_execution_complete(move(result));
    }));

    // 15. Return success with data null.
    return JsonValue {};
}

// 13.1 Get Page Source, https://w3c.github.io/webdriver/#dfn-get-page-source
Web::WebDriver::Response WebDriverConnection::get_source()
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Try to handle any user prompts with session.
    handle_any_user_prompts([this]() {
        auto* document = current_browsing_context().active_document();
        Optional<Utf16String> source;

        // 3. Let source be the result of invoking the fragment serializing algorithm on a fictional node whose only
        //    child is the document element providing true for the require well-formed flag. If this causes an exception
        //    to be thrown, let source be null.
        if (auto result = document->document_element()->serialize_fragment(Web::HTML::RequireWellFormed::Yes, Web::DOM::FragmentSerializationMode::Outer); !result.is_error())
            source = result.release_value();

        // 4. Let source be the result of serializing to string session's current browsing context's active document,
        //    if source is null.
        if (!source.has_value())
            source = MUST(document->serialize_fragment(Web::HTML::RequireWellFormed::No));

        // 5. Return success with data source.
        driver_execution_complete({ source.release_value().to_utf8_but_should_be_ported_to_utf16() });
    });

    return JsonValue {};
}

// 13.2.1 Execute Script, https://w3c.github.io/webdriver/#dfn-execute-script
Web::WebDriver::Response WebDriverConnection::execute_script(JsonValue payload)
{
    // 1. Let body and arguments be the result of trying to extract the script arguments from a request with argument parameters.
    auto [body, arguments] = TRY(extract_the_script_arguments_from_a_request(payload));

    // 2. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 3. Handle any user prompts, and return its value if it is an error.
    handle_any_user_prompts([this, body = move(body), arguments = move(arguments)]() mutable {
        auto script_execution_id = m_script_execution_id_counter++;
        m_current_script_execution_id = script_execution_id;

        // 4. Let timeout be session's session timeouts' script timeout.
        auto timeout_ms = m_timeouts_configuration.script_timeout;

        // This handles steps 5 to 9 and produces the appropriate result type for the following steps.
        Web::WebDriver::execute_script(current_browsing_context(), move(body), move(arguments), timeout_ms, GC::create_function(GC::Heap::the(), [this, script_execution_id](Web::WebDriver::ExecutionResult result) {
            dbgln_if(WEBDRIVER_DEBUG, "Executing script returned: {}", result.value);
            handle_script_response(result, script_execution_id);
        }));
    });

    return JsonValue {};
}

// 13.2.2 Execute Async Script, https://w3c.github.io/webdriver/#dfn-execute-async-script
Web::WebDriver::Response WebDriverConnection::execute_async_script(JsonValue payload)
{
    // 1. Let body and arguments by the result of trying to extract the script arguments from a request with argument parameters.
    auto [body, arguments] = TRY(extract_the_script_arguments_from_a_request(payload));

    // 2. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 3. Handle any user prompts, and return its value if it is an error.
    handle_any_user_prompts([this, body = move(body), arguments = move(arguments)]() mutable {
        auto script_execution_id = m_script_execution_id_counter++;
        m_current_script_execution_id = script_execution_id;

        // 4. Let timeout be session's session timeouts' script timeout.
        auto timeout_ms = m_timeouts_configuration.script_timeout;

        // This handles steps 5 to 9 and produces the appropriate result type for the following steps.
        Web::WebDriver::execute_async_script(current_browsing_context(), move(body), move(arguments), timeout_ms, GC::create_function(GC::Heap::the(), [this, script_execution_id](Web::WebDriver::ExecutionResult result) {
            dbgln_if(WEBDRIVER_DEBUG, "Executing async script returned: {}", result.value);
            handle_script_response(result, script_execution_id);
        }));
    });

    return JsonValue {};
}

void WebDriverConnection::handle_script_response(Web::WebDriver::ExecutionResult result, size_t script_execution_id)
{
    if (script_execution_id != m_current_script_execution_id)
        return;
    m_current_script_execution_id.clear();

    auto response = [&]() -> Web::WebDriver::Response {
        switch (result.state) {
        // 10. If promise is still pending and timer's timeout fired flag is set, return error with error code script
        //     timeout.
        case JS::Promise::State::Pending:
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::ScriptTimeoutError, "Script timed out"sv);

        // 11. If promise is fulfilled with value v, let result be JSON clone with session and v, and return success
        //     with data result.
        case JS::Promise::State::Fulfilled:
            return Web::WebDriver::json_clone(current_browsing_context(), result.value);

        // 12. If promise is rejected with reason r, let result be JSON clone with session and r, and return error
        //     with error code javascript error and data result.
        case JS::Promise::State::Rejected: {
            auto reason = TRY(Web::WebDriver::json_clone(current_browsing_context(), result.value));
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::JavascriptError, "Script returned an error"sv, move(reason));
        }
        }

        VERIFY_NOT_REACHED();
    }();

    driver_execution_complete(move(response));
}

// 14.1 Get All Cookies, https://w3c.github.io/webdriver/#dfn-get-all-cookies
Web::WebDriver::Response WebDriverConnection::get_all_cookies()
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts, and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 3. Let cookies be a new JSON List.
        JsonArray cookies;

        // 4. For each cookie in all associated cookies of the current browsing context’s active document:
        auto* document = current_browsing_context().active_document();

        for (auto const& cookie : current_browsing_context().page().client().page_did_request_all_cookies_webdriver(document->url())) {
            // 1. Let serialized cookie be the result of serializing cookie.
            auto serialized_cookie = serialize_cookie(cookie);

            // 2. Append serialized cookie to cookies
            cookies.must_append(move(serialized_cookie));
        }

        // 5. Return success with data cookies.
        driver_execution_complete({ move(cookies) });
    });

    return JsonValue {};
}

// 14.2 Get Named Cookie, https://w3c.github.io/webdriver/#dfn-get-named-cookie
Web::WebDriver::Response WebDriverConnection::get_named_cookie(String name)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts, and return its value if it is an error.
    handle_any_user_prompts([this, name]() {
        // 3. If the url variable name is equal to a cookie’s cookie name amongst all associated cookies of the current browsing context’s active document, return success with the serialized cookie as data.
        auto* document = current_browsing_context().active_document();

        if (auto cookie = current_browsing_context().page().client().page_did_request_named_cookie(document->url(), name); cookie.has_value()) {
            auto serialized_cookie = serialize_cookie(*cookie);
            driver_execution_complete(move(serialized_cookie));
            return;
        }

        // 4. Otherwise, return error with error code no such cookie.
        driver_execution_complete(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchCookie, MUST(String::formatted("Cookie '{}' not found", name))));
    });

    return JsonValue {};
}

// 14.3 Add Cookie, https://w3c.github.io/webdriver/#dfn-adding-a-cookie
Web::WebDriver::Response WebDriverConnection::add_cookie(JsonValue payload)
{
    // 1. Let data be the result of getting a property named cookie from the parameters argument.
    auto const& data = *TRY(Web::WebDriver::get_property<JsonObject const*>(payload, "cookie"sv));

    // 2. If data is not a JSON Object with all the required (non-optional) JSON keys listed in the table for cookie conversion, return error with error code invalid argument.
    // NOTE: This validation is performed in subsequent steps.

    // 3. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 4. Handle any user prompts, and return its value if it is an error.
    handle_any_user_prompts([this, data = move(const_cast<JsonObject&>(data))]() {
        driver_execution_complete(add_cookie_impl(data));
    });

    return JsonValue {};
}

Web::WebDriver::Response WebDriverConnection::add_cookie_impl(JsonObject const& data)
{
    auto* document = current_browsing_context().active_document();

    // 5. If the current browsing context’s document element is a cookie-averse Document object, return error with
    //    error code invalid cookie domain.
    if (document->is_cookie_averse())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidCookieDomain, "Document is cookie-averse"sv);

    // 6. If cookie name or cookie value is null, cookie domain is not equal to the current browsing context’s active
    //    document’s domain, cookie secure only or cookie HTTP only are not boolean types, or cookie expiry time is not
    //    an integer type, or it less than 0 or greater than the maximum safe integer, return error with error code
    //    invalid argument.
    // NOTE: This validation is either performed in subsequent steps.

    // 7. Create a cookie in the cookie store associated with the active document’s address using cookie name name, cookie value value, and an attribute-value list of the following cookie concepts listed in the table for cookie conversion from data:
    HTTP::Cookie::ParsedCookie cookie {};
    cookie.name = TRY(Web::WebDriver::get_property(data, "name"sv));
    cookie.value = TRY(Web::WebDriver::get_property(data, "value"sv));

    // Cookie path
    //     The value if the entry exists, otherwise "/".
    if (data.has("path"sv))
        cookie.path = TRY(Web::WebDriver::get_property(data, "path"sv));
    else
        cookie.path = "/"_string;

    // Cookie domain
    //     The value if the entry exists, otherwise the current browsing context’s active document’s URL domain.
    // NOTE: The otherwise case is handled by the CookieJar
    if (data.has("domain"sv)) {
        auto domain = TRY(Web::WebDriver::get_property(data, "domain"sv));

        // NB: Clients conventionally send parent-domain cookies with a leading '.', which the Set-Cookie parser
        //     would strip. Strip it here as well, since the cookie store never sees a leading dot.
        if (domain.starts_with_bytes("."sv))
            domain = MUST(domain.substring_from_byte_offset(1));

        // FIXME: Spec issue: We must return InvalidCookieDomain for invalid domains, rather than InvalidArgument.
        // https://github.com/w3c/webdriver/issues/1570
        auto document_domain = document->domain().to_utf8();
        if (!HTTP::Cookie::domain_matches(document_domain, domain))
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidCookieDomain, "Cookie domain does not match document domain"sv);

        cookie.domain = move(domain);
    }

    // Cookie secure only
    //     The value if the entry exists, otherwise false.
    if (data.has("secure"sv))
        cookie.secure_attribute_present = TRY(Web::WebDriver::get_property<bool>(data, "secure"sv));

    // Cookie HTTP only
    //     The value if the entry exists, otherwise false.
    if (data.has("httpOnly"sv))
        cookie.http_only_attribute_present = TRY(Web::WebDriver::get_property<bool>(data, "httpOnly"sv));

    // Cookie expiry time
    //     The value if the entry exists, otherwise leave unset to indicate that this is a session cookie.
    if (data.has("expiry"sv)) {
        auto expiry = TRY(Web::WebDriver::get_property<i64>(data, "expiry"sv));
        cookie.expiry_time_from_expires_attribute = UnixDateTime::from_seconds_since_epoch(expiry);
    }

    // Cookie same site
    //     The value if the entry exists, otherwise leave unset to indicate that no same site policy is defined.
    if (data.has("sameSite"sv)) {
        auto same_site = TRY(Web::WebDriver::get_property(data, "sameSite"sv));
        cookie.same_site_attribute = HTTP::Cookie::same_site_from_string(same_site);

        if (cookie.same_site_attribute == HTTP::Cookie::SameSite::Default)
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidArgument, "Invalid same-site attribute"sv);
    }

    current_browsing_context().page().client().page_did_set_cookie(document->url(), cookie, HTTP::Cookie::Source::Http);

    // If there is an error during this step, return error with error code unable to set cookie.
    // NOTE: This probably should only apply to the actual setting of the cookie in the Browser, which cannot fail in our case.

    // 8. Return success with data null.
    return JsonValue {};
}

// 14.4 Delete Cookie, https://w3c.github.io/webdriver/#dfn-delete-cookie
Web::WebDriver::Response WebDriverConnection::delete_cookie(String name)
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts, and return its value if it is an error.
    handle_any_user_prompts([this, name]() {
        // 3. Delete cookies using the url variable name parameter as the filter argument.
        delete_cookies(name);

        // 4. Return success with data null.
        driver_execution_complete(JsonValue {});
    });

    return JsonValue {};
}

// 14.5 Delete All Cookies, https://w3c.github.io/webdriver/#dfn-delete-all-cookies
Web::WebDriver::Response WebDriverConnection::delete_all_cookies()
{
    // 1. If the current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Handle any user prompts, and return its value if it is an error.
    handle_any_user_prompts([this]() {
        // 3. Delete cookies, giving no filtering argument.
        delete_cookies();

        // 4. Return success with data null.
        driver_execution_complete(JsonValue {});
    });

    return JsonValue {};
}

// 15.7 Perform Actions, https://w3c.github.io/webdriver/#perform-actions
Web::WebDriver::Response WebDriverConnection::perform_actions(JsonValue payload)
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Try to handle any user prompts with session.
    handle_any_user_prompts([this, payload = move(payload)]() {
        // 3. Let input state be the result of get the input state with session and session's current top-level browsing context.
        auto& input_state = Web::WebDriver::get_input_state(*current_top_level_browsing_context());

        // 4. Let actions options be a new actions options with the is element origin steps set to represents a web element,
        //    and the get element origin steps set to get a WebElement origin.
        Web::WebDriver::ActionsOptions actions_options {
            .is_element_origin = &Web::WebDriver::represents_a_web_element,
            .get_element_origin = &Web::WebDriver::get_web_element_origin,
        };

        // 5. Let actions by tick be the result of trying to extract an action sequence with input state, parameters, and
        //    actions options.
        auto actions_by_tick = WEBDRIVER_TRY(Web::WebDriver::extract_an_action_sequence(input_state, payload, actions_options));

        // 6. Dispatch actions with input state, actions by tick, current browsing context, and actions options. If this
        //    results in an error return that error.
        auto on_complete = GC::create_function(GC::Heap::the(), [this](Web::WebDriver::Response result) {
            m_action_executor = nullptr;
            driver_execution_complete(move(result));
        });

        m_action_executor = Web::WebDriver::dispatch_actions(input_state, move(actions_by_tick), current_browsing_context(), move(actions_options), on_complete);
    });

    // 7. Return success with data null.
    return JsonValue {};
}

// 15.8 Release Actions, https://w3c.github.io/webdriver/#release-actions
Web::WebDriver::Response WebDriverConnection::release_actions()
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Try to handle any user prompts with session.
    handle_any_user_prompts([this]() {
        // 3. Let input state be the result of get the input state with session and current top-level browsing context.
        auto& input_state = Web::WebDriver::get_input_state(*current_top_level_browsing_context());

        // 4. Let actions options be a new actions options with the is element origin steps set to represents a web element,
        //    and the get element origin steps set to get a WebElement origin.
        Web::WebDriver::ActionsOptions actions_options {
            .is_element_origin = &Web::WebDriver::represents_a_web_element,
            .get_element_origin = &Web::WebDriver::get_web_element_origin,
        };

        // 5. Wait for an action queue token with input state.
        Web::WebDriver::wait_for_an_action_queue_token(input_state);

        // FIXME: Spec issue: The token we just enqueued must be dequeued, otherwise another token enqueued by dispatching
        //        the undo actions below will never be at the head of the queue.
        //        https://github.com/w3c/webdriver/issues/1878
        input_state.actions_queue.take_first();

        // 6. Let undo actions be input state's input cancel list in reverse order.
        auto undo_actions = input_state.input_cancel_list;
        undo_actions.reverse();

        // 7. Try to dispatch actions with input state, undo actions, current browsing context, and actions options.
        auto on_complete = GC::create_function(GC::Heap::the(), [this](Web::WebDriver::Response result) {
            m_action_executor = nullptr;

            // 8. Reset the input state with session and session's current top-level browsing context.
            Web::WebDriver::reset_input_state(*current_top_level_browsing_context());

            driver_execution_complete(move(result));
        });

        m_action_executor = Web::WebDriver::dispatch_actions(input_state, { move(undo_actions) }, current_browsing_context(), move(actions_options), on_complete);
    });

    // 9. Return success with data null.
    return JsonValue {};
}

// 16.1 Dismiss Alert, https://w3c.github.io/webdriver/#dismiss-alert
Web::WebDriver::Response WebDriverConnection::dismiss_alert()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 2. If there is no current user prompt, return error with error code no such alert.
    if (!current_browsing_context().page().has_pending_dialog())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchAlert, "No user dialog is currently open"sv);

    // 3. Dismiss the current user prompt.
    current_browsing_context().page().dismiss_dialog(GC::create_function(GC::Heap::the(), [this]() {
        driver_execution_complete(JsonValue {});
    }));

    // 4. Return success with data null.
    return JsonValue {};
}

// 16.2 Accept Alert, https://w3c.github.io/webdriver/#accept-alert
Web::WebDriver::Response WebDriverConnection::accept_alert()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 2. If there is no current user prompt, return error with error code no such alert.
    if (!current_browsing_context().page().has_pending_dialog())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchAlert, "No user dialog is currently open"sv);

    // 3. Accept the current user prompt.
    current_browsing_context().page().accept_dialog(GC::create_function(GC::Heap::the(), [this]() {
        driver_execution_complete(JsonValue {});
    }));

    // 4. Return success with data null.
    return JsonValue {};
}

// 16.3 Get Alert Text, https://w3c.github.io/webdriver/#get-alert-text
Web::WebDriver::Response WebDriverConnection::get_alert_text()
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 2. If there is no current user prompt, return error with error code no such alert.
    if (!current_browsing_context().page().has_pending_dialog())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchAlert, "No user dialog is currently open"sv);

    // 3. Let message be the text message associated with the current user prompt, or otherwise be null.
    auto const& message = current_browsing_context().page().pending_dialog_text();

    // 4. Return success with data message.
    if (message.has_value())
        return JsonValue { message->to_utf8() };
    return JsonValue {};
}

// 16.4 Send Alert Text, https://w3c.github.io/webdriver/#send-alert-text
Web::WebDriver::Response WebDriverConnection::send_alert_text(JsonValue payload)
{
    // 1. Let text be the result of getting the property "text" from parameters.
    // 2. If text is not a String, return error with error code invalid argument.
    auto text = TRY(Web::WebDriver::get_property(payload, "text"sv));

    // 3. If the current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // 4. If there is no current user prompt, return error with error code no such alert.
    if (!current_browsing_context().page().has_pending_dialog())
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchAlert, "No user dialog is currently open"sv);

    // 5. Run the substeps of the first matching current user prompt:
    switch (current_browsing_context().page().pending_dialog()) {
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
    current_browsing_context().page().client().page_did_request_set_prompt_text(Utf16String::from_utf8(text));

    // 7. Return success with data null.
    return JsonValue {};
}

// 17.1 Take Screenshot, https://w3c.github.io/webdriver/#take-screenshot
Web::WebDriver::Response WebDriverConnection::take_screenshot()
{
    // 1. If session's current top-level browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_top_level_browsing_context_is_open());

    // FIXME: Spec issue: We must handle user prompts in this endpoint, just like we do in Take Element Screenshot.
    // https://github.com/w3c/webdriver/issues/1678
    handle_any_user_prompts([this]() {
        auto* document = current_top_level_browsing_context()->active_document();
        auto window = document->window();

        // 2. When the user agent is next to run the animation frame callbacks:
        (void)window->animation_frame_callback_driver().add(GC::create_function(GC::Heap::the(), [this, document](double) mutable {
            // a. Let root rect be session's current top-level browsing context's document element's rectangle.
            auto root_rect = calculate_absolute_rect_of_element(*document->document_element());

            // b. Let screenshot result be the result of trying to call draw a bounding box from the framebuffer, given root rect as an argument.
            Web::WebDriver::draw_bounding_box_from_the_framebuffer(*current_top_level_browsing_context(), *document->document_element(), root_rect, [this](auto canvas_or_error) mutable {
                if (canvas_or_error.is_error()) {
                    driver_execution_complete(canvas_or_error.release_error());
                    return;
                }

                // c. Let canvas be a canvas element of screenshot result's data.
                auto canvas = canvas_or_error.release_value();

                // d. Let encoding result be the result of trying encoding a canvas as Base64 canvas.
                // e. Let encoded string be encoding result's data.
                auto encoded_string = Web::WebDriver::encode_canvas_element(*canvas);

                // 3. Return success with data encoded string.
                driver_execution_complete(move(encoded_string));
            });
        }));
        document->page().client().request_frame();
    });

    return JsonValue {};
}

// 17.2 Take Element Screenshot, https://w3c.github.io/webdriver/#dfn-take-element-screenshot
Web::WebDriver::Response WebDriverConnection::take_element_screenshot(String element_id)
{
    // 1. If session's current browsing context is no longer open, return error with error code no such window.
    TRY(ensure_current_browsing_context_is_open());

    // 2. Try to handle any user prompts with session.
    handle_any_user_prompts([this, element_id]() {
        auto* document = current_browsing_context().active_document();
        auto window = document->window();

        // 3. Let element be the result of trying to get a known element with session and URL variables["element id"].
        auto element = WEBDRIVER_TRY(Web::WebDriver::get_known_element(current_browsing_context(), element_id));

        // 4. Scroll into view the element.
        scroll_element_into_view(element);

        // 5. When the user agent is next to run the animation frame callbacks:
        (void)window->animation_frame_callback_driver().add(GC::create_function(GC::Heap::the(), [this, element](double) {
            // a. Let element rect be element's rectangle.
            auto element_rect = calculate_absolute_rect_of_element(element);

            // b. Let screenshot result be the result of trying to call draw a bounding box from the framebuffer, given element rect as an argument.
            Web::WebDriver::draw_bounding_box_from_the_framebuffer(current_browsing_context(), element, element_rect, [this](auto canvas_or_error) mutable {
                if (canvas_or_error.is_error()) {
                    driver_execution_complete(canvas_or_error.release_error());
                    return;
                }

                // c. Let canvas be a canvas element of screenshot result's data.
                auto canvas = canvas_or_error.release_value();

                // d. Let encoding result be the result of trying encoding a canvas as Base64 canvas.
                // e. Let encoded string be encoding result's data.
                auto encoded_string = Web::WebDriver::encode_canvas_element(*canvas);

                // 6. Return success with data encoded string.
                driver_execution_complete(move(encoded_string));
            });
        }));
        document->page().client().request_frame();
    });

    return JsonValue {};
}

// 18.1 Print Page, https://w3c.github.io/webdriver/#dfn-print-page
Web::WebDriver::Response WebDriverConnection::print_page(JsonValue payload)
{
    dbgln("FIXME: WebDriverConnection::print_page({})", payload);
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnsupportedOperation, "Print not implemented"sv);
}

// https://w3c.github.io/webdriver/#dfn-set-the-current-browsing-context
void WebDriverConnection::set_current_browsing_context(Web::HTML::BrowsingContext& browsing_context)
{
    // 1. Set session's current browsing context to context.
    m_current_browsing_context = browsing_context;

    // 2. Set the session's current parent browsing context to the parent browsing context of context, if that context
    //    exists, or null otherwise.
    if (auto navigable = browsing_context.active_document()->navigable(); navigable && navigable->parent())
        m_current_parent_browsing_context = as<Web::HTML::LocalNavigable>(*navigable->parent()).active_browsing_context();
    else
        m_current_parent_browsing_context = nullptr;
}

// https://w3c.github.io/webdriver/#dfn-set-the-current-browsing-context
void WebDriverConnection::set_current_top_level_browsing_context(Web::HTML::BrowsingContext& browsing_context)
{
    // 1. Assert: context is a top-level browsing context.
    VERIFY(browsing_context.is_top_level());

    if (m_current_top_level_browsing_context)
        m_current_top_level_browsing_context->page().set_window_rect_observer({});

    // 2. Set session's current top-level browsing context to context.
    m_current_top_level_browsing_context = browsing_context;

    if (m_current_top_level_browsing_context) {
        m_current_top_level_browsing_context->page().set_window_rect_observer(GC::create_function(GC::Heap::the(), [this](Web::DevicePixelRect rect, u64 request_id) {
            if (m_pending_window_rect_requests.remove(request_id) && m_pending_window_rect_requests.is_empty())
                driver_execution_complete(serialize_rect(rect.to_type<int>()));
        }));
    }

    // 3. Set the current browsing context with session and context.
    set_current_browsing_context(browsing_context);
}

Web::WebDriver::Response WebDriverConnection::ensure_top_level_browsing_context_is_open()
{
    TRY(ensure_current_top_level_browsing_context_is_open());
    return JsonValue {};
}

ErrorOr<void, Web::WebDriver::Error> WebDriverConnection::ensure_current_browsing_context_is_open()
{
    return Web::WebDriver::ensure_browsing_context_is_open(current_browsing_context());
}

ErrorOr<void, Web::WebDriver::Error> WebDriverConnection::ensure_current_top_level_browsing_context_is_open()
{
    return Web::WebDriver::ensure_browsing_context_is_open(current_top_level_browsing_context());
}

// https://w3c.github.io/webdriver/#dfn-handle-any-user-prompts
void WebDriverConnection::handle_any_user_prompts(Function<void()> on_dialog_closed)
{
    Web::WebDriver::handle_any_user_prompts(current_browsing_context().page(),
        GC::create_function(GC::Heap::the(), [this, on_dialog_closed = GC::create_function(GC::Heap::the(), move(on_dialog_closed))](Optional<Web::WebDriver::Error> error) {
            if (error.has_value()) {
                driver_execution_complete(error.release_value());
                return;
            }

            on_dialog_closed->function()();
        }));
}

// https://w3c.github.io/webdriver/#dfn-wait-for-navigation-to-complete
// FIXME: Update this AO to the latest spec steps.
void WebDriverConnection::wait_for_navigation_to_complete(OnNavigationComplete on_complete)
{
    // 1. If the current session has a page loading strategy of none, return success with data null.
    if (m_page_load_strategy == Web::WebDriver::PageLoadStrategy::None) {
        on_complete->function()(JsonValue {});
        return;
    }

    // 2. If the current browsing context is no longer open, return success with data null.
    if (Web::WebDriver::ensure_browsing_context_is_open(current_browsing_context()).is_error()) {
        on_complete->function()(JsonValue {});
        return;
    }

    auto navigable = current_browsing_context().active_document()->navigable();

    if (!navigable || navigable->ongoing_navigation().has<Empty>()) {
        on_complete->function()(JsonValue {});
        return;
    }

    auto reset_observers = [](auto& self) {
        if (self.m_navigation_observer) {
            self.m_navigation_observer->set_navigation_complete({});
            self.m_navigation_observer = nullptr;
        }
        if (self.m_document_observer) {
            self.m_document_observer->set_document_readiness_observer({});
            self.m_document_observer = nullptr;
        }
    };

    // 3. Start a timer. If this algorithm has not completed before timer reaches the session’s session page load timeout
    //    in milliseconds, return an error with error code timeout.
    m_navigation_timer = GC::Heap::the().allocate<GC::Timer>();

    // 4. If there is an ongoing attempt to navigate the current browsing context that has not yet matured, wait for
    //    navigation to mature.
    m_navigation_observer = Web::HTML::NavigationObserver::create(*navigable);

    m_navigation_observer->set_navigation_complete([this, reset_observers]() {
        reset_observers(*this);

        // 5. Let readiness target be the document readiness state associated with the current session’s page loading
        //    strategy, which can be found in the table of page load strategies.
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
        //    or for the session page load timeout to pass, whichever occurs sooner.
        if (auto* document = current_browsing_context().active_document(); document->readiness() != readiness_target) {
            m_document_observer = Web::DOM::DocumentObserver::create(*document);

            m_document_observer->set_document_readiness_observer([this, readiness_target](Web::HTML::DocumentReadyState readiness) {
                if (readiness == readiness_target)
                    m_navigation_timer->stop_and_fire_timeout_handler();
            });
        } else {
            m_navigation_timer->stop_and_fire_timeout_handler();
        }
    });

    m_navigation_timer->start(m_timeouts_configuration.page_load_timeout.value_or(300'000), GC::create_function(GC::Heap::the(), [this, on_complete, reset_observers]() {
        reset_observers(*this);

        auto did_time_out = m_navigation_timer->is_timed_out();
        m_navigation_timer = nullptr;

        // 7. If the previous step completed by the session page load timeout being reached and the browser does
        //    not have an active user prompt, return error with error code timeout.
        if (did_time_out && !current_browsing_context().active_document()->page().has_pending_dialog()) {
            on_complete->function()(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::Timeout, "Navigation timed out"sv));
            return;
        }

        // 8. Return success with data null.
        on_complete->function()(JsonValue {});
    }));
}

void WebDriverConnection::page_did_open_dialog(Badge<PageClient>)
{
    // OPTMIZATION: If a dialog is opened while we are awaiting a specific document readiness state, that state will
    //              never be reached, as the dialog will block the HTML event loop from any further processing. Instead
    //              of waiting for the session's page load timeout to expire, unblock the waiter immediately. This also
    //              seems to match how other browsers behave.
    if (m_navigation_timer)
        m_navigation_timer->stop_and_fire_timeout_handler();

    // https://w3c.github.io/webdriver/#dfn-execute-a-function-body
    // If at any point during the algorithm a user prompt appears, immediately return Completion { [[Type]]: normal,
    // [[Value]]: null, [[Target]]: empty }, but continue to run the other steps of this algorithm in parallel.
    if (m_current_script_execution_id.has_value()) {
        m_current_script_execution_id.clear();
        driver_execution_complete(JsonValue {});
    }
}

// https://w3c.github.io/webdriver/#dfn-maximize-the-window
void WebDriverConnection::maximize_the_window()
{
    // To maximize the window, given an operating system level window with an associated top-level browsing context, run
    // the implementation-specific steps to transition the operating system level window into the maximized window state.
    // Return when the window has completed the transition, or within an implementation-defined timeout.
    auto request_id = register_window_rect_request();
    current_top_level_browsing_context()->page().client().page_did_request_maximize_window(request_id);
}

u64 WebDriverConnection::register_window_rect_request()
{
    auto request_id = m_next_window_rect_request_id++;
    m_pending_window_rect_requests.set(request_id);
    return request_id;
}

// https://w3c.github.io/webdriver/#dfn-iconify-the-window
void WebDriverConnection::iconify_the_window(GC::Ref<GC::Function<void()>> on_complete)
{
    // To iconify the window, given an operating system level window with an associated top-level browsing context, run
    // implementation-specific steps to iconify, minimize, or hide the window from the visible screen.
    current_top_level_browsing_context()->page().client().page_did_request_minimize_window();

    // Do not return from this operation until the visibility state of the top-level browsing context’s active document
    // has reached the hidden state, or until the operation times out.
    wait_for_visibility_state(on_complete, Web::HTML::VisibilityState::Hidden);
}

// https://w3c.github.io/webdriver/#dfn-restore-the-window
void WebDriverConnection::restore_the_window(GC::Ref<GC::Function<void()>> on_complete)
{
    // To restore the window, given an operating system level window with an associated top-level browsing context, run
    // implementation-specific steps to restore or unhide the window to the visible screen.
    current_top_level_browsing_context()->page().client().page_did_request_restore_window();

    // Do not return from this operation until the visibility state of the top-level browsing context’s active document
    // has reached the visible state, or until the operation times out.
    wait_for_visibility_state(on_complete, Web::HTML::VisibilityState::Visible);
}

void WebDriverConnection::wait_for_visibility_state(GC::Ref<GC::Function<void()>> on_complete, Web::HTML::VisibilityState target_visibility_state)
{
    static constexpr auto VISIBILITY_STATE_TIMEOUT_MS = 5'000;

    auto* document = current_top_level_browsing_context()->active_document();

    if (document->visibility_state() == target_visibility_state) {
        on_complete->function()();
        return;
    }

    auto timer = GC::Heap::the().allocate<GC::Timer>();
    m_document_observer = Web::DOM::DocumentObserver::create(*document);

    m_document_observer->set_document_visibility_state_observer([timer, target_visibility_state](Web::HTML::VisibilityState visibility_state) {
        if (visibility_state == target_visibility_state)
            timer->stop_and_fire_timeout_handler();
    });

    timer->start(VISIBILITY_STATE_TIMEOUT_MS, GC::create_function(GC::Heap::the(), [this, on_complete]() {
        m_document_observer->set_document_visibility_state_observer({});
        m_document_observer = nullptr;

        on_complete->function()();
    }));
}

class ElementLocator final : public JS::Cell {
    GC_CELL(ElementLocator, JS::Cell);
    GC_DECLARE_ALLOCATOR(ElementLocator);

public:
    ElementLocator(
        Web::HTML::BrowsingContext const& browsing_context,
        Web::WebDriver::LocationStrategy location_strategy,
        String selector,
        WebDriverConnection::GetStartNode get_start_node,
        WebDriverConnection::OnFindComplete on_complete,
        GC::Ref<GC::Timer> timer)
        : m_browsing_context(browsing_context)
        , m_location_strategy(location_strategy)
        , m_selector(location_strategy == Web::WebDriver::LocationStrategy::TagName
                  ? Variant<String, Utf16FlyString> { Utf16FlyString::from_utf8(selector) }
                  : Variant<String, Utf16FlyString> { move(selector) })
        , m_get_start_node(get_start_node)
        , m_on_complete(on_complete)
        , m_timer(timer)
    {
    }

    void search_for_element()
    {
        if (auto result = perform_search(); result.has_value()) {
            m_on_complete->function()(result.release_value());
            return;
        }

        if (m_timer->is_timed_out())
            return;

        Web::HTML::queue_a_task(Web::HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(GC::Heap::the(), [this]() {
            search_for_element();
        }));
    }

private:
    Optional<Web::WebDriver::Response> perform_search()
    {
        // 1. Set elements returned to the result of trying to call the relevant element location strategy with arguments
        //    start node, and selector.
        auto maybe_elements = Web::WebDriver::invoke_location_strategy(m_location_strategy, TRY(m_get_start_node->function()()), m_selector);

        // 2. If a DOMException, SyntaxError, XPathException, or other error occurs during the execution of the element
        //    location strategy, return error invalid selector.
        if (maybe_elements.is_error())
            return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidSelector, MUST(String::formatted("The location strategy could not finish: {}", maybe_elements.error().message)));

        if (auto elements = maybe_elements.release_value(); elements->length() > 0) {
            // 8. Let result be an empty List.
            JsonArray result;
            result.ensure_capacity(elements->length());

            // 9. For each element in elements returned, append the web element reference object for session and element,
            //    to result.
            for (size_t i = 0; i < elements->length(); ++i)
                result.must_append(Web::WebDriver::web_element_reference_object(m_browsing_context, *elements->item(i)));

            // 10. Return success with data result.
            return JsonValue { move(result) };
        }

        return {};
    }

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_browsing_context);
        visitor.visit(m_get_start_node);
        visitor.visit(m_on_complete);
        visitor.visit(m_timer);
    }

    GC::Ref<Web::HTML::BrowsingContext const> m_browsing_context;

    Web::WebDriver::LocationStrategy m_location_strategy;
    Variant<String, Utf16FlyString> m_selector;

    WebDriverConnection::GetStartNode m_get_start_node;
    WebDriverConnection::OnFindComplete m_on_complete;

    GC::Ref<GC::Timer> m_timer;
};

GC_DEFINE_ALLOCATOR(ElementLocator);

// https://w3c.github.io/webdriver/#dfn-find
void WebDriverConnection::find(Web::WebDriver::LocationStrategy location_strategy, String selector, GetStartNode get_start_node, OnFindComplete on_complete)
{
    // 1. Let location strategy be equal to using.
    // 2. Let selector be equal to value.

    // 3. Let timeout be session's session timeouts' implicit wait timeout.
    auto timeout = m_timeouts_configuration.implicit_wait_timeout;

    // 4. Let timer be a new timer.
    auto timer = GC::Heap::the().allocate<GC::Timer>();

    auto wrapped_on_complete = GC::create_function(GC::Heap::the(), [this, on_complete, timer](Web::WebDriver::Response result) {
        m_element_locator = nullptr;
        timer->stop();

        on_complete->function()(move(result));
    });

    // 5. If timeout is not null:
    if (timeout.has_value()) {
        // 1. Start the timer with timer and timeout.
        timer->start(*timeout, GC::create_function(GC::Heap::the(), [wrapped_on_complete]() {
            wrapped_on_complete->function()({ JsonArray {} });
        }));
    }

    // 6. Let elements returned be an empty List.
    // 7. While elements returned is empty and timer's timeout fired flag is not set:
    m_element_locator = GC::Heap::the().allocate<ElementLocator>(current_browsing_context(), location_strategy, move(selector), get_start_node, wrapped_on_complete, timer);
    m_element_locator->search_for_element();
}

// https://w3c.github.io/webdriver/#dfn-extract-the-script-arguments-from-a-request
ErrorOr<WebDriverConnection::ScriptArguments, Web::WebDriver::Error> WebDriverConnection::extract_the_script_arguments_from_a_request(JsonValue const& payload)
{
    // Creating JSON objects below requires an execution context.
    Web::HTML::TemporaryExecutionContext execution_context { current_browsing_context().active_document()->relevant_settings_object() };

    // 1. Let script be the result of getting a property named script from the parameters.
    // 2. If script is not a String, return error with error code invalid argument.
    auto script = TRY(Web::WebDriver::get_property(payload, "script"sv));

    // 3. Let args be the result of getting a property named args from the parameters.
    // 4. If args is not an Array return error with error code invalid argument.
    auto const& args = *TRY(Web::WebDriver::get_property<JsonArray const*>(payload, "args"sv));

    // 5. Let arguments be the result of calling the JSON deserialize algorithm with arguments args.
    GC::RootVector<JS::Value> arguments;
    auto& browsing_context = current_browsing_context();

    TRY(args.try_for_each([&](JsonValue const& arg) -> ErrorOr<void, Web::WebDriver::Error> {
        auto deserialized = TRY(Web::WebDriver::json_deserialize(browsing_context, arg));
        arguments.append(deserialized);

        return {};
    }));

    // 6. Return success with data script and arguments.
    return ScriptArguments { move(script), move(arguments) };
}

// https://w3c.github.io/webdriver/#dfn-delete-cookies
void WebDriverConnection::delete_cookies(Optional<StringView> const& name)
{
    // For each cookie among all associated cookies of the current browsing context’s active document, un the substeps of the first matching condition:
    auto* document = current_browsing_context().active_document();

    for (auto& cookie : current_browsing_context().page().client().page_did_request_all_cookies_webdriver(document->url())) {
        // -> name is undefined
        // -> name is equal to cookie name
        if (!name.has_value() || name.value() == cookie.name) {
            // Set the cookie expiry time to a Unix timestamp in the past.
            cookie.expiry_time = UnixDateTime::earliest();
            current_browsing_context().page().client().page_did_update_cookie(cookie);
        }
        // -> Otherwise
        //    Do nothing.
    }
}

// https://w3c.github.io/webdriver/#dfn-calculate-the-absolute-position
Gfx::IntPoint WebDriverConnection::calculate_absolute_position_of_element(Web::CSSPixelRect rect)
{
    // 1. Let rect be the value returned by calling getBoundingClientRect().

    // 2. Let window be the associated window of current top-level browsing context.
    auto const* window = current_top_level_browsing_context()->active_window();

    // 3. Let x be (scrollX of window + rect’s x coordinate).
    auto x = (window ? static_cast<int>(window->scroll_x()) : 0) + static_cast<int>(rect.x());

    // 4. Let y be (scrollY of window + rect’s y coordinate).
    auto y = (window ? static_cast<int>(window->scroll_y()) : 0) + static_cast<int>(rect.y());

    // 5. Return a pair of (x, y).
    return { x, y };
}

Gfx::IntRect WebDriverConnection::calculate_absolute_rect_of_element(Web::DOM::Element const& element)
{
    auto bounding_rect = element.get_bounding_client_rect();
    auto coordinates = calculate_absolute_position_of_element(bounding_rect);

    return {
        coordinates.x(),
        coordinates.y(),
        static_cast<int>(bounding_rect.width()),
        static_cast<int>(bounding_rect.height())
    };
}

}
