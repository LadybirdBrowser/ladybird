/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#mutationrecord
class MutationRecord : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(MutationRecord, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(MutationRecord);

public:
    static constexpr size_t target_offset() { return offsetof(MutationRecord, m_target); }
    static constexpr size_t added_nodes_offset() { return offsetof(MutationRecord, m_added_nodes); }
    static constexpr size_t removed_nodes_offset() { return offsetof(MutationRecord, m_removed_nodes); }
    [[nodiscard]] static GC::Ref<MutationRecord> create(Utf16FlyString const& type, Node const& target, NodeList& added_nodes, NodeList& removed_nodes, Node* previous_sibling, Node* next_sibling, Optional<Utf16FlyString> const& attribute_name, Optional<Utf16FlyString> const& attribute_namespace, Optional<Utf16String> const& old_value);

    virtual ~MutationRecord() override;

    Utf16FlyString const& type() const { return m_type; }
    Node const* target() const { return m_target.ptr(); }
    NodeList const* added_nodes() const { return m_added_nodes.ptr(); }
    NodeList const* removed_nodes() const { return m_removed_nodes.ptr(); }
    Node const* previous_sibling() const { return m_previous_sibling.ptr(); }
    Node const* next_sibling() const { return m_next_sibling.ptr(); }
    static constexpr size_t previous_sibling_offset() { return offsetof(MutationRecord, m_previous_sibling); }
    static constexpr size_t next_sibling_offset() { return offsetof(MutationRecord, m_next_sibling); }
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
    MutationRecord(Utf16FlyString const& type, Node const& target, NodeList& added_nodes, NodeList& removed_nodes, Node* previous_sibling, Node* next_sibling, Optional<Utf16FlyString> const& attribute_name, Optional<Utf16FlyString> const& attribute_namespace, Optional<Utf16String> const& old_value);

    virtual void visit_edges(GC::Cell::Visitor&) override;

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
