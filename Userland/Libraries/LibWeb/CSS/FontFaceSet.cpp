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
    , m_status(Bindings::FontFaceSetLoadStatus::Loaded)
{
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
    visitor.visit(m_loading_fonts);
    visitor.visit(m_loaded_fonts);
    visitor.visit(m_failed_fonts);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-add
WebIDL::ExceptionOr<JS::NonnullGCPtr<FontFaceSet>>
FontFaceSet::add(JS::Handle<FontFace> face)
{
    // 1. If font is already in the FontFaceSet’s set entries, skip to the last step of this algorithm immediately.
    if (m_set_entries->set_has(face))
        return JS::NonnullGCPtr<FontFaceSet>(*this);

    // 2. If font is CSS-connected, throw an InvalidModificationError exception and exit this algorithm immediately.
    if (face->is_css_connected()) {
        return WebIDL::InvalidModificationError::create(realm(), "Cannot add a CSS-connected FontFace to a FontFaceSet"_string);
    }

    // 3. Add the font argument to the FontFaceSet’s set entries.
    m_set_entries->set_add(face);

    // 4. If font’s status attribute is "loading"
    if (face->status() == Bindings::FontFaceLoadStatus::Loading) {

        // 1. If the FontFaceSet’s [[LoadingFonts]] list is empty, switch the FontFaceSet to loading.
        if (m_loading_fonts.is_empty()) {
            m_status = Bindings::FontFaceSetLoadStatus::Loading;
        }

        // 2. Append font to the FontFaceSet’s [[LoadingFonts]] list.
        m_loading_fonts.append(*face);
    }

    // 5. Return the FontFaceSet.
    return JS::NonnullGCPtr<FontFaceSet>(*this);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-delete
bool FontFaceSet::delete_(JS::Handle<FontFace> face)
{
    // 1. If font is CSS-connected, return false and exit this algorithm immediately.
    if (face->is_css_connected()) {
        return false;
    }

    // 2. Let deleted be the result of removing font from the FontFaceSet’s set entries.
    bool deleted = m_set_entries->set_remove(face);

    // 3. If font is present in the FontFaceSet’s [[LoadedFonts]], or [[FailedFonts]] lists, remove it.
    m_loaded_fonts.remove_all_matching([face](auto const& entry) { return entry == face; });
    m_failed_fonts.remove_all_matching([face](auto const& entry) { return entry == face; });

    // 4. If font is present in the FontFaceSet’s [[LoadingFonts]] list, remove it. If font was the last item in that list (and so the list is now empty), switch the FontFaceSet to loaded.
    m_loading_fonts.remove_all_matching([face](auto const& entry) { return entry == face; });

    if (m_loading_fonts.is_empty()) {
        m_status = Bindings::FontFaceSetLoadStatus::Loaded;
    }

    return deleted;
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
    auto promise = WebIDL::create_rejected_promise(realm(), WebIDL::NotSupportedError::create(realm(), "FontFaceSet::load is not yet implemented"_string));
    return verify_cast<JS::Promise>(*promise->promise());
}

// https://drafts.csswg.org/css-font-loading/#font-face-set-ready
JS::NonnullGCPtr<JS::Promise> FontFaceSet::ready() const
{
    return verify_cast<JS::Promise>(*m_ready_promise->promise());
}

void FontFaceSet::resolve_ready_promise()
{
    WebIDL::resolve_promise(realm(), *m_ready_promise);
}

}
