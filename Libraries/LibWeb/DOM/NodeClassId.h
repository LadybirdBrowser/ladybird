/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/DOM/NodeClassesGenerated.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

// Each class of DOM nodes has a NodeClassId, which DOM/NodeClasses.json gives it: the file lists the classes as a tree,
// which libweb_rust's build script numbers in depth-first order, from 1 for DOM::Node. The classes that descend from a
// class have the ids right after its own, up to NodeClassTraits<T>::last_descendant, so whether a node is of a class T,
// or of a class that descends from it, is whether its class id is in T's range.
//
// A node keeps the id of the class it was constructed as in the header of its cell (see Bindings::Wrappable), where C++
// and libweb_rust (see Rust/src/dom/node_class.rs) read it. GC::CellConstructionHook in DOM/Node.h writes it, and fails
// to compile for a class of nodes the file does not list. DOM/NodeClassSteps.cpp fails to compile where the file lists
// a class under another class than the one it derives from.
// An event target that is no node has NodeClassId::NotANode.

template<typename T>
concept HasNodeClassId = requires { NodeClassTraits<T>::id; };

// Whether `id` is the id of T, or of a class that descends from T.
template<HasNodeClassId T>
constexpr bool node_class_id_is_a(NodeClassId id)
{
    constexpr auto first = to_underlying(NodeClassTraits<T>::id);
    constexpr auto last = to_underlying(NodeClassTraits<T>::last_descendant);
    // NB: The subtraction wraps for an id below T's, so that one comparison tells both bounds.
    return static_cast<u16>(to_underlying(id) - first) <= last - first;
}

// The RustFFI::NODE_CLASS_HAS_* bits of each class of nodes, by class id, which say which steps the class has. See
// DOM/NodeClassSteps.cpp.
extern "C" WEB_API Array<u8, node_class_id_count> const ladybird_dom_node_class_steps;

// The RustFFI::NODE_CLASS_HAS_* bits of the class whose id is `id`.
inline u8 node_class_steps(NodeClassId id)
{
    return ladybird_dom_node_class_steps.data()[to_underlying(id)];
}

}
