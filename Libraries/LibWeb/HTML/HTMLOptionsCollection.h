/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

using HTMLOptionOrOptGroupElement = Variant<GC::Root<HTMLOptionElement>, GC::Root<HTMLOptGroupElement>>;
using HTMLElementOrElementIndex = Variant<GC::Root<HTMLElement>, i32>;

class HTMLOptionsCollection final : public DOM::HTMLCollection {
    WEB_PLATFORM_OBJECT(HTMLOptionsCollection, DOM::HTMLCollection);
    GC_DECLARE_ALLOCATOR(HTMLOptionsCollection);

public:
    [[nodiscard]] static GC::Ref<HTMLOptionsCollection> create(DOM::ParentNode& root, ESCAPING Function<bool(DOM::Element const&)> filter);
    virtual ~HTMLOptionsCollection() override;

    WebIDL::ExceptionOr<void> set_value_of_indexed_property(u32, JS::Value) override;

    WebIDL::ExceptionOr<void> set_length(WebIDL::UnsignedLong);

    WebIDL::ExceptionOr<void> add(HTMLOptionOrOptGroupElement element, Optional<HTMLElementOrElementIndex> before = {});

    void remove(WebIDL::Long);

    WebIDL::Long selected_index() const;
    WebIDL::ExceptionOr<void> set_selected_index(WebIDL::Long);

private:
    HTMLOptionsCollection(DOM::ParentNode& root, ESCAPING Function<bool(DOM::Element const&)> filter);

    virtual void initialize(JS::Realm&) override;
};

}
