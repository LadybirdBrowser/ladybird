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
    [[nodiscard]] static GC::Ref<FontFaceSet> create(JS::Realm&);
    virtual ~FontFaceSet() override = default;

    GC::Ref<JS::Set> set_entries() const { return m_set_entries; }

    WebIDL::ExceptionOr<GC::Ref<FontFaceSet>> add(GC::Root<FontFace>);
    bool delete_(GC::Root<FontFace>);
    void clear();

    void add_css_connected_font(GC::Ref<FontFace>);

    void set_onloading(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloading();
    void set_onloadingdone(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloadingdone();
    void set_onloadingerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloadingerror();

    JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> load(String const& font, String const& text);

    Vector<GC::Ref<FontFace>>& loading_fonts() { return m_loading_fonts; }
    Vector<GC::Ref<FontFace>>& loaded_fonts() { return m_loaded_fonts; }
    Vector<GC::Ref<FontFace>>& failed_fonts() { return m_failed_fonts; }

    GC::Ref<WebIDL::Promise> ready() const;
    Bindings::FontFaceSetLoadStatus status() const { return m_status; }

    void on_set_modified_from_js(Badge<Bindings::FontFaceSetPrototype>) { }

    void fire_a_font_load_event(FlyString name, Vector<GC::Ref<FontFace>> = {});
    void set_is_pending_on_the_environment(bool);

    void switch_to_loading();
    void switch_to_loaded();

private:
    explicit FontFaceSet(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<JS::Set> m_set_entries;
    GC::Ref<WebIDL::Promise> m_ready_promise; // [[ReadyPromise]]

    Vector<GC::Ref<FontFace>> m_loading_fonts {}; // [[LoadingFonts]]
    Vector<GC::Ref<FontFace>> m_loaded_fonts {};  // [[LoadedFonts]]
    Vector<GC::Ref<FontFace>> m_failed_fonts {};  // [[FailedFonts]]

    Bindings::FontFaceSetLoadStatus m_status { Bindings::FontFaceSetLoadStatus::Loaded };

    bool m_is_pending_on_the_environment { true };

    // https://drafts.csswg.org/css-font-loading/#fontfaceset-stuck-on-the-environment
    bool m_is_stuck_on_the_environment { false };
};

}
