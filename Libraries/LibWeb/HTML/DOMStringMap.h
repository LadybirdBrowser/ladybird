/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DOMStringMap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#domstringmap
class DOMStringMap final : public Bindings::Wrappable {
    WEB_WRAPPABLE(DOMStringMap, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DOMStringMap);

public:
    [[nodiscard]] static GC::Ref<DOMStringMap> create(DOM::Element&);

    virtual ~DOMStringMap() override;

    String determine_value_of_named_property(FlyString const&) const;

    virtual WebIDL::ExceptionOr<void> set_value_of_new_named_property(String const&, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_named_property(String const&, JS::Value) override;

    virtual WebIDL::ExceptionOr<Bindings::NamedPropertyDeletionResult> delete_value(String const&) override;

private:
    explicit DOMStringMap(DOM::Element&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // ^Wrappable
    virtual JS::Value named_item_value(JS::Realm&, FlyString const&) const override;
    virtual Vector<FlyString> supported_property_names() const override;

    struct NameValuePair {
        FlyString name;
        String value;
    };

    Vector<NameValuePair> get_name_value_pairs() const;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-domstringmap-element
    GC::Ref<DOM::Element> m_associated_element;
};

}
