/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#domstringmap
class DOMStringMap final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DOMStringMap, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DOMStringMap);

public:
    [[nodiscard]] static GC::Ref<DOMStringMap> create(DOM::Element&);

    virtual ~DOMStringMap() override;

    Utf16String determine_value_of_named_property(Utf16View) const;

    virtual WebIDL::ExceptionOr<void> set_value_of_new_named_property(Utf16FlyString const&, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_named_property(Utf16FlyString const&, JS::Value) override;

    virtual WebIDL::ExceptionOr<DidDeletionFail> delete_value(Utf16FlyString const&) override;

private:
    explicit DOMStringMap(DOM::Element&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // ^PlatformObject
    virtual JS::Value named_item_value(Utf16FlyString const&) const override;
    virtual Vector<Utf16FlyString> supported_property_names() const override;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-domstringmap-element
    GC::Ref<DOM::Element> m_associated_element;
};

}
