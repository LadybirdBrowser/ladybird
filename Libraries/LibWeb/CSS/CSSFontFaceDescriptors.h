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
    WEB_WRAPPABLE(CSSFontFaceDescriptors, CSSDescriptors);
    GC_DECLARE_ALLOCATOR(CSSFontFaceDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSFontFaceDescriptors> create(Vector<Descriptor>);

    virtual ~CSSFontFaceDescriptors() override;
    virtual WebIDL::ExceptionOr<void> set_property(JS::Realm&, Utf16FlyString const& property, StringView value, StringView priority) override;

    WebIDL::ExceptionOr<void> set_ascent_override(JS::Realm&, StringView value);
    String ascent_override() const;

    WebIDL::ExceptionOr<void> set_descent_override(JS::Realm&, StringView value);
    String descent_override() const;

    WebIDL::ExceptionOr<void> set_font_display(JS::Realm&, StringView value);
    String font_display() const;

    WebIDL::ExceptionOr<void> set_font_family(JS::Realm&, StringView value);
    String font_family() const;

    WebIDL::ExceptionOr<void> set_font_feature_settings(JS::Realm&, StringView value);
    String font_feature_settings() const;

    WebIDL::ExceptionOr<void> set_font_language_override(JS::Realm&, StringView value);
    String font_language_override() const;

    WebIDL::ExceptionOr<void> set_font_named_instance(JS::Realm&, StringView value);
    String font_named_instance() const;

    WebIDL::ExceptionOr<void> set_font_style(JS::Realm&, StringView value);
    String font_style() const;

    WebIDL::ExceptionOr<void> set_font_variation_settings(JS::Realm&, StringView value);
    String font_variation_settings() const;

    WebIDL::ExceptionOr<void> set_font_weight(JS::Realm&, StringView value);
    String font_weight() const;

    WebIDL::ExceptionOr<void> set_font_width(JS::Realm&, StringView value);
    String font_width() const;

    WebIDL::ExceptionOr<void> set_line_gap_override(JS::Realm&, StringView value);
    String line_gap_override() const;

    WebIDL::ExceptionOr<void> set_src(JS::Realm&, StringView value);
    String src() const;

    WebIDL::ExceptionOr<void> set_unicode_range(JS::Realm&, StringView value);
    String unicode_range() const;

private:
    explicit CSSFontFaceDescriptors(Vector<Descriptor>);
};

}
