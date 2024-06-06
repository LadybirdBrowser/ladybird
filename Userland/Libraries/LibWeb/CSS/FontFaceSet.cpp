/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Set.h>
#include <LibWeb/Bindings/FontFaceSetPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(FontFaceSet);

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-fontfaceset
JS::NonnullGCPtr<FontFaceSet> FontFaceSet::construct_impl(JS::Realm& realm, Vector<JS::Handle<FontFace>> const& initial_faces)
{
    auto ready_promise = WebIDL::create_promise(realm);
    auto set_entries = JS::Set::create(realm);

    // The FontFaceSet constructor, when called, must iterate its initialFaces argument and add each value to its set entries.
    for (auto const& face : initial_faces)
        set_entries->set_add(face);

    if (set_entries->set_size() == 0)
        WebIDL::resolve_promise(realm, *ready_promise);

    return realm.heap().allocate<FontFaceSet>(realm, realm, ready_promise, set_entries);
}

JS::NonnullGCPtr<FontFaceSet> FontFaceSet::create(JS::Realm& realm)
{
    return construct_impl(realm, {});
}

FontFaceSet::FontFaceSet(JS::Realm& realm, JS::NonnullGCPtr<WebIDL::Promise> ready_promise, JS::NonnullGCPtr<JS::Set> set_entries)
    : DOM::EventTarget(realm)
    , m_set_entries(set_entries)
    , m_ready_promise(ready_promise)
{
    bool const is_ready = ready()->state() == JS::Promise::State::Fulfilled;
    m_status = is_ready ? Bindings::FontFaceSetLoadStatus::Loaded : Bindings::FontFaceSetLoadStatus::Loading;
}

void FontFaceSet::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    WEB_SET_PROTOTYPE_FOR_INTERFACE(FontFaceSet);
}

void FontFaceSet::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_set_entries);
    visitor.visit(m_ready_promise);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-add
JS::NonnullGCPtr<FontFaceSet> FontFaceSet::add(JS::Handle<FontFace> face)
{
    // FIXME: Do the actual spec steps
    m_set_entries->set_add(face);
    return *this;
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-delete
bool FontFaceSet::delete_(JS::Handle<FontFace> face)
{
    // FIXME: Do the actual spec steps
    return m_set_entries->set_remove(face);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-clear
void FontFaceSet::clear()
{
    // FIXME: Do the actual spec steps
    m_set_entries->set_clear();
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloading
void FontFaceSet::set_onloading(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::loading, event_handler);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloading
WebIDL::CallbackType* FontFaceSet::onloading()
{
    return event_handler_attribute(HTML::EventNames::loading);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloadingdone
void FontFaceSet::set_onloadingdone(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::loadingdone, event_handler);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloadingdone
WebIDL::CallbackType* FontFaceSet::onloadingdone()
{
    return event_handler_attribute(HTML::EventNames::loadingdone);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloadingerror
void FontFaceSet::set_onloadingerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::loadingerror, event_handler);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloadingerror
WebIDL::CallbackType* FontFaceSet::onloadingerror()
{
    return event_handler_attribute(HTML::EventNames::loadingerror);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-load
JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Promise>> FontFaceSet::load(String const&, String const&)
{
    // FIXME: Do the steps
    auto promise = WebIDL::create_rejected_promise(realm(), WebIDL::NotSupportedError::create(realm(), "FontFaceSet::load is not yet implemented"_fly_string));
    return verify_cast<JS::Promise>(*promise->promise());
}

// https://drafts.csswg.org/css-font-loading/#font-face-set-ready
JS::NonnullGCPtr<JS::Promise> FontFaceSet::ready() const
{
    return verify_cast<JS::Promise>(*m_ready_promise->promise());
}

}
