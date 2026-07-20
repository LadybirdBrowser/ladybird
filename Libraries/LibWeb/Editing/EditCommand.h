/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Editing {

// A single reversible DOM mutation performed by an editing command, modeled after the simple
// edit command hierarchies in WebKit and Blink. The mutation itself happens when the command is
// created and applied through one of the proxy functions below; unapply() and reapply() replay
// it through the editing history. Scripts may have rearranged the DOM since the command first
// ran, so both re-validate their preconditions and become no-ops when the mutation no longer
// makes sense. Commands keep strong references to their nodes so the original objects survive
// on the undo stack and undo can reinsert the very same nodes.
class EditCommand : public JS::Cell {
    GC_CELL(EditCommand, JS::Cell);

public:
    virtual ~EditCommand() override = default;

    // Both return whether the mutation was actually performed; a command declines when scripts
    // have since rearranged the DOM in a way that makes its replay meaningless.
    virtual bool unapply() = 0;
    virtual bool reapply() = 0;

    // Whether this command removed a node from the document that the rest of its editing action
    // did not put back. Used to end typing coalescence, since Blink closes an open typing unit
    // when a node near the selection is removed.
    virtual bool is_lasting_node_removal() const { return false; }
};

class InsertNodeCommand final : public EditCommand {
    GC_CELL(InsertNodeCommand, EditCommand);
    GC_DECLARE_ALLOCATOR(InsertNodeCommand);

public:
    InsertNodeCommand(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> parent, GC::Ptr<DOM::Node> reference_child);

    void apply();
    virtual bool unapply() override;
    virtual bool reapply() override;

private:
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOM::Node> m_node;
    GC::Ref<DOM::Node> m_parent;
    GC::Ptr<DOM::Node> m_reference_child;
};

class RemoveNodeCommand final : public EditCommand {
    GC_CELL(RemoveNodeCommand, EditCommand);
    GC_DECLARE_ALLOCATOR(RemoveNodeCommand);

public:
    explicit RemoveNodeCommand(GC::Ref<DOM::Node> node);

    void apply();
    virtual bool unapply() override;
    virtual bool reapply() override;

    virtual bool is_lasting_node_removal() const override;

private:
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOM::Node> m_node;
    GC::Ptr<DOM::Node> m_parent;
    GC::Ptr<DOM::Node> m_old_next_sibling;
};

class ReplaceDataCommand final : public EditCommand {
    GC_CELL(ReplaceDataCommand, EditCommand);
    GC_DECLARE_ALLOCATOR(ReplaceDataCommand);

public:
    ReplaceDataCommand(GC::Ref<DOM::CharacterData> node, size_t offset, Utf16String removed_data, Utf16String inserted_data);

    WebIDL::ExceptionOr<void> apply();
    virtual bool unapply() override;
    virtual bool reapply() override;

    GC::Ref<DOM::CharacterData> node() const { return m_node; }
    size_t offset() const { return m_offset; }
    Utf16String const& removed_data() const { return m_removed_data; }

private:
    virtual void visit_edges(Cell::Visitor&) override;

    bool replay(Utf16String const& from, Utf16String const& to);

    GC::Ref<DOM::CharacterData> m_node;
    size_t m_offset { 0 };
    Utf16String m_removed_data;
    Utf16String m_inserted_data;
};

class SplitTextCommand final : public EditCommand {
    GC_CELL(SplitTextCommand, EditCommand);
    GC_DECLARE_ALLOCATOR(SplitTextCommand);

public:
    SplitTextCommand(GC::Ref<DOM::Text> node, size_t offset);

    WebIDL::ExceptionOr<GC::Ref<DOM::Text>> apply();
    virtual bool unapply() override;
    virtual bool reapply() override;

private:
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOM::Text> m_node;
    size_t m_offset { 0 };
    GC::Ptr<DOM::Text> m_new_node;
};

class SetAttributeCommand final : public EditCommand {
    GC_CELL(SetAttributeCommand, EditCommand);
    GC_DECLARE_ALLOCATOR(SetAttributeCommand);

public:
    SetAttributeCommand(GC::Ref<DOM::Element> element, Utf16FlyString local_name, Optional<Utf16FlyString> namespace_, Optional<Utf16String> old_value, Optional<Utf16String> new_value);

    void apply();
    virtual bool unapply() override;
    virtual bool reapply() override;

private:
    virtual void visit_edges(Cell::Visitor&) override;

    void set_or_remove(Optional<Utf16String> const& value);

    GC::Ref<DOM::Element> m_element;
    Utf16FlyString m_local_name;
    Optional<Utf16FlyString> m_namespace;
    Optional<Utf16String> m_old_value;
    Optional<Utf16String> m_new_value;
};

// Proxy for the DOM mutations performed by editing command implementations. Each function
// performs the same mutation as the corresponding DOM API, but does so through a reversible
// EditCommand which is recorded on the document's editing history whenever a user editing
// command is being executed (Document::exec_command() opens that recording around each command
// action). Editing code must mutate the DOM through these functions and never through the raw
// DOM APIs, or the mutation silently escapes undo; EditingHistory::ProxyMutationScope diagnoses
// such bypasses at runtime. Script-driven DOM mutation never passes through here and therefore
// never enters the history. Moving an already-parented node records its removal first, so undo
// restores it to where it came from, and inserting a DocumentFragment records its children
// individually, like the DOM insertion algorithm.
void insert_node_before(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> parent, GC::Ptr<DOM::Node> child);
WebIDL::ExceptionOr<void> append_node(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> parent);
void remove_node(GC::Ref<DOM::Node>);
WebIDL::ExceptionOr<void> replace_data(GC::Ref<DOM::CharacterData>, size_t offset, size_t count, Utf16View const& data);
WebIDL::ExceptionOr<void> insert_data(GC::Ref<DOM::CharacterData>, size_t offset, Utf16View const& data);
WebIDL::ExceptionOr<void> delete_data(GC::Ref<DOM::CharacterData>, size_t offset, size_t count);
WebIDL::ExceptionOr<GC::Ref<DOM::Text>> split_text(GC::Ref<DOM::Text>, size_t offset);
void set_attribute_value(GC::Ref<DOM::Element>, Utf16FlyString const& local_name, Utf16View value);
void remove_attribute(GC::Ref<DOM::Element>, Utf16FlyString const& name);
void remove_attribute_ns(GC::Ref<DOM::Element>, Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name);

// Create detached staging content without diagnosing its internal construction as an unrecorded edit. The eventual
// insertion of the completed clone still goes through an InsertNodeCommand and is therefore fully reversible.
WebIDL::ExceptionOr<GC::Ref<DOM::Node>> clone_node_for_editing(GC::Ref<DOM::Node>, bool subtree);

// AD-HOC: Equivalent of Range::insert_node() that performs its mutations through reversible
//         edit commands. https://dom.spec.whatwg.org/#concept-range-insert
WebIDL::ExceptionOr<void> insert_node_into_range(GC::Ref<DOM::Range>, GC::Ref<DOM::Node>);

}
