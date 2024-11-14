/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class HTMLAllCollection : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(HTMLAllCollection, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(HTMLAllCollection);

public:
    enum class Scope {
        Children,
        Descendants,
    };
    [[nodiscard]] static GC::Ref<HTMLAllCollection> create(DOM::ParentNode& root, Scope, ESCAPING Function<bool(DOM::Element const&)> filter);

    virtual ~HTMLAllCollection() override;

    size_t length() const;
    Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> item(Optional<FlyString> const& name_or_index) const;
    Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> named_item(FlyString const& name) const;

    GC::MarkedVector<GC::Ref<DOM::Element>> collect_matching_elements() const;

    virtual Optional<JS::Value> item_value(size_t index) const override;
    virtual JS::Value named_item_value(FlyString const& name) const override;
    virtual Vector<FlyString> supported_property_names() const override;

protected:
    HTMLAllCollection(DOM::ParentNode& root, Scope, ESCAPING Function<bool(DOM::Element const&)> filter);

    virtual void initialize(JS::Realm&) override;

    virtual bool is_htmldda() const override { return true; }

private:
    Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> get_the_all_named_elements(FlyString const& name) const;
    GC::Ptr<DOM::Element> get_the_all_indexed_element(u32 index) const;
    Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> get_the_all_indexed_or_named_elements(JS::PropertyKey const& name_or_index) const;

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOM::ParentNode> m_root;
    Function<bool(DOM::Element const&)> m_filter;
    Scope m_scope { Scope::Descendants };
};

}
