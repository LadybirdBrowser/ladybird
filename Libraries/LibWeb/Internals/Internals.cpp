/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/InternalsPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/Internals.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Internals {

static u16 s_echo_server_port { 0 };

GC_DEFINE_ALLOCATOR(Internals);

Internals::Internals(JS::Realm& realm)
    : InternalsBase(realm)
{
}

Internals::~Internals() = default;

void Internals::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Internals);
}

void Internals::signal_test_is_done(String const& text)
{
    page().client().page_did_finish_test(text);
}

void Internals::set_test_timeout(double milliseconds)
{
    page().client().page_did_set_test_timeout(milliseconds);
}

void Internals::gc()
{
    vm().heap().collect_garbage();
}

JS::Object* Internals::hit_test(double x, double y)
{
    auto& active_document = window().associated_document();
    // NOTE: Force a layout update just before hit testing. This is because the current layout tree, which is required
    //       for stacking context traversal, might not exist if this call occurs between the tear_down_layout_tree()
    //       and update_layout() calls
    active_document.update_layout(DOM::UpdateLayoutReason::InternalsHitTest);
    auto result = active_document.paintable_box()->hit_test({ x, y }, Painting::HitTestType::Exact);
    if (result.has_value()) {
        auto hit_tеsting_result = JS::Object::create(realm(), nullptr);
        hit_tеsting_result->define_direct_property("node"_fly_string, result->dom_node(), JS::default_attributes);
        hit_tеsting_result->define_direct_property("indexInNode"_fly_string, JS::Value(result->index_in_node), JS::default_attributes);
        return hit_tеsting_result;
    }
    return nullptr;
}

void Internals::send_text(HTML::HTMLElement& target, String const& text, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    target.focus();

    for (auto code_point : text.code_points())
        page.handle_keydown(UIEvents::code_point_to_key_code(code_point), modifiers, code_point, false);
}

void Internals::send_key(HTML::HTMLElement& target, String const& key_name, WebIDL::UnsignedShort modifiers)
{
    auto key_code = UIEvents::key_code_from_string(key_name);
    target.focus();

    page().handle_keydown(key_code, modifiers, 0, false);
}

void Internals::commit_text()
{
    page().handle_keydown(UIEvents::Key_Return, 0, 0x0d, false);
}

void Internals::click(double x, double y)
{
    click(x, y, UIEvents::MouseButton::Primary);
}

void Internals::doubleclick(double x, double y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_doubleclick(position, position, UIEvents::MouseButton::Primary, 0, 0);
}

void Internals::middle_click(double x, double y)
{
    click(x, y, UIEvents::MouseButton::Middle);
}

void Internals::click(double x, double y, UIEvents::MouseButton button)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_mousedown(position, position, button, 0, 0);
    page.handle_mouseup(position, position, button, 0, 0);
}

void Internals::mouse_down(double x, double y)
{
    mouse_down(x, y, UIEvents::MouseButton::Primary);
}

void Internals::mouse_down(double x, double y, UIEvents::MouseButton button)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_mousedown(position, position, button, 0, 0);
}

void Internals::move_pointer_to(double x, double y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_mousemove(position, position, 0, 0);
}

void Internals::wheel(double x, double y, double delta_x, double delta_y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_mousewheel(position, position, 0, 0, 0, delta_x, delta_y);
}

WebIDL::ExceptionOr<bool> Internals::dispatch_user_activated_event(DOM::EventTarget& target, DOM::Event& event)
{
    event.set_is_trusted(true);
    return target.dispatch_event(event);
}

void Internals::spoof_current_url(String const& url_string)
{
    auto url = DOMURL::parse(url_string);

    VERIFY(url.has_value());

    auto origin = url->origin();

    auto& window = this->window();
    window.associated_document().set_url(url.value());
    window.associated_document().set_origin(origin);
    HTML::relevant_settings_object(window.associated_document()).creation_url = url.release_value();
}

GC::Ref<InternalAnimationTimeline> Internals::create_internal_animation_timeline()
{
    auto& realm = this->realm();
    return realm.create<InternalAnimationTimeline>(realm);
}

void Internals::simulate_drag_start(double x, double y, String const& name, String const& contents)
{
    Vector<HTML::SelectedFile> files;
    files.empend(name.to_byte_string(), MUST(ByteBuffer::copy(contents.bytes())));

    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_drag_and_drop_event(DragEvent::Type::DragStart, position, position, UIEvents::MouseButton::Primary, 0, 0, move(files));
}

void Internals::simulate_drag_move(double x, double y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_drag_and_drop_event(DragEvent::Type::DragMove, position, position, UIEvents::MouseButton::Primary, 0, 0, {});
}

void Internals::simulate_drop(double x, double y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_drag_and_drop_event(DragEvent::Type::Drop, position, position, UIEvents::MouseButton::Primary, 0, 0, {});
}

void Internals::enable_cookies_on_file_domains()
{
    window().associated_document().enable_cookies_on_file_domains({});
}

void Internals::expire_cookies_with_time_offset(WebIDL::LongLong seconds)
{
    page().client().page_did_expire_cookies_with_time_offset(AK::Duration::from_seconds(seconds));
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static
String Internals::get_computed_role(DOM::Element& element)
{
    if (auto role = element.role_or_default(); role.has_value())
        return MUST(String::from_utf8(ARIA::role_name(role.value())));
    return String {};
}

String Internals::get_computed_label(DOM::Element& element)
{
    auto& active_document = window().associated_document();
    return MUST(element.accessible_name(active_document));
}

u16 Internals::get_echo_server_port()
{
    return s_echo_server_port;
}

void Internals::set_echo_server_port(u16 const port)
{
    s_echo_server_port = port;
}

void Internals::set_browser_zoom(double factor)
{
    page().client().page_did_set_browser_zoom(factor);
}

bool Internals::headless()
{
    return page().client().is_headless();
}

}
