/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DocumentState);

DocumentState::DocumentState() = default;

DocumentState::~DocumentState() = default;

void DocumentState::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    m_history_policy_container.visit(
        [&](GC::Ref<PolicyContainer> const& policy_container) { visitor.visit(policy_container); },
        [](auto const&) {});
    for (auto& nested_history : m_nested_histories) {
        visitor.visit(nested_history.entries);
    }
}

}
