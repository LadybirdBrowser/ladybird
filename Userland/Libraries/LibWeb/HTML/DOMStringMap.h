/*
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dom.html#domstringmap
class DOMStringMap final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DOMStringMap, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DOMStringMap);

public:
    [[nodiscard]] static JS::NonnullGCPtr<DOMStringMap> create(DOM::Element&);

    virtual ~DOMStringMap() override;

    String determine_value_of_named_property(FlyString const&) const;

    virtual WebIDL::ExceptionOr<void> set_value_of_new_named_property(String const&, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_named_property(String const&, JS::Value) override;

    virtual WebIDL::ExceptionOr<DidDeletionFail> delete_value(String const&) override;

private:
    explicit DOMStringMap(DOM::Element&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // ^PlatformObject
    virtual JS::Value named_item_value(FlyString const&) const override;
    virtual Vector<FlyString> supported_property_names() const override;

    struct NameValuePair {
        FlyString name;
        String value;
    };

    Vector<NameValuePair> get_name_value_pairs() const;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-domstringmap-element
    JS::NonnullGCPtr<DOM::Element> m_associated_element;
};

}
