/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Slottable.h>

namespace Web::DOM {

class EditableTextNodeOwner {
public:
    virtual ~EditableTextNodeOwner() = default;
    virtual void did_edit_text_node(Badge<Document>) = 0;
};

class Text
    : public CharacterData
    , public SlottableMixin {
    WEB_PLATFORM_OBJECT(Text, CharacterData);
    JS_DECLARE_ALLOCATOR(Text);

public:
    virtual ~Text() override = default;

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Text>> construct_impl(JS::Realm& realm, String const& data);

    // ^Node
    virtual FlyString node_name() const override { return "#text"_fly_string; }
    virtual bool is_editable() const override { return m_always_editable || CharacterData::is_editable(); }

    void set_always_editable(bool b) { m_always_editable = b; }

    Optional<size_t> max_length() const { return m_max_length; }
    void set_max_length(Optional<size_t> max_length) { m_max_length = move(max_length); }

    template<DerivedFrom<EditableTextNodeOwner> T>
    void set_editable_text_node_owner(Badge<T>, Element& owner_element) { m_owner = &owner_element; }
    EditableTextNodeOwner* editable_text_node_owner();

    WebIDL::ExceptionOr<JS::NonnullGCPtr<Text>> split_text(size_t offset);
    String whole_text();

    bool is_password_input() const { return m_is_password_input; }
    void set_is_password_input(Badge<HTML::HTMLInputElement>, bool b) { m_is_password_input = b; }

    Optional<Element::Directionality> directionality() const;

protected:
    Text(Document&, String const&);
    Text(Document&, NodeType, String const&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    JS::GCPtr<Element> m_owner;

    bool m_always_editable { false };
    Optional<size_t> m_max_length {};
    bool m_is_password_input { false };
};

template<>
inline bool Node::fast_is<Text>() const { return is_text(); }

}
