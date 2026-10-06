/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/FontFaceState.h>

namespace Web::CSS {

class FontFace final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(FontFace, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(FontFace);

public:
    using FontFaceSource = FontFaceState::FontFaceSource;

    [[nodiscard]] static GC::Ref<FontFace> create_for_constructor(JS::Object&, Utf16String family, FontFaceSource source, Bindings::FontFaceDescriptors const&);
    [[nodiscard]] static GC::Ref<FontFace> create(FontFaceState&);

    FontFaceState& state() const { return m_state; }

    Utf16String family() const { return m_state->descriptor_text(DescriptorID::FontFamily); }
    WebIDL::ExceptionOr<void> set_family(Utf16View value) { return m_state->set_descriptor(DescriptorID::FontFamily, value); }
    Utf16String style() const { return m_state->descriptor_text(DescriptorID::FontStyle); }
    WebIDL::ExceptionOr<void> set_style(Utf16View value) { return m_state->set_descriptor(DescriptorID::FontStyle, value); }
    Utf16String weight() const { return m_state->descriptor_text(DescriptorID::FontWeight); }
    WebIDL::ExceptionOr<void> set_weight(Utf16View value) { return m_state->set_descriptor(DescriptorID::FontWeight, value); }
    Utf16String stretch() const { return m_state->descriptor_text(DescriptorID::FontWidth); }
    WebIDL::ExceptionOr<void> set_stretch(Utf16View value) { return m_state->set_descriptor(DescriptorID::FontWidth, value); }
    Utf16String unicode_range() const { return m_state->descriptor_text(DescriptorID::UnicodeRange); }
    WebIDL::ExceptionOr<void> set_unicode_range(Utf16View value) { return m_state->set_descriptor(DescriptorID::UnicodeRange, value); }
    Utf16String feature_settings() const { return m_state->descriptor_text(DescriptorID::FontFeatureSettings); }
    WebIDL::ExceptionOr<void> set_feature_settings(Utf16View value) { return m_state->set_descriptor(DescriptorID::FontFeatureSettings, value); }
    Utf16String variation_settings() const { return m_state->descriptor_text(DescriptorID::FontVariationSettings); }
    WebIDL::ExceptionOr<void> set_variation_settings(Utf16View value) { return m_state->set_descriptor(DescriptorID::FontVariationSettings, value); }
    Utf16String display() const { return m_state->descriptor_text(DescriptorID::FontDisplay); }
    WebIDL::ExceptionOr<void> set_display(Utf16View value) { return m_state->set_descriptor(DescriptorID::FontDisplay, value); }
    Utf16String ascent_override() const { return m_state->descriptor_text(DescriptorID::AscentOverride); }
    WebIDL::ExceptionOr<void> set_ascent_override(Utf16View value) { return m_state->set_descriptor(DescriptorID::AscentOverride, value); }
    Utf16String descent_override() const { return m_state->descriptor_text(DescriptorID::DescentOverride); }
    WebIDL::ExceptionOr<void> set_descent_override(Utf16View value) { return m_state->set_descriptor(DescriptorID::DescentOverride, value); }
    Utf16String line_gap_override() const { return m_state->descriptor_text(DescriptorID::LineGapOverride); }
    WebIDL::ExceptionOr<void> set_line_gap_override(Utf16View value) { return m_state->set_descriptor(DescriptorID::LineGapOverride, value); }

    FontFaceLoadStatus status() const { return m_state->status(); }
    GC::Ref<WebIDL::Promise> load() { return m_state->load(); }
    GC::Ref<WebIDL::Promise> loaded() const { return m_state->loaded(); }
    void set_font_display_time_for_testing(u32 milliseconds) { m_state->set_font_display_time_for_testing(milliseconds); }

private:
    explicit FontFace(FontFaceState&);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    NonnullRefPtr<FontFaceState> m_state;
};

}
