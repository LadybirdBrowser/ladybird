/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/SetIterator.h>
#include <LibWeb/Bindings/FontFaceSetPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::CSS {

class FontFaceSet final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(FontFaceSet, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(FontFaceSet);

public:
    [[nodiscard]] static GC::Ref<FontFaceSet> construct_impl(JS::Realm&, Vector<GC::Root<FontFace>> const& initial_faces);
    [[nodiscard]] static GC::Ref<FontFaceSet> create(JS::Realm&);
    virtual ~FontFaceSet() override = default;

    GC::Ref<JS::Set> set_entries() const { return m_set_entries; }

    WebIDL::ExceptionOr<GC::Ref<FontFaceSet>> add(GC::Root<FontFace> const&);
    bool delete_(GC::Root<FontFace>);
    void clear();

    void set_onloading(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloading();
    void set_onloadingdone(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloadingdone();
    void set_onloadingerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloadingerror();

    JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> load(String const& font, String const& text);

    GC::Ref<WebIDL::Promise> ready() const;
    Bindings::FontFaceSetLoadStatus status() const { return m_status; }

    void resolve_ready_promise();

    void on_set_modified_from_js(Badge<Bindings::FontFaceSetPrototype>) { }

private:
    FontFaceSet(JS::Realm&, GC::Ref<WebIDL::Promise> ready_promise, GC::Ref<JS::Set> set_entries);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<JS::Set> m_set_entries;
    GC::Ref<WebIDL::Promise> m_ready_promise; // [[ReadyPromise]]

    Vector<GC::Ref<FontFace>> m_loading_fonts {}; // [[LoadingFonts]]
    Vector<GC::Ref<FontFace>> m_loaded_fonts {};  // [[LoadedFonts]]
    Vector<GC::Ref<FontFace>> m_failed_fonts {};  // [[FailedFonts]]

    Bindings::FontFaceSetLoadStatus m_status { Bindings::FontFaceSetLoadStatus::Loading };
};

}
