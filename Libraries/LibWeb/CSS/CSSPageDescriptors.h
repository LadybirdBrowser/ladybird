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

// https://drafts.csswg.org/cssom/#csspagedescriptors
class CSSPageDescriptors final : public CSSDescriptors {
    WEB_WRAPPABLE(CSSPageDescriptors, CSSDescriptors);
    GC_DECLARE_ALLOCATOR(CSSPageDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSPageDescriptors> create(Vector<Descriptor>);

    virtual ~CSSPageDescriptors() override;

    WebIDL::ExceptionOr<void> set_margin(Utf16View value);
    Utf16String margin() const;

    WebIDL::ExceptionOr<void> set_margin_top(Utf16View value);
    Utf16String margin_top() const;

    WebIDL::ExceptionOr<void> set_margin_right(Utf16View value);
    Utf16String margin_right() const;

    WebIDL::ExceptionOr<void> set_margin_bottom(Utf16View value);
    Utf16String margin_bottom() const;

    WebIDL::ExceptionOr<void> set_margin_left(Utf16View value);
    Utf16String margin_left() const;

    WebIDL::ExceptionOr<void> set_size(Utf16View value);
    Utf16String size() const;

    WebIDL::ExceptionOr<void> set_page_orientation(Utf16View value);
    Utf16String page_orientation() const;

    WebIDL::ExceptionOr<void> set_marks(Utf16View value);
    Utf16String marks() const;

    WebIDL::ExceptionOr<void> set_bleed(Utf16View value);
    Utf16String bleed() const;

private:
    explicit CSSPageDescriptors(Vector<Descriptor>);
};

}
