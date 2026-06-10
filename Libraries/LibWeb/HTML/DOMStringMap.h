/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#domstringmap
class DOMStringMap final : public Bindings::Wrappable {
    WEB_WRAPPABLE(DOMStringMap, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DOMStringMap);

public:
    [[nodiscard]] static GC::Ref<DOMStringMap> create(DOM::Element&);

    virtual ~DOMStringMap() override;

    String determine_value_of_named_property(FlyString const&) const;
    WebIDL::ExceptionOr<void> set_value_of_named_property(String const&, String const&);
    void delete_named_property(String const&);

private:
    explicit DOMStringMap(DOM::Element&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // ^Wrappable
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
