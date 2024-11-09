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
    JS_DECLARE_ALLOCATOR(HTMLFormControlsCollection);

public:
    [[nodiscard]] static JS::NonnullGCPtr<HTMLFormControlsCollection> create(DOM::ParentNode& root, Scope, ESCAPING Function<bool(DOM::Element const&)> filter);

    virtual ~HTMLFormControlsCollection() override;

    Variant<Empty, DOM::Element*, JS::Handle<RadioNodeList>> named_item_or_radio_node_list(FlyString const& name) const;

protected:
    virtual void initialize(JS::Realm&) override;

    virtual JS::Value named_item_value(FlyString const& name) const final;

private:
    HTMLFormControlsCollection(DOM::ParentNode& root, Scope, ESCAPING Function<bool(DOM::Element const&)> filter);
};

}
