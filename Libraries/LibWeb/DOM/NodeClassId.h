/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <LibWeb/DOM/NodeClassesGenerated.h>

namespace Web::DOM {

// Each class of DOM nodes has a NodeClassId, which DOM/NodeClasses.json gives it: the file lists the classes as a tree,
// which libweb_rust's build script numbers in depth-first order, from 1 for DOM::Node. The classes that descend from a
// class have the ids right after its own, up to NodeClassTraits<T>::last_descendant, so whether a node is of a class T,
// or of a class that descends from it, is whether its class id is in T's range.
//
// A node keeps the id of the class it was constructed as in the header of its cell (see Bindings::Wrappable).
// GC::CellConstructionHook in DOM/Node.h writes it, and fails to compile for a class of nodes the file does not list, or
// lists under another class than the one it derives from.
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

}
