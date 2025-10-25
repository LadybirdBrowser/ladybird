/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/QualifiedName.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#attr
class WEB_API Attr final : public Node {
    WEB_PLATFORM_OBJECT(Attr, Node);
    GC_DECLARE_ALLOCATOR(Attr);

public:
    [[nodiscard]] static GC::Ref<Attr> create(Document&, QualifiedName, String value = {}, Element* = nullptr);
    [[nodiscard]] static GC::Ref<Attr> create(Document&, FlyString const& local_name, String value = {}, Element* = nullptr);
    GC::Ref<Attr> clone(Document&) const;

    virtual ~Attr() override = default;

    virtual FlyString node_name() const override { return name(); }

    Optional<FlyString> const& namespace_uri() const { return m_qualified_name.namespace_(); }
    Optional<FlyString> const& prefix() const { return m_qualified_name.prefix(); }
    FlyString const& local_name() const { return m_qualified_name.local_name(); }
    FlyString const& name() const { return m_qualified_name.as_string(); }
    FlyString const& lowercase_name() const { return m_lowercase_name; }

    String const& value() const { return m_value; }
    void set_value(String value);
    void change_attribute(String value);

    Element* owner_element();
    Element const* owner_element() const;
    void set_owner_element(Element* owner_element);

    // Always returns true: https://dom.spec.whatwg.org/#dom-attr-specified
    constexpr bool specified() const { return true; }

    void handle_attribute_changes(Element&, Optional<String> const& old_value, Optional<String> const& new_value);

private:
    Attr(Document&, QualifiedName, String value, Element*);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    QualifiedName m_qualified_name;
    FlyString m_lowercase_name;
    String m_value;
    GC::Ptr<Element> m_owner_element;
};

template<>
inline bool Node::fast_is<Attr>() const { return is_attribute(); }

}
