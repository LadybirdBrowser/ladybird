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
    WEB_WRAPPABLE(CSSPageDescriptors, CSSDescriptors);
    GC_DECLARE_ALLOCATOR(CSSPageDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSPageDescriptors> create(Vector<Descriptor>);

    virtual ~CSSPageDescriptors() override;

    WebIDL::ExceptionOr<void> set_margin(JS::Realm&, StringView value);
    String margin() const;

    WebIDL::ExceptionOr<void> set_margin_top(JS::Realm&, StringView value);
    String margin_top() const;

    WebIDL::ExceptionOr<void> set_margin_right(JS::Realm&, StringView value);
    String margin_right() const;

    WebIDL::ExceptionOr<void> set_margin_bottom(JS::Realm&, StringView value);
    String margin_bottom() const;

    WebIDL::ExceptionOr<void> set_margin_left(JS::Realm&, StringView value);
    String margin_left() const;

    WebIDL::ExceptionOr<void> set_size(JS::Realm&, StringView value);
    String size() const;

    WebIDL::ExceptionOr<void> set_page_orientation(JS::Realm&, StringView value);
    String page_orientation() const;

    WebIDL::ExceptionOr<void> set_marks(JS::Realm&, StringView value);
    String marks() const;

    WebIDL::ExceptionOr<void> set_bleed(JS::Realm&, StringView value);
    String bleed() const;

private:
    explicit CSSPageDescriptors(Vector<Descriptor>);
};

}
