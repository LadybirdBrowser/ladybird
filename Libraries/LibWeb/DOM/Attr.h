/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/WeakPtr.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/QualifiedName.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#attr
class WEB_API Attr final : public Node {
    WEB_WRAPPABLE(Attr, Node);
    GC_DECLARE_ALLOCATOR(Attr);

public:
    [[nodiscard]] static GC::Ref<Attr> create(Document&, QualifiedName, Utf16String value = {}, GC::Ptr<Element> = nullptr);
    [[nodiscard]] static GC::Ref<Attr> create(Document&, Utf16FlyString local_name, Utf16String value = {}, GC::Ptr<Element> = nullptr);
    GC::Ref<Attr> clone(Document&) const;

    virtual ~Attr() override = default;

    virtual Utf16FlyString node_name() const override { return m_qualified_name.as_string(); }

    Optional<Utf16FlyString> const& namespace_uri() const { return m_qualified_name.namespace_(); }
    Optional<Utf16FlyString> const& prefix() const { return m_qualified_name.prefix(); }
    Utf16FlyString const& local_name() const { return m_qualified_name.local_name(); }
    Utf16FlyString const& name() const { return m_qualified_name.as_string(); }

    Utf16String value() const;
    WebIDL::ExceptionOr<void> set_value(Utf16String value);
    WebIDL::ExceptionOr<void> set_value(Utf16View value) { return set_value(Utf16String::from_utf16(value)); }
    void change_attribute(Utf16String value);
    void change_attribute(Utf16View value) { change_attribute(Utf16String::from_utf16(value)); }

    Element* owner_element();
    Element const* owner_element() const;
    void set_owner_element(Element* owner_element);
    static constexpr size_t owner_element_offset() { return offsetof(Attr, m_owner_element); }

    // Always returns true: https://dom.spec.whatwg.org/#dom-attr-specified
    constexpr bool specified() const { return true; }

private:
    friend class NamedNodeMap;

    void detach_from_element(Utf16String value);

    Attr(Document&, QualifiedName, Utf16String value, GC::Ptr<Element>);
    virtual void visit_edges(Cell::Visitor&) override;

    QualifiedName m_qualified_name;
    Utf16String m_value;
    GC::Ptr<Element> m_owner_element;
};

template<>
inline bool Node::fast_is<Attr>() const { return is_attribute(); }

}
