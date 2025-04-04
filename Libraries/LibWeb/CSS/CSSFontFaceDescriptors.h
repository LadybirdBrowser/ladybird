/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/Descriptor.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-fonts-4/#cssfontfacedescriptors
class CSSFontFaceDescriptors final
    : public CSSStyleDeclaration {
    WEB_PLATFORM_OBJECT(CSSFontFaceDescriptors, CSSStyleDeclaration);
    GC_DECLARE_ALLOCATOR(CSSFontFaceDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSFontFaceDescriptors> create(JS::Realm&, Vector<Descriptor>);

    virtual ~CSSFontFaceDescriptors() override;

    virtual void initialize(JS::Realm&) override;

    virtual size_t length() const override;
    virtual String item(size_t index) const override;

    virtual WebIDL::ExceptionOr<void> set_property(StringView property, StringView value, StringView priority) override;
    virtual WebIDL::ExceptionOr<String> remove_property(StringView property) override;
    virtual String get_property_value(StringView property) const override;
    virtual StringView get_property_priority(StringView property) const override;

    RefPtr<CSSStyleValue const> descriptor(DescriptorID) const;
    RefPtr<CSSStyleValue const> descriptor_or_initial_value(DescriptorID) const;

    WebIDL::ExceptionOr<void> set_ascent_override(StringView value);
    String ascent_override() const;

    WebIDL::ExceptionOr<void> set_descent_override(StringView value);
    String descent_override() const;

    WebIDL::ExceptionOr<void> set_font_display(StringView value);
    String font_display() const;

    WebIDL::ExceptionOr<void> set_font_family(StringView value);
    String font_family() const;

    WebIDL::ExceptionOr<void> set_font_feature_settings(StringView value);
    String font_feature_settings() const;

    WebIDL::ExceptionOr<void> set_font_language_override(StringView value);
    String font_language_override() const;

    WebIDL::ExceptionOr<void> set_font_named_instance(StringView value);
    String font_named_instance() const;

    WebIDL::ExceptionOr<void> set_font_style(StringView value);
    String font_style() const;

    WebIDL::ExceptionOr<void> set_font_variation_settings(StringView value);
    String font_variation_settings() const;

    WebIDL::ExceptionOr<void> set_font_weight(StringView value);
    String font_weight() const;

    WebIDL::ExceptionOr<void> set_font_width(StringView value);
    String font_width() const;

    WebIDL::ExceptionOr<void> set_line_gap_override(StringView value);
    String line_gap_override() const;

    WebIDL::ExceptionOr<void> set_src(StringView value);
    String src() const;

    WebIDL::ExceptionOr<void> set_unicode_range(StringView value);
    String unicode_range() const;

    virtual String serialized() const override;

    virtual WebIDL::ExceptionOr<void> set_css_text(StringView) override;

private:
    CSSFontFaceDescriptors(JS::Realm&, Vector<Descriptor>);

    bool set_a_css_declaration(DescriptorID, NonnullRefPtr<CSSStyleValue const>, Important);

    virtual void visit_edges(Visitor&) override;

    Vector<Descriptor> m_descriptors;
};

}
