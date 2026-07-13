/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#mutationrecord
class MutationRecord : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MutationRecord, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MutationRecord);

public:
    [[nodiscard]] static GC::Ref<MutationRecord> create(JS::Realm&, Utf16FlyString const& type, Node const& target, NodeList& added_nodes, NodeList& removed_nodes, Node* previous_sibling, Node* next_sibling, Optional<Utf16FlyString> const& attribute_name, Optional<Utf16FlyString> const& attribute_namespace, Optional<Utf16String> const& old_value);

    virtual ~MutationRecord() override;

    Utf16FlyString const& type() const { return m_type; }
    Node const* target() const { return m_target; }
    NodeList const* added_nodes() const { return m_added_nodes; }
    NodeList const* removed_nodes() const { return m_removed_nodes; }
    Node const* previous_sibling() const { return m_previous_sibling; }
    Node const* next_sibling() const { return m_next_sibling; }
    Optional<Utf16String> attribute_name() const
    {
        return m_attribute_name.map([](auto const& name) { return name.to_utf16_string(); });
    }
    Optional<Utf16String> attribute_namespace() const
    {
        return m_attribute_namespace.map([](auto const& namespace_) { return namespace_.to_utf16_string(); });
    }
    Optional<Utf16String> const& old_value() const { return m_old_value; }

private:
    MutationRecord(JS::Realm& realm, Utf16FlyString const& type, Node const& target, NodeList& added_nodes, NodeList& removed_nodes, Node* previous_sibling, Node* next_sibling, Optional<Utf16FlyString> const& attribute_name, Optional<Utf16FlyString> const& attribute_namespace, Optional<Utf16String> const& old_value);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Utf16FlyString m_type;
    GC::Ptr<Node const> m_target;
    GC::Ptr<NodeList> m_added_nodes;
    GC::Ptr<NodeList> m_removed_nodes;
    GC::Ptr<Node> m_previous_sibling;
    GC::Ptr<Node> m_next_sibling;
    Optional<Utf16FlyString> m_attribute_name;
    Optional<Utf16FlyString> m_attribute_namespace;
    Optional<Utf16String> m_old_value;
};

}
