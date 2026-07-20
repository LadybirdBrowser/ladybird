/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/EditingHistory.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Editing {

GC_DEFINE_ALLOCATOR(InsertNodeCommand);
GC_DEFINE_ALLOCATOR(RemoveNodeCommand);
GC_DEFINE_ALLOCATOR(ReplaceDataCommand);
GC_DEFINE_ALLOCATOR(SplitTextCommand);
GC_DEFINE_ALLOCATOR(SetAttributeCommand);

InsertNodeCommand::InsertNodeCommand(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> parent, GC::Ptr<DOM::Node> reference_child)
    : m_node(node)
    , m_parent(parent)
    , m_reference_child(reference_child)
{
}

void InsertNodeCommand::apply()
{
    m_parent->insert_before(m_node, m_reference_child);
}

bool InsertNodeCommand::unapply()
{
    if (!m_node->parent() || !m_node->parent()->is_editable_or_editing_host())
        return false;
    m_node->remove();
    return true;
}

bool InsertNodeCommand::reapply()
{
    if (!m_parent->is_editable_or_editing_host())
        return false;
    if (m_reference_child && m_reference_child->parent() != m_parent.ptr())
        return false;
    if (m_parent->ensure_pre_insert_validity(m_parent->realm(), m_node, m_reference_child, DOM::Node::ChildrenToExclude::None).is_error())
        return false;
    m_parent->insert_before(m_node, m_reference_child);
    return true;
}

void InsertNodeCommand::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_node);
    visitor.visit(m_parent);
    visitor.visit(m_reference_child);
}

RemoveNodeCommand::RemoveNodeCommand(GC::Ref<DOM::Node> node)
    : m_node(node)
    , m_parent(node->parent())
    , m_old_next_sibling(node->next_sibling())
{
}

void RemoveNodeCommand::apply()
{
    if (m_node->parent())
        m_node->remove();
}

bool RemoveNodeCommand::unapply()
{
    if (!m_parent || !m_parent->is_editable_or_editing_host())
        return false;
    auto reference_child = m_old_next_sibling && m_old_next_sibling->parent() == m_parent ? m_old_next_sibling : nullptr;
    if (m_parent->ensure_pre_insert_validity(m_parent->realm(), m_node, reference_child, DOM::Node::ChildrenToExclude::None).is_error())
        return false;
    m_parent->insert_before(m_node, reference_child);
    return true;
}

bool RemoveNodeCommand::is_lasting_node_removal() const
{
    return !m_node->parent();
}

bool RemoveNodeCommand::reapply()
{
    if (!m_node->parent() || !m_node->parent()->is_editable_or_editing_host())
        return false;
    m_node->remove();
    return true;
}

void RemoveNodeCommand::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_node);
    visitor.visit(m_parent);
    visitor.visit(m_old_next_sibling);
}

ReplaceDataCommand::ReplaceDataCommand(GC::Ref<DOM::CharacterData> node, size_t offset, Utf16String removed_data, Utf16String inserted_data)
    : m_node(node)
    , m_offset(offset)
    , m_removed_data(move(removed_data))
    , m_inserted_data(move(inserted_data))
{
}

// A text node backing a text control mirrors the control's value; its text must be replaced
// through the control so the value, its sanitization, and the control's bookkeeping stay
// coherent. Other engines do not need this distinction since their text controls edit inner
// text nodes directly.
static HTML::FormAssociatedTextControlElement* text_control_owner_of(DOM::CharacterData& node)
{
    auto* shadow_root = as_if<DOM::ShadowRoot>(node.root());
    if (!shadow_root)
        return nullptr;
    return dynamic_cast<HTML::FormAssociatedTextControlElement*>(shadow_root->host());
}

WebIDL::ExceptionOr<void> ReplaceDataCommand::apply()
{
    return m_node->replace_data(m_offset, m_removed_data.length_in_code_units(), m_inserted_data);
}

bool ReplaceDataCommand::replay(Utf16String const& from, Utf16String const& to)
{
    auto* control = text_control_owner_of(m_node);
    if (!control && !m_node->is_editable_or_editing_host())
        return false;

    // NB: Scripts may have rewritten the text since this command ran; leave it alone unless the
    //     recorded text is still where this command left it, since splicing at the recorded
    //     offsets would garble the rewritten content.
    auto current_data = control ? control->relevant_value() : m_node->data();
    auto length = from.length_in_code_units();
    if (m_offset + length > current_data.length_in_code_units())
        return false;
    if (current_data.substring_view(m_offset, length) != from)
        return false;

    if (control) {
        MUST(control->set_range_text(to, m_offset, m_offset + length, Bindings::SelectionMode::Preserve));
        return true;
    }
    return !m_node->replace_data(m_offset, length, to).is_error();
}

bool ReplaceDataCommand::unapply()
{
    return replay(m_inserted_data, m_removed_data);
}

bool ReplaceDataCommand::reapply()
{
    return replay(m_removed_data, m_inserted_data);
}

void ReplaceDataCommand::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_node);
}

SplitTextCommand::SplitTextCommand(GC::Ref<DOM::Text> node, size_t offset)
    : m_node(node)
    , m_offset(offset)
{
}

WebIDL::ExceptionOr<GC::Ref<DOM::Text>> SplitTextCommand::apply()
{
    m_new_node = TRY(m_node->split_text(m_offset));
    return GC::Ref { *m_new_node };
}

bool SplitTextCommand::unapply()
{
    if (!m_new_node || !m_node->is_editable_or_editing_host())
        return false;
    (void)m_node->replace_data(m_node->data().length_in_code_units(), 0, m_new_node->data());
    if (m_new_node->parent())
        m_new_node->remove();
    return true;
}

bool SplitTextCommand::reapply()
{
    if (!m_new_node || !m_node->is_editable_or_editing_host() || !m_node->parent())
        return false;
    auto length = m_node->data().length_in_code_units();
    if (m_offset > length)
        return false;
    (void)m_node->replace_data(m_offset, length - m_offset, {});
    m_node->parent()->insert_before(*m_new_node, m_node->next_sibling());
    return true;
}

void SplitTextCommand::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_node);
    visitor.visit(m_new_node);
}

SetAttributeCommand::SetAttributeCommand(GC::Ref<DOM::Element> element, Utf16FlyString local_name, Optional<Utf16FlyString> namespace_, Optional<Utf16String> old_value, Optional<Utf16String> new_value)
    : m_element(element)
    , m_local_name(move(local_name))
    , m_namespace(move(namespace_))
    , m_old_value(move(old_value))
    , m_new_value(move(new_value))
{
}

void SetAttributeCommand::set_or_remove(Optional<Utf16String> const& value)
{
    if (value.has_value()) {
        m_element->set_attribute_value(m_local_name, *value, {}, m_namespace);
    } else if (m_namespace.has_value()) {
        m_element->remove_attribute_ns(m_namespace, m_local_name);
    } else {
        m_element->remove_attribute(m_local_name);
    }
}

void SetAttributeCommand::apply()
{
    set_or_remove(m_new_value);
}

bool SetAttributeCommand::unapply()
{
    set_or_remove(m_old_value);
    return true;
}

bool SetAttributeCommand::reapply()
{
    set_or_remove(m_new_value);
    return true;
}

void SetAttributeCommand::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

static GC::Ptr<UndoStep> undo_step_being_recorded(DOM::Node const& node)
{
    auto history = node.document().editing_history_if_exists();
    if (!history)
        return {};
    return history->undo_step_being_recorded();
}

void insert_node_before(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> parent, GC::Ptr<DOM::Node> child)
{
    // Inserting a DocumentFragment inserts its children instead, like the DOM insertion algorithm.
    if (auto* fragment = as_if<DOM::DocumentFragment>(*node)) {
        GC::RootVector<GC::Ref<DOM::Node>> children;
        fragment->for_each_child([&children](DOM::Node& fragment_child) {
            children.append(fragment_child);
            return IterationDecision::Continue;
        });
        for (auto& fragment_child : children)
            insert_node_before(fragment_child, parent, child);
        return;
    }

    auto step = undo_step_being_recorded(parent);
    EditingHistory::ProxyMutationScope proxy_scope { parent };

    // NB: Moving a node records its removal first, so that undo can put it back where it came from. Children of a
    //     DocumentFragment are staging nodes, however: undo only removes them from the edited document, and redo
    //     inserts them again. Recording their temporary fragment parent would make the undo step depend on a
    //     non-editable, detached node.
    if (node->parent() && !is<DOM::DocumentFragment>(*node->parent())) {
        auto remove_command = parent->heap().allocate<RemoveNodeCommand>(node);
        remove_command->apply();
        if (step)
            step->add_command(*remove_command);
    }

    auto command = parent->heap().allocate<InsertNodeCommand>(node, parent, child);
    command->apply();
    if (step)
        step->add_command(*command);
}

WebIDL::ExceptionOr<void> append_node(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> parent)
{
    TRY(parent->ensure_pre_insert_validity(parent->realm(), node, nullptr, DOM::Node::ChildrenToExclude::None));
    insert_node_before(node, parent, nullptr);
    return {};
}

void remove_node(GC::Ref<DOM::Node> node)
{
    auto step = undo_step_being_recorded(node);
    EditingHistory::ProxyMutationScope proxy_scope { node };
    auto command = node->heap().allocate<RemoveNodeCommand>(node);
    command->apply();
    if (step)
        step->add_command(*command);
}

WebIDL::ExceptionOr<void> replace_data(GC::Ref<DOM::CharacterData> node, size_t offset, size_t count, Utf16View const& data)
{
    // Clamp count the same way CharacterData::replace_data() does, so the recorded removed
    // substring matches what the mutation actually removes.
    auto length = node->data().length_in_code_units();
    if (offset > length)
        return node->replace_data(offset, count, data);
    if (offset + count > length)
        count = length - offset;

    auto removed_data = Utf16String::from_utf16(node->data().substring_view(offset, count));
    bool is_no_op = removed_data.utf16_view() == data;

    EditingHistory::ProxyMutationScope proxy_scope { node };
    auto command = node->heap().allocate<ReplaceDataCommand>(node, offset, move(removed_data), Utf16String::from_utf16(data));
    TRY(command->apply());
    if (auto step = undo_step_being_recorded(node); step && !is_no_op)
        step->add_command(*command);
    return {};
}

WebIDL::ExceptionOr<void> insert_data(GC::Ref<DOM::CharacterData> node, size_t offset, Utf16View const& data)
{
    return replace_data(node, offset, 0, data);
}

WebIDL::ExceptionOr<void> delete_data(GC::Ref<DOM::CharacterData> node, size_t offset, size_t count)
{
    return replace_data(node, offset, count, {});
}

WebIDL::ExceptionOr<GC::Ref<DOM::Text>> split_text(GC::Ref<DOM::Text> node, size_t offset)
{
    auto step = undo_step_being_recorded(node);
    EditingHistory::ProxyMutationScope proxy_scope { node };
    auto command = node->heap().allocate<SplitTextCommand>(node, offset);
    auto new_node = TRY(command->apply());
    if (step)
        step->add_command(*command);
    return new_node;
}

void set_attribute_value(GC::Ref<DOM::Element> element, Utf16FlyString const& local_name, Utf16View value)
{
    auto step = undo_step_being_recorded(element);
    EditingHistory::ProxyMutationScope proxy_scope { element };
    auto command = element->heap().allocate<SetAttributeCommand>(element, local_name, Optional<Utf16FlyString> {}, element->get_attribute(local_name), Utf16String::from_utf16(value));
    command->apply();
    if (step)
        step->add_command(*command);
}

void remove_attribute(GC::Ref<DOM::Element> element, Utf16FlyString const& name)
{
    auto old_value = element->get_attribute(name);
    if (!old_value.has_value())
        return;
    auto step = undo_step_being_recorded(element);
    EditingHistory::ProxyMutationScope proxy_scope { element };
    auto command = element->heap().allocate<SetAttributeCommand>(element, name, Optional<Utf16FlyString> {}, move(old_value), Optional<Utf16String> {});
    command->apply();
    if (step)
        step->add_command(*command);
}

void remove_attribute_ns(GC::Ref<DOM::Element> element, Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name)
{
    auto old_value = element->get_attribute_ns(namespace_, name);
    if (!old_value.has_value())
        return;
    auto step = undo_step_being_recorded(element);
    EditingHistory::ProxyMutationScope proxy_scope { element };
    auto command = element->heap().allocate<SetAttributeCommand>(element, name, namespace_, move(old_value), Optional<Utf16String> {});
    command->apply();
    if (step)
        step->add_command(*command);
}

WebIDL::ExceptionOr<GC::Ref<DOM::Node>> clone_node_for_editing(GC::Ref<DOM::Node> node, bool subtree)
{
    EditingHistory::ProxyMutationScope mutation_scope { node };
    return node->clone_node(nullptr, subtree);
}

// AD-HOC: This mirrors Range::insert_node() step for step, but performs the split and the
//         insertion through reversible edit commands so they are recorded on the undo step.
//         https://dom.spec.whatwg.org/#concept-range-insert
WebIDL::ExceptionOr<void> insert_node_into_range(GC::Ref<DOM::Range> range, GC::Ref<DOM::Node> node)
{
    auto& realm = node->realm();
    auto start_container = range->start_container();

    // 1. If range's start node is a ProcessingInstruction or Comment node, is a Text node whose parent is null, or is
    //    node, then throw a "HierarchyRequestError" DOMException.
    if ((is<DOM::ProcessingInstruction>(*start_container) || is<DOM::Comment>(*start_container))
        || (is<DOM::Text>(*start_container) && !start_container->parent_node())
        || start_container.ptr() == node.ptr()) {
        return WebIDL::HierarchyRequestError::create(realm, "Range has inappropriate start node for insertion"_utf16);
    }

    // 2. Let referenceNode be null.
    // 3. If range's start node is a Text node, set referenceNode to that Text node.
    // 4. Otherwise, set referenceNode to the child of start node whose index is start offset, and null if there is no
    //    such child.
    GC::Ptr<DOM::Node> reference_node;
    if (is<DOM::Text>(*start_container))
        reference_node = start_container;
    else
        reference_node = start_container->child_at_index(range->start_offset());

    // 5. Let parent be range's start node if referenceNode is null, and referenceNode's parent otherwise.
    GC::Ptr<DOM::Node> parent = !reference_node ? GC::Ptr<DOM::Node> { start_container } : GC::Ptr<DOM::Node> { reference_node->parent() };

    // 6. Ensure pre-insert validity given node, parent, referenceNode, and « ».
    TRY(parent->ensure_pre_insert_validity(realm, node, reference_node, DOM::Node::ChildrenToExclude::None));

    // 7. If range's start node is a Text node, set referenceNode to the result of splitting it with offset range's
    //    start offset.
    if (auto* text = as_if<DOM::Text>(*start_container))
        reference_node = TRY(Editing::split_text(*text, range->start_offset()));

    // 8. If node is referenceNode, set referenceNode to its next sibling.
    if (node.ptr() == reference_node.ptr())
        reference_node = reference_node->next_sibling();

    // 9. If node's parent is non-null, then remove node.
    if (node->parent())
        Editing::remove_node(node);

    // 10. Let newOffset be parent's length if referenceNode is null, and referenceNode's index otherwise.
    size_t new_offset = !reference_node ? parent->length() : reference_node->index();

    // 11. Increase newOffset by node's length if node is a DocumentFragment node, and one otherwise.
    new_offset += is<DOM::DocumentFragment>(*node) ? node->length() : 1;

    // 12. Pre-insert node into parent before referenceNode.
    TRY(parent->ensure_pre_insert_validity(realm, node, reference_node, DOM::Node::ChildrenToExclude::None));
    Editing::insert_node_before(node, *parent, reference_node);

    // 13. If range is collapsed, then set range's end to (parent, newOffset).
    if (range->collapsed())
        TRY(range->set_end(*parent, new_offset));

    return {};
}

}
