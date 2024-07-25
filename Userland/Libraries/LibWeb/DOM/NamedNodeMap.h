/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Alexander Narsudinov <a.narsudinov@gmail.com>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#interface-namednodemap
class NamedNodeMap : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(NamedNodeMap, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(NamedNodeMap);

public:
    [[nodiscard]] static JS::NonnullGCPtr<NamedNodeMap> create(Element&);
    ~NamedNodeMap() = default;

    virtual Vector<FlyString> supported_property_names() const override;
    virtual Optional<JS::Value> item_value(size_t index) const override;
    virtual JS::Value named_item_value(FlyString const& name) const override;

    size_t length() const { return m_attributes.size(); }
    bool is_empty() const { return m_attributes.is_empty(); }

    // Methods defined by the spec for JavaScript:
    Attr const* item(u32 index) const;
    Attr const* get_named_item(FlyString const& qualified_name) const;
    Attr const* get_named_item_ns(Optional<FlyString> const& namespace_, FlyString const& local_name) const;
    WebIDL::ExceptionOr<JS::GCPtr<Attr>> set_named_item(Attr& attribute);
    WebIDL::ExceptionOr<JS::GCPtr<Attr>> set_named_item_ns(Attr& attribute);
    WebIDL::ExceptionOr<Attr const*> remove_named_item(FlyString const& qualified_name);
    WebIDL::ExceptionOr<Attr const*> remove_named_item_ns(Optional<FlyString> const& namespace_, FlyString const& local_name);

    // Methods defined by the spec for internal use:
    Attr* get_attribute(FlyString const& qualified_name, size_t* item_index = nullptr);
    Attr const* get_attribute(FlyString const& qualified_name, size_t* item_index = nullptr) const;
    WebIDL::ExceptionOr<JS::GCPtr<Attr>> set_attribute(Attr& attribute);
    void replace_attribute(Attr& old_attribute, Attr& new_attribute, size_t old_attribute_index);
    void append_attribute(Attr& attribute);

    Attr* get_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& local_name, size_t* item_index = nullptr);
    Attr const* get_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& local_name, size_t* item_index = nullptr) const;

    Attr const* remove_attribute(FlyString const& qualified_name);
    Attr const* remove_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& local_name);

    Attr const* get_attribute_with_lowercase_qualified_name(FlyString const&) const;

    WebIDL::ExceptionOr<JS::NonnullGCPtr<Attr>> remove_attribute_node(JS::NonnullGCPtr<Attr>);

private:
    explicit NamedNodeMap(Element&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Element& associated_element() { return *m_element; }
    Element const& associated_element() const { return *m_element; }

    void remove_attribute_at_index(size_t attribute_index);

    JS::NonnullGCPtr<DOM::Element> m_element;
    Vector<JS::NonnullGCPtr<Attr>> m_attributes;
};

}
