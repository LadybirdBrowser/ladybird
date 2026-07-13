/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWeb/CSS/CSSDescriptors.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-fonts-4/#cssfontfacedescriptors
class CSSFontFaceDescriptors final
    : public CSSDescriptors {
    WEB_PLATFORM_OBJECT(CSSFontFaceDescriptors, CSSDescriptors);
    GC_DECLARE_ALLOCATOR(CSSFontFaceDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSFontFaceDescriptors> create(JS::Realm&, Vector<Descriptor>);

    virtual ~CSSFontFaceDescriptors() override;

    virtual void initialize(JS::Realm&) override;
    virtual WebIDL::ExceptionOr<void> set_property(Utf16FlyString const& property, Utf16View value, Utf16View priority) override;

    WebIDL::ExceptionOr<void> set_ascent_override(Utf16View value);
    Utf16String ascent_override() const;

    WebIDL::ExceptionOr<void> set_descent_override(Utf16View value);
    Utf16String descent_override() const;

    WebIDL::ExceptionOr<void> set_font_display(Utf16View value);
    Utf16String font_display() const;

    WebIDL::ExceptionOr<void> set_font_family(Utf16View value);
    Utf16String font_family() const;

    WebIDL::ExceptionOr<void> set_font_feature_settings(Utf16View value);
    Utf16String font_feature_settings() const;

    WebIDL::ExceptionOr<void> set_font_language_override(Utf16View value);
    Utf16String font_language_override() const;

    WebIDL::ExceptionOr<void> set_font_named_instance(Utf16View value);
    Utf16String font_named_instance() const;

    WebIDL::ExceptionOr<void> set_font_style(Utf16View value);
    Utf16String font_style() const;

    WebIDL::ExceptionOr<void> set_font_variation_settings(Utf16View value);
    Utf16String font_variation_settings() const;

    WebIDL::ExceptionOr<void> set_font_weight(Utf16View value);
    Utf16String font_weight() const;

    WebIDL::ExceptionOr<void> set_font_width(Utf16View value);
    Utf16String font_width() const;

    WebIDL::ExceptionOr<void> set_line_gap_override(Utf16View value);
    Utf16String line_gap_override() const;

    WebIDL::ExceptionOr<void> set_src(Utf16View value);
    Utf16String src() const;

    WebIDL::ExceptionOr<void> set_unicode_range(Utf16View value);
    Utf16String unicode_range() const;

private:
    CSSFontFaceDescriptors(JS::Realm&, Vector<Descriptor>);
};

}
