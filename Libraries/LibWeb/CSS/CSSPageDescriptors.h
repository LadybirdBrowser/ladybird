/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSDescriptors.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom/#csspagedescriptors
class CSSPageDescriptors final : public CSSDescriptors {
    WEB_PLATFORM_OBJECT(CSSPageDescriptors, CSSDescriptors);
    GC_DECLARE_ALLOCATOR(CSSPageDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSPageDescriptors> create(JS::Realm&, Vector<Descriptor>);

    virtual ~CSSPageDescriptors() override;

    virtual void initialize(JS::Realm&) override;

    WebIDL::ExceptionOr<void> set_margin(StringView value);
    String margin() const;

    WebIDL::ExceptionOr<void> set_margin_top(StringView value);
    String margin_top() const;

    WebIDL::ExceptionOr<void> set_margin_right(StringView value);
    String margin_right() const;

    WebIDL::ExceptionOr<void> set_margin_bottom(StringView value);
    String margin_bottom() const;

    WebIDL::ExceptionOr<void> set_margin_left(StringView value);
    String margin_left() const;

    WebIDL::ExceptionOr<void> set_size(StringView value);
    String size() const;

    WebIDL::ExceptionOr<void> set_page_orientation(StringView value);
    String page_orientation() const;

    WebIDL::ExceptionOr<void> set_marks(StringView value);
    String marks() const;

    WebIDL::ExceptionOr<void> set_bleed(StringView value);
    String bleed() const;

private:
    CSSPageDescriptors(JS::Realm&, Vector<Descriptor>);
};

}
