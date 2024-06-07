/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/InternalsPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/Internals.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Internals {

JS_DEFINE_ALLOCATOR(Internals);

Internals::Internals(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

Internals::~Internals() = default;

void Internals::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Internals);
}

void Internals::signal_text_test_is_done()
{
    global_object().browsing_context()->page().client().page_did_finish_text_test();
}

void Internals::gc()
{
    vm().heap().collect_garbage();
}

JS::Object* Internals::hit_test(double x, double y)
{
    auto* active_document = global_object().browsing_context()->top_level_browsing_context()->active_document();
    // NOTE: Force a layout update just before hit testing. This is because the current layout tree, which is required
    //       for stacking context traversal, might not exist if this call occurs between the tear_down_layout_tree()
    //       and update_layout() calls
    active_document->update_layout();
    auto result = active_document->paintable_box()->hit_test({ x, y }, Painting::HitTestType::Exact);
    if (result.has_value()) {
        auto hit_tеsting_result = JS::Object::create(realm(), nullptr);
        hit_tеsting_result->define_direct_property("node", result->dom_node(), JS::default_attributes);
        hit_tеsting_result->define_direct_property("indexInNode", JS::Value(result->index_in_node), JS::default_attributes);
        return hit_tеsting_result;
    }
    return nullptr;
}

void Internals::send_text(HTML::HTMLElement& target, String const& text)
{
    auto& page = global_object().browsing_context()->page();
    target.focus();

    for (auto code_point : text.code_points())
        page.handle_keydown(UIEvents::code_point_to_key_code(code_point), 0, code_point);
}

void Internals::commit_text()
{
    global_object().browsing_context()->page().handle_keydown(UIEvents::Key_Return, 0, 0);
}

void Internals::click(double x, double y)
{
    auto& page = global_object().browsing_context()->page();
    page.handle_mousedown({ x, y }, { x, y }, 1, 0, 0);
    page.handle_mouseup({ x, y }, { x, y }, 1, 0, 0);
}

void Internals::move_pointer_to(double x, double y)
{
    auto& page = global_object().browsing_context()->page();
    page.handle_mousemove({ x, y }, { x, y }, 0, 0);
}

void Internals::wheel(double x, double y, double delta_x, double delta_y)
{
    auto& page = global_object().browsing_context()->page();
    page.handle_mousewheel({ x, y }, { x, y }, 0, 0, 0, delta_x, delta_y);
}

WebIDL::ExceptionOr<bool> Internals::dispatch_user_activated_event(DOM::EventTarget& target, DOM::Event& event)
{
    event.set_is_trusted(true);
    return target.dispatch_event(event);
}

JS::NonnullGCPtr<InternalAnimationTimeline> Internals::create_internal_animation_timeline()
{
    auto& realm = this->realm();
    return realm.heap().allocate<InternalAnimationTimeline>(realm, realm);
}

}
