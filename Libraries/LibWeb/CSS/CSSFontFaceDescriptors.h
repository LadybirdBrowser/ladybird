/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

private:
    CSSFontFaceDescriptors(JS::Realm&, Vector<Descriptor>);
};

}
