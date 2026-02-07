/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/Export.h>

namespace Web::DOM {

class WEB_API Text
    : public CharacterData
    , public SlottableMixin {
    WEB_PLATFORM_OBJECT(Text, CharacterData);
    GC_DECLARE_ALLOCATOR(Text);

public:
    virtual ~Text() override = default;

    static WebIDL::ExceptionOr<GC::Ref<Text>> construct_impl(JS::Realm& realm, Utf16String data);

    // ^Node
    virtual FlyString node_name() const override { return "#text"_fly_string; }

    virtual Node& slottable_as_node() override { return *this; }

    Optional<size_t> max_length() const { return m_max_length; }
    void set_max_length(Optional<size_t> max_length) { m_max_length = move(max_length); }

    WebIDL::ExceptionOr<GC::Ref<Text>> split_text(size_t offset);
    Utf16String whole_text();

    bool is_password_input() const { return m_is_password_input; }
    void set_is_password_input(Badge<HTML::HTMLInputElement>, bool b) { m_is_password_input = b; }

    Optional<Element::Directionality> directionality() const;

protected:
    Text(Document&, Utf16String);
    Text(Document&, NodeType, Utf16String);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    Optional<size_t> m_max_length {};
    bool m_is_password_input { false };
};

template<>
inline bool Node::fast_is<Text>() const { return is_text(); }

}
