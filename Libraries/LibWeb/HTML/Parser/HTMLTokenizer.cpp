/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/NeverDestroyed.h>
#include <AK/Vector.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTMLTokenizerRustFFI.h>

namespace Web::HTML {

static Vector<u32> code_points_from_utf16_view(Utf16View input)
{
    Vector<u32> code_points;
    code_points.ensure_capacity(input.length_in_code_units());
    for (auto code_point : input)
        code_points.append(is_unicode_surrogate(code_point) ? AK::UnicodeUtils::REPLACEMENT_CODE_POINT : code_point);
    return code_points;
}

static RustFfiTokenizerHandle* create_tokenizer_from_utf16(Utf16View input)
{
    auto code_points = code_points_from_utf16_view(input);
    return rust_html_tokenizer_create(code_points.data(), code_points.size());
}

static Utf16FlyString utf16_fly_string_from_ffi(u16 const* ptr, size_t len)
{
    if (!ptr || len == 0)
        return {};
    return Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(ptr), len });
}

static Utf16String utf16_string_from_ffi(u16 const* ptr, size_t len)
{
    if (!ptr || len == 0)
        return {};
    return Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(ptr), len });
}

static Vector<Utf16FlyString> build_interned_tag_name_table()
{
    Vector<Utf16FlyString> table;
    // Slot 0 is unused (id 0 means "not interned"); store an empty Utf16FlyString there.
    table.append(Utf16FlyString {});
#define __ENUMERATE_HTML_TAG(name, tag) table.append(TagNames::name);
    ENUMERATE_HTML_TAGS
#undef __ENUMERATE_HTML_TAG
    VERIFY(table.size() == rust_html_tokenizer_interned_tag_name_count() + 1);
    return table;
}

static Vector<Utf16FlyString> build_interned_attr_name_table()
{
    Vector<Utf16FlyString> table;
    // Slot 0 is unused (id 0 means "not interned"); store an empty Utf16FlyString there.
    table.append(Utf16FlyString {});
#define __ENUMERATE_HTML_ATTRIBUTE(name, attribute) table.append(AttributeNames::name);
    ENUMERATE_HTML_ATTRIBUTES
#undef __ENUMERATE_HTML_ATTRIBUTE
    VERIFY(table.size() == rust_html_tokenizer_interned_attr_name_count() + 1);
    return table;
}

static Utf16FlyString const& interned_rust_tag_name(uint16_t id)
{
    static NeverDestroyed<Vector<Utf16FlyString>> table { build_interned_tag_name_table() };
    if (id == 0 || id >= table->size())
        return (*table)[0];
    return (*table)[id];
}

static Utf16FlyString const& interned_rust_attr_name(uint16_t id)
{
    static NeverDestroyed<Vector<Utf16FlyString>> table { build_interned_attr_name_table() };
    if (id == 0 || id >= table->size())
        return (*table)[0];
    return (*table)[id];
}

HTMLTokenizer::HTMLTokenizer()
{
    m_tokenizer = create_tokenizer_from_utf16({});
    rust_html_tokenizer_set_input_stream_closed(m_tokenizer, false);
}

HTMLTokenizer::~HTMLTokenizer()
{
    if (m_tokenizer)
        rust_html_tokenizer_destroy(m_tokenizer);
}

HTMLTokenizer::HTMLTokenizer(Utf16View input)
    : m_source(Utf16String::from_utf16(input))
    , m_input_stream_closed(true)
{
    m_tokenizer = create_tokenizer_from_utf16(input);
}

HTMLTokenizer::HTMLTokenizer(Utf16String input)
    : m_source(move(input))
    , m_input_stream_closed(true)
{
    m_tokenizer = create_tokenizer_from_utf16(m_source.utf16_view());
}

Optional<HTMLToken> HTMLTokenizer::next_token(StopAtInsertionPoint stop_at_insertion_point)
{
    RustFfiToken ffi;
    bool stop = stop_at_insertion_point == StopAtInsertionPoint::Yes;
    bool cdata_allowed = false;
    if (!rust_html_tokenizer_next_token(m_tokenizer, &ffi, stop, cdata_allowed))
        return {};

    HTMLToken::Type type;
    switch (ffi.token_type) {
    case 1:
        type = HTMLToken::Type::DOCTYPE;
        break;
    case 2:
        type = HTMLToken::Type::StartTag;
        break;
    case 3:
        type = HTMLToken::Type::EndTag;
        break;
    case 4:
        type = HTMLToken::Type::Comment;
        break;
    case 5:
        type = HTMLToken::Type::Character;
        break;
    case 6:
        type = HTMLToken::Type::EndOfFile;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    HTMLToken token { type };
    token.set_start_position({}, { ffi.start_line, ffi.start_column });
    token.set_end_position({}, { ffi.end_line, ffi.end_column });

    switch (type) {
    case HTMLToken::Type::Character:
        token.set_code_point(ffi.code_point);
        break;
    case HTMLToken::Type::StartTag:
    case HTMLToken::Type::EndTag: {
        if (ffi.tag_name_id != 0)
            token.set_tag_name(interned_rust_tag_name(ffi.tag_name_id));
        else
            token.set_tag_name(utf16_fly_string_from_ffi(ffi.tag_name_ptr, ffi.tag_name_len));

        token.set_self_closing(ffi.self_closing);
        for (size_t i = 0; i < ffi.attributes_len; ++i) {
            auto const& ffi_attribute = ffi.attributes_ptr[i];
            HTMLToken::Attribute attribute;
            if (ffi_attribute.name_id != 0)
                attribute.local_name = interned_rust_attr_name(ffi_attribute.name_id);
            else
                attribute.local_name = utf16_fly_string_from_ffi(ffi_attribute.name_ptr, ffi_attribute.name_len);
            attribute.value = utf16_string_from_ffi(ffi_attribute.value_ptr, ffi_attribute.value_len);
            attribute.name_start_position = { ffi_attribute.name_start_line, ffi_attribute.name_start_column };
            attribute.name_end_position = { ffi_attribute.name_end_line, ffi_attribute.name_end_column };
            attribute.value_start_position = { ffi_attribute.value_start_line, ffi_attribute.value_start_column };
            attribute.value_end_position = { ffi_attribute.value_end_line, ffi_attribute.value_end_column };
            token.add_attribute(move(attribute));
        }
        token.normalize_attributes();
        if (ffi.had_duplicate_attribute)
            token.set_had_duplicate_attribute({});
        break;
    }
    case HTMLToken::Type::Comment:
        token.set_comment(utf16_string_from_ffi(ffi.comment_ptr, ffi.comment_len));
        break;
    case HTMLToken::Type::DOCTYPE: {
        auto& doctype = token.ensure_doctype_data();
        if (!ffi.missing_name) {
            doctype.name = utf16_fly_string_from_ffi(ffi.doctype_name_ptr, ffi.doctype_name_len);
            doctype.missing_name = false;
        }
        if (!ffi.missing_public_id) {
            doctype.public_identifier = utf16_string_from_ffi(ffi.public_id_ptr, ffi.public_id_len);
            doctype.missing_public_identifier = false;
        }
        if (!ffi.missing_system_id) {
            doctype.system_identifier = utf16_string_from_ffi(ffi.system_id_ptr, ffi.system_id_len);
            doctype.missing_system_identifier = false;
        }
        doctype.force_quirks = ffi.force_quirks;
        break;
    }
    case HTMLToken::Type::EndOfFile:
        break;
    case HTMLToken::Type::Invalid:
        VERIFY_NOT_REACHED();
    }

    return token;
}

void HTMLTokenizer::parser_did_run(Badge<HTMLParser>)
{
    rust_html_tokenizer_parser_did_run(m_tokenizer);
}

Utf16String HTMLTokenizer::unparsed_input() const
{
    u16 const* ptr = nullptr;
    size_t len = 0;
    rust_html_tokenizer_unparsed_input(m_tokenizer, &ptr, &len);
    return utf16_string_from_ffi(ptr, len);
}

void HTMLTokenizer::append_to_input_stream(Utf16View input)
{
    if (input.is_empty())
        return;

    auto code_points = code_points_from_utf16_view(input);
    rust_html_tokenizer_append_input(m_tokenizer, code_points.data(), code_points.size());
}

void HTMLTokenizer::close_input_stream()
{
    m_input_stream_closed = true;
    rust_html_tokenizer_set_input_stream_closed(m_tokenizer, true);
}

void HTMLTokenizer::insert_input_at_insertion_point(Utf16View input)
{
    auto code_points = code_points_from_utf16_view(input);
    rust_html_tokenizer_insert_input(m_tokenizer, code_points.data(), code_points.size());
}

void HTMLTokenizer::insert_eof()
{
    close_input_stream();
    rust_html_tokenizer_insert_eof(m_tokenizer);
}

bool HTMLTokenizer::is_insertion_point_defined() const
{
    return rust_html_tokenizer_is_insertion_point_defined(m_tokenizer);
}

bool HTMLTokenizer::is_insertion_point_reached()
{
    return rust_html_tokenizer_is_insertion_point_reached(m_tokenizer);
}

void HTMLTokenizer::undefine_insertion_point()
{
    rust_html_tokenizer_undefine_insertion_point(m_tokenizer);
}

void HTMLTokenizer::store_insertion_point()
{
    rust_html_tokenizer_store_insertion_point(m_tokenizer);
}

void HTMLTokenizer::restore_insertion_point()
{
    rust_html_tokenizer_restore_insertion_point(m_tokenizer);
}

void HTMLTokenizer::update_insertion_point()
{
    rust_html_tokenizer_update_insertion_point(m_tokenizer);
}

void HTMLTokenizer::abort()
{
    rust_html_tokenizer_abort(m_tokenizer);
}

void HTMLTokenizer::switch_to(State new_state)
{
    dbgln_if(TOKENIZER_TRACE_DEBUG, "[{}] Switch to {}", state_name(m_state), state_name(new_state));
    m_state = new_state;
    rust_html_tokenizer_switch_state(m_tokenizer, static_cast<uint8_t>(new_state));
}

}
