/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeClassHeadersGenerated.h>
#include <LibWeb/DOM/NodeClassId.h>

namespace Web::DOM {

// Whether DOM/NodeClasses.json lists T under the class it derives from.
template<typename T>
static constexpr bool is_listed_under_its_base()
{
    if constexpr (IsSame<T, Node>)
        return NodeClassTraits<T>::parent == NodeClassId::NotANode;
    else
        return NodeClassTraits<T>::parent == NodeClassTraits<typename T::Base>::id;
}

static constexpr u8 tree_steps = RustFFI::NODE_CLASS_HAS_INSERTION_STEPS | RustFFI::NODE_CLASS_HAS_REMOVING_STEPS
    | RustFFI::NODE_CLASS_HAS_MOVING_STEPS | RustFFI::NODE_CLASS_HAS_POST_CONNECTION_STEPS;

// NB: Every class DOM/NodeClasses.json lists is checked here, including those no node is ever constructed as, and
//     GC::CellConstructionHook in DOM/Node.h checks that it lists every class a node is constructed as. libweb_rust
//     takes the tree steps of character data and doctypes to be those of every node.
#define __ENUMERATE_NODE_CLASS(T)                                                                                                       \
    static_assert(is_listed_under_its_base<T>(), "DOM/NodeClasses.json must list each class of nodes under the class it derives from"); \
    static_assert(!(IsBaseOf<CharacterData, T> || IsBaseOf<DocumentType, T>) || !(GC::CellConstructionHook<T>::node_class_steps() & tree_steps));
ENUMERATE_NODE_CLASSES(__ENUMERATE_NODE_CLASS)
#undef __ENUMERATE_NODE_CLASS

static consteval Array<u8, node_class_id_count> compute_node_class_steps()
{
    Array<u8, node_class_id_count> steps {};
#define __ENUMERATE_NODE_CLASS(T) \
    steps[to_underlying(NodeClassTraits<T>::id)] = GC::CellConstructionHook<T>::node_class_steps();
    ENUMERATE_NODE_CLASSES(__ENUMERATE_NODE_CLASS)
#undef __ENUMERATE_NODE_CLASS
    return steps;
}

constinit Array<u8, node_class_id_count> const ladybird_dom_node_class_steps = compute_node_class_steps();

}
