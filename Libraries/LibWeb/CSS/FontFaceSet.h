/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/FontFaceSet.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

enum class FontFaceSetLoadStatus : u8;

}

namespace Web::CSS {

using FontFaceSetLoadStatus = Bindings::FontFaceSetLoadStatus;

class FontFaceSet final : public DOM::EventTarget {
    WEB_WRAPPABLE(FontFaceSet, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(FontFaceSet);

public:
    [[nodiscard]] static GC::Ref<FontFaceSet> create(HTML::EnvironmentSettingsObject&);
    virtual ~FontFaceSet() override = default;

    size_t set_size() const { return m_font_faces.size(); }
    Vector<NonnullRefPtr<FontFaceState>> const& font_faces() const { return m_font_faces; }

    WebIDL::ExceptionOr<GC::Ref<FontFaceSet>> add(GC::Ref<FontFace>);
    bool delete_(GC::Ref<FontFace>);
    void clear();

    void add_css_connected_font(NonnullRefPtr<FontFaceState>);
    void remove_css_connected_font(NonnullRefPtr<FontFaceState>);
    void synchronize_css_connected_font_order();

    void set_onloading(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloading();
    void set_onloadingdone(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloadingdone();
    void set_onloadingerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloadingerror();

    JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> load(Utf16String font, Utf16String text);
    WebIDL::ExceptionOr<bool> check(Utf16String const& font, Utf16String const& text);

    Vector<NonnullRefPtr<FontFaceState>>& loading_fonts() { return m_loading_fonts; }
    Vector<NonnullRefPtr<FontFaceState>>& loaded_fonts() { return m_loaded_fonts; }
    Vector<NonnullRefPtr<FontFaceState>>& failed_fonts() { return m_failed_fonts; }

    GC::Ref<WebIDL::Promise> ready();
    HTML::EnvironmentSettingsObject& relevant_settings_object() const { return *m_environment; }
    FontFaceSetLoadStatus status() const { return m_status; }

    void fire_a_font_load_event(Utf16FlyString name, Vector<NonnullRefPtr<FontFaceState>> = {});
    void set_is_pending_on_the_environment(bool);

    void switch_to_loading();
    void switch_to_loaded();

private:
    enum class AllowCSSConnected {
        No,
        Yes,
    };

    explicit FontFaceSet(HTML::EnvironmentSettingsObject&);
    virtual void visit_edges(Cell::Visitor&) override;
    bool remove_font_face(NonnullRefPtr<FontFaceState>, AllowCSSConnected);

    Vector<NonnullRefPtr<FontFaceState>> m_font_faces;
    GC::Ref<HTML::EnvironmentSettingsObject> m_environment;
    GC::Ref<WebIDL::Promise> m_ready_promise; // [[ReadyPromise]]

    Vector<NonnullRefPtr<FontFaceState>> m_loading_fonts {}; // [[LoadingFonts]]
    Vector<NonnullRefPtr<FontFaceState>> m_loaded_fonts {};  // [[LoadedFonts]]
    Vector<NonnullRefPtr<FontFaceState>> m_failed_fonts {};  // [[FailedFonts]]

    FontFaceSetLoadStatus m_status;

    bool m_is_pending_on_the_environment { true };

    // https://drafts.csswg.org/css-font-loading/#fontfaceset-stuck-on-the-environment
    bool m_is_stuck_on_the_environment { false };
};

}
