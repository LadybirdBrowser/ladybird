/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class WEB_API Text
    : public CharacterData
    , public SlottableMixin {
    WEB_WRAPPABLE(Text, CharacterData);
    GC_DECLARE_ALLOCATOR(Text);

public:
    virtual ~Text() override = default;

    [[nodiscard]] static GC::Ref<Text> create(Document&, Utf16String data);
    [[nodiscard]] static GC::Ref<Text> create_for_constructor(JS::Object&, Utf16String data);

    // ^Node
    virtual Utf16FlyString node_name() const override { return "#text"_utf16_fly_string; }

    virtual Node& slottable_as_node() override { return *this; }

    Optional<size_t> max_length() const
    {
        if (m_text_core.max_length == RustFFI::TEXT_NO_MAX_LENGTH)
            return {};
        return m_text_core.max_length;
    }
    // NB: A form control's maximum length is a non-negative WebIDL long.
    void set_max_length(Optional<size_t> max_length)
    {
        VERIFY(!max_length.has_value() || *max_length < RustFFI::TEXT_NO_MAX_LENGTH);
        m_text_core.max_length = max_length.has_value() ? static_cast<u32>(*max_length) : RustFFI::TEXT_NO_MAX_LENGTH;
    }

    WebIDL::ExceptionOr<GC::Ref<Text>> split_text(size_t offset);
    Utf16String whole_text();

    bool is_password_input() const { return m_text_core.is_password_input; }
    void set_is_password_input(Badge<HTML::HTMLInputElement>, bool b) { m_text_core.is_password_input = b; }

    // https://dom.spec.whatwg.org/#slotable-assigned-slot, which the Text node keeps in its core.
    // NB: This hides SlottableMixin::assigned_slot_internal(), which has to find the core first. It is defined in
    //     HTML/HTMLSlotElement.h.
    inline GC::Ptr<HTML::HTMLSlotElement> assigned_slot_internal() const;
    Element* assigned_slot_in_core() const { return m_text_core.assigned_slot.ptr(); }
    void set_assigned_slot_in_core(Badge<SlottableMixin>, Element* slot) { m_text_core.assigned_slot = slot; }

    Optional<Element::Directionality> directionality() const;

    // The text node's StyleEngine identity, or 0 while it is disconnected.
    void set_style_node_id(CSS::StyleNodeID);

protected:
    Text(Document&, Utf16String);
    Text(Document&, NodeType, Utf16String);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void adopted_from(Document&) override;

private:
    struct RareData;
    virtual OwnPtr<Node::RareData> create_rare_data() const override;
    virtual SlottableMixin::RareData* slottable_rare_data() override;
    virtual SlottableMixin::RareData const* slottable_rare_data() const override;
    virtual SlottableMixin::RareData& ensure_slottable_rare_data() override;

    RustFFI::TextCore m_text_core { .assigned_slot = {}, .max_length = RustFFI::TEXT_NO_MAX_LENGTH, .is_password_input = false };
};

}
