/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Max Wipfli <max.wipfli@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>

namespace Web::HTML {

class HTMLTokenizer;

class HTMLToken {
    AK_MAKE_NONCOPYABLE(HTMLToken);
    AK_MAKE_DEFAULT_MOVABLE(HTMLToken);

public:
    enum class Type : u8 {
        Invalid,
        DOCTYPE,
        StartTag,
        EndTag,
        Comment,
        Character,
        EndOfFile,
    };

    struct Position {
        size_t line { 0 };
        size_t column { 0 };
        size_t byte_offset { 0 };
    };

    struct Attribute {
        Optional<FlyString> prefix;
        FlyString local_name;
        Optional<FlyString> namespace_;
        String value;
        Position name_start_position;
        Position value_start_position;
        Position name_end_position;
        Position value_end_position;
    };

    struct DoctypeData {
        // NOTE: "Missing" is a distinct state from the empty string.
        String name;
        String public_identifier;
        String system_identifier;
        bool missing_name { true };
        bool missing_public_identifier { true };
        bool missing_system_identifier { true };
        bool force_quirks { false };
    };

    static HTMLToken make_character(u32 code_point)
    {
        HTMLToken token { Type::Character };
        token.set_code_point(code_point);
        return token;
    }

    static HTMLToken make_start_tag(FlyString const& tag_name)
    {
        HTMLToken token { Type::StartTag };
        token.set_tag_name(tag_name);
        return token;
    }

    HTMLToken() = default;

    HTMLToken(Type type)
        : m_type(type)
    {
        switch (m_type) {
        case Type::Character:
            m_data.set(0u);
            break;
        case Type::DOCTYPE:
            m_data.set(OwnPtr<DoctypeData> {});
            break;
        case Type::StartTag:
        case Type::EndTag:
            m_data.set(OwnPtr<Vector<Attribute>>());
            break;
        default:
            break;
        }
    }

    bool is_doctype() const { return m_type == Type::DOCTYPE; }
    bool is_start_tag() const { return m_type == Type::StartTag; }
    bool is_end_tag() const { return m_type == Type::EndTag; }
    bool is_comment() const { return m_type == Type::Comment; }
    bool is_character() const { return m_type == Type::Character; }
    bool is_end_of_file() const { return m_type == Type::EndOfFile; }

    u32 code_point() const
    {
        VERIFY(is_character());
        return m_data.get<u32>();
    }

    bool is_parser_whitespace() const
    {
        // NOTE: The parser considers '\r' to be whitespace, while the tokenizer does not.
        if (!is_character())
            return false;
        switch (code_point()) {
        case '\t':
        case '\n':
        case '\f':
        case '\r':
        case ' ':
            return true;
        default:
            return false;
        }
    }

    void set_code_point(u32 code_point)
    {
        VERIFY(is_character());
        m_data.get<u32>() = code_point;
    }

    String const& comment() const
    {
        VERIFY(is_comment());
        return m_comment_data;
    }

    void set_comment(String comment)
    {
        VERIFY(is_comment());
        m_comment_data = move(comment);
    }

    FlyString const& tag_name() const
    {
        VERIFY(is_start_tag() || is_end_tag());
        return m_string_data;
    }

    void set_tag_name(FlyString name)
    {
        VERIFY(is_start_tag() || is_end_tag());
        m_string_data = move(name);
    }

    bool is_self_closing() const
    {
        VERIFY(is_start_tag() || is_end_tag());
        return m_tag_self_closing;
    }

    void set_self_closing(bool self_closing)
    {
        VERIFY(is_start_tag() || is_end_tag());
        m_tag_self_closing = self_closing;
    }

    bool has_acknowledged_self_closing_flag() const
    {
        VERIFY(is_self_closing());
        return m_tag_self_closing_acknowledged;
    }

    void acknowledge_self_closing_flag_if_set()
    {
        if (is_self_closing())
            m_tag_self_closing_acknowledged = true;
    }

    bool has_attributes() const
    {
        VERIFY(is_start_tag() || is_end_tag());
        auto* ptr = tag_attributes();
        return ptr && !ptr->is_empty();
    }

    size_t attribute_count() const
    {
        VERIFY(is_start_tag() || is_end_tag());
        if (auto* ptr = tag_attributes())
            return ptr->size();
        return 0;
    }

    void add_attribute(Attribute attribute)
    {
        VERIFY(is_start_tag() || is_end_tag());
        ensure_tag_attributes().append(move(attribute));
    }

    Attribute const& last_attribute() const
    {
        VERIFY(is_start_tag() || is_end_tag());
        VERIFY(has_attributes());
        return tag_attributes()->last();
    }

    Attribute& last_attribute()
    {
        VERIFY(is_start_tag() || is_end_tag());
        VERIFY(has_attributes());
        return tag_attributes()->last();
    }

    void drop_attributes()
    {
        VERIFY(is_start_tag() || is_end_tag());
        m_data.get<OwnPtr<Vector<Attribute>>>().clear();
    }

    void for_each_attribute(Function<IterationDecision(Attribute const&)> callback) const
    {
        VERIFY(is_start_tag() || is_end_tag());
        auto* ptr = tag_attributes();
        if (!ptr)
            return;
        for (auto& attribute : *ptr) {
            if (callback(attribute) == IterationDecision::Break)
                break;
        }
    }

    void for_each_attribute(Function<IterationDecision(Attribute&)> callback)
    {
        VERIFY(is_start_tag() || is_end_tag());
        auto* ptr = tag_attributes();
        if (!ptr)
            return;
        for (auto& attribute : *ptr) {
            if (callback(attribute) == IterationDecision::Break)
                break;
        }
    }

    Optional<String> attribute(FlyString const& attribute_name) const
    {
        if (auto result = raw_attribute(attribute_name); result.has_value())
            return result->value;
        return {};
    }

    Optional<Attribute const&> raw_attribute(FlyString const& attribute_name) const
    {
        VERIFY(is_start_tag() || is_end_tag());

        auto* ptr = tag_attributes();
        if (!ptr)
            return {};
        for (auto const& attribute : *ptr) {
            if (attribute_name == attribute.local_name)
                return attribute;
        }
        return {};
    }

    bool has_attribute(FlyString const& attribute_name) const
    {
        return attribute(attribute_name).has_value();
    }

    void adjust_tag_name(FlyString const& old_name, FlyString const& new_name)
    {
        VERIFY(is_start_tag() || is_end_tag());
        if (old_name == tag_name())
            set_tag_name(new_name);
    }

    void adjust_attribute_name(FlyString const& old_name, FlyString const& new_name)
    {
        VERIFY(is_start_tag() || is_end_tag());
        for_each_attribute([&](Attribute& attribute) {
            if (old_name == attribute.local_name)
                attribute.local_name = new_name;
            return IterationDecision::Continue;
        });
    }

    void adjust_foreign_attribute(FlyString const& old_name, Optional<FlyString> const& prefix, FlyString const& local_name, Optional<FlyString> const& namespace_)
    {
        VERIFY(is_start_tag() || is_end_tag());
        for_each_attribute([&](Attribute& attribute) {
            if (old_name == attribute.local_name) {
                attribute.prefix = prefix;
                attribute.local_name = local_name;
                attribute.namespace_ = namespace_;
            }
            return IterationDecision::Continue;
        });
    }

    DoctypeData const& doctype_data() const
    {
        VERIFY(is_doctype());
        auto* ptr = m_data.get<OwnPtr<DoctypeData>>().ptr();
        VERIFY(ptr);
        return *ptr;
    }

    DoctypeData& ensure_doctype_data()
    {
        VERIFY(is_doctype());
        auto& ptr = m_data.get<OwnPtr<DoctypeData>>();
        if (!ptr)
            ptr = make<DoctypeData>();
        return *ptr;
    }

    Type type() const { return m_type; }

    String to_string() const;

    Position const& start_position() const { return m_start_position; }
    Position const& end_position() const { return m_end_position; }

    void set_start_position(Badge<HTMLTokenizer>, Position start_position) { m_start_position = start_position; }
    void set_end_position(Badge<HTMLTokenizer>, Position end_position) { m_end_position = end_position; }

    void normalize_attributes();

private:
    Vector<Attribute> const* tag_attributes() const
    {
        return m_data.get<OwnPtr<Vector<Attribute>>>().ptr();
    }

    Vector<Attribute>* tag_attributes()
    {
        return m_data.get<OwnPtr<Vector<Attribute>>>().ptr();
    }

    Vector<Attribute>& ensure_tag_attributes()
    {
        VERIFY(is_start_tag() || is_end_tag());
        auto& ptr = m_data.get<OwnPtr<Vector<Attribute>>>();
        if (!ptr)
            ptr = make<Vector<Attribute>>();
        return *ptr;
    }

    Type m_type { Type::Invalid };

    // Type::StartTag and Type::EndTag
    bool m_tag_self_closing { false };
    bool m_tag_self_closing_acknowledged { false };

    // Type::StartTag and Type::EndTag (tag name)
    FlyString m_string_data;

    // Type::Comment (comment data)
    String m_comment_data;

    Variant<Empty, u32, OwnPtr<DoctypeData>, OwnPtr<Vector<Attribute>>> m_data {};

    Position m_start_position;
    Position m_end_position;
};

}
