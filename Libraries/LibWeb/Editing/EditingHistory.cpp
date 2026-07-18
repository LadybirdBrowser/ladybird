/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/EditingHistory.h>

namespace Web::Editing {

GC_DEFINE_ALLOCATOR(UndoStep);
GC_DEFINE_ALLOCATOR(EditingHistory);

UndoStep::UndoStep(GC::Ref<DOM::Node> editing_host)
    : m_editing_host(editing_host)
{
}

void UndoStep::unapply()
{
    for (auto command : m_commands.in_reverse())
        command->unapply();
}

void UndoStep::reapply()
{
    for (auto command : m_commands)
        command->reapply();
}

void UndoStep::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_editing_host);
    visitor.visit(m_commands);
}

GC::Ref<EditingHistory> EditingHistory::create(JS::Realm& realm)
{
    return realm.heap().allocate<EditingHistory>();
}

void EditingHistory::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_undo_step_being_recorded);
    visitor.visit(m_undo_stack);
    visitor.visit(m_redo_stack);
}

}
