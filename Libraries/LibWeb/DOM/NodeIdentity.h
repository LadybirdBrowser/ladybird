/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/Traits.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// Names a DOM node without pointing at it, so that layout and paint state can name a node without
// keeping it alive or reading it. An element, a text node and a shadow root are named by the
// StyleNodeID the style engine gave them; only the document, which has none, is named by itself.
// An identity whose node has left the tree resolves to nothing, where a pointer would have handed
// back a node the document no longer contains.
class WEB_API NodeIdentity {
public:
    NodeIdentity() = default;

    static NodeIdentity of(Node const&);
    static NodeIdentity of(Node const* node) { return node ? of(*node) : NodeIdentity {}; }
    static NodeIdentity of_style_node(CSS::StyleNodeID);
    static NodeIdentity of_document() { return { Kind::Document, {} }; }

    bool is_none() const { return m_kind == Kind::None; }
    explicit operator bool() const { return !is_none(); }
    bool operator==(NodeIdentity const&) const = default;
    [[nodiscard]] unsigned hash() const { return pair_int_hash(m_style_node.value(), to_underlying(m_kind)); }

    // The StyleNodeID this names, which is none for the document and for no node at all.
    [[nodiscard]] CSS::StyleNodeID style_node() const { return m_kind == Kind::StyleNode ? m_style_node : CSS::StyleNodeID {}; }

    [[nodiscard]] GC::Ptr<Node> resolve(Document&) const;
    // The layout node the arena has bound to this identity's node, if any. No DOM node is asked for its layout node.
    [[nodiscard]] Layout::Node* bound_layout_node(Layout::BegunRead const& read, Layout::NodeArena&) const;
    // Whether `layout_node`, which the caller holds, is the one the arena has bound to this identity's node.
    [[nodiscard]] bool binds(Layout::Node const& layout_node) const;

private:
    enum class Kind : u8 {
        None,
        StyleNode,
        Document,
    };

    NodeIdentity(Kind kind, CSS::StyleNodeID style_node)
        : m_style_node(style_node)
        , m_kind(kind)
    {
    }

    CSS::StyleNodeID m_style_node {};
    Kind m_kind { Kind::None };
};

}

namespace AK {

template<>
struct Traits<Web::DOM::NodeIdentity> : public DefaultTraits<Web::DOM::NodeIdentity> {
    static unsigned hash(Web::DOM::NodeIdentity const& identity) { return identity.hash(); }
};

}
