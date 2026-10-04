/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeIdentity.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>

namespace Web::DOM {

// What layout found out and has to tell the document, in one ordered list. A message names the node it is about by
// identity and says what layout found out about it; layout itself writes nothing on the DOM side. The document applies
// the messages in the order layout produced them, where they are delivered.
class WEB_API CommitMessages {
    AK_MAKE_NONCOPYABLE(CommitMessages);
    AK_MAKE_NONMOVABLE(CommitMessages);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit CommitMessages(Document& document)
        : m_document(document)
    {
    }

    void visit_edges(GC::Cell::Visitor&);

    // A message a finished layout pass or tree build left for this document.
    void append(Layout::BegunRead const& read, Layout::RustFFI::FfiCommitMessage const&);

    // Applies every message in order and empties the list.
    void apply(Layout::BegunRead const&);

private:
    enum class Kind : u8 {
        ContentSizeChangedForContainerQueries,
        NavigableContainerViewportCommitted,
        UnexpectedFragmentedInline,
        NeedsLayoutTreeUpdate,
        TopLayerZoneRebuildNeeded,
        ListItemCounterValueRendered,
        SvgResourceReferenced,
        UnstyledElementReached,
    };

    struct Message {
        NodeIdentity identity;
        // The second node a message about a pair names. Only SvgResourceReferenced has one.
        NodeIdentity other_identity {};
        Kind kind;
        // Only the layout tree update trace reads this.
        SetNeedsLayoutTreeUpdateReason layout_tree_update_reason { SetNeedsLayoutTreeUpdateReason::None };
    };

    void apply(Layout::BegunRead const& read, Message const&);
    Layout::Node* bound_layout_node(Layout::BegunRead const& read, NodeIdentity) const;

    GC::Ref<Document> m_document;
    Vector<Message> m_messages;
    bool m_applying { false };
};

}
