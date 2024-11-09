/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <LibXML/DOM/Node.h>

namespace XML {

bool Node::operator==(Node const& other) const
{
    return content.visit(
        [&](Text const& text) -> bool {
            auto other_text = other.content.get_pointer<Text>();
            if (!other_text)
                return false;
            return text.builder.string_view() == other_text->builder.string_view();
        },
        [&](Comment const& comment) -> bool {
            auto other_comment = other.content.get_pointer<Comment>();
            if (!other_comment)
                return false;
            return comment.text == other_comment->text;
        },
        [&](Element const& element) -> bool {
            auto other_element = other.content.get_pointer<Element>();
            if (!other_element)
                return false;
            if (element.name != other_element->name)
                return false;
            if (element.attributes.size() != other_element->attributes.size())
                return false;

            for (auto& entry : element.attributes) {
                auto it = other_element->attributes.find(entry.key);
                if (it == other_element->attributes.end())
                    return false;
                if (it->value != entry.value)
                    return false;
            }

            if (element.children.size() != other_element->children.size())
                return false;
            for (size_t i = 0; i < element.children.size(); ++i) {
                if (element.children[i].ptr() != other_element->children[i].ptr())
                    return false;
            }
            return true;
        });
}

}
