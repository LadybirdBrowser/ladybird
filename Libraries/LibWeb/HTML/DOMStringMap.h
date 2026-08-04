/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#domstringmap
class DOMStringMap final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(DOMStringMap, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(DOMStringMap);

public:
    [[nodiscard]] static GC::Ref<DOMStringMap> create(DOM::Element&);

    virtual ~DOMStringMap() override;

    Utf16String determine_value_of_named_property(Utf16View) const;

    WebIDL::ExceptionOr<void> set_value_of_named_property(Utf16FlyString const&, Utf16String const&);
    void delete_named_property(Utf16FlyString const&);

private:
    explicit DOMStringMap(DOM::Element&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    virtual Vector<Utf16FlyString> supported_property_names() const override;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-domstringmap-element
    GC::Ref<DOM::Element> m_associated_element;
};

}
