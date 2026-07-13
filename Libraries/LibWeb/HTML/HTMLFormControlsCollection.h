/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/HTMLCollection.h>

namespace Web::HTML {

class HTMLFormControlsCollection : public DOM::HTMLCollection {
    WEB_PLATFORM_OBJECT(HTMLFormControlsCollection, DOM::HTMLCollection);
    GC_DECLARE_ALLOCATOR(HTMLFormControlsCollection);

public:
    [[nodiscard]] static GC::Ref<HTMLFormControlsCollection> create(DOM::ParentNode& root, Scope, ESCAPING Function<bool(DOM::Element const&)> filter);

    virtual ~HTMLFormControlsCollection() override;

    Variant<Empty, GC::Ref<DOM::Element>, GC::Ref<RadioNodeList>> named_item_or_radio_node_list(Utf16View name) const;

protected:
    virtual void initialize(JS::Realm&) override;

    virtual JS::Value named_item_value(Utf16FlyString const& name) const final;

private:
    HTMLFormControlsCollection(DOM::ParentNode& root, Scope, ESCAPING Function<bool(DOM::Element const&)> filter);

    DOM::Element* first_matching_named_element(Utf16View name, bool& multiple_matching) const;
    GC::Ref<RadioNodeList> create_radio_node_list(Utf16String name) const;
    GC::Ref<RadioNodeList> create_radio_node_list(Utf16FlyString name) const;
};

}
