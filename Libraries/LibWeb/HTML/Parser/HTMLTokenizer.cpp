/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/FlyString.h>
#include <AK/Vector.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>
#include <LibWeb/HTMLTokenizerRustFFI.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

static Vector<u32> code_points_from_string(String const& string)
{
    Vector<u32> code_points;
    code_points.ensure_capacity(string.bytes().size());
    for (auto code_point : string.code_points())
        code_points.append(code_point);
    return code_points;
}

static StringView ffi_string_view(u8 const* ptr, size_t len)
{
    if (ptr == nullptr || len == 0)
        return {};
    return { ptr, len };
}

static RustFfiTokenizerHandle* create_tokenizer_from_utf8(StringView utf8_bytes)
{
    auto* bytes = reinterpret_cast<u8 const*>(utf8_bytes.characters_without_null_termination());
    if (bytes == nullptr)
        bytes = reinterpret_cast<u8 const*>("");
    return rust_html_tokenizer_create_from_utf8(bytes, utf8_bytes.length());
}

static Vector<FlyString> build_interned_name_table(size_t count, void (*fetch)(uint16_t, uint8_t const**, size_t*))
{
    Vector<FlyString> table;
    // Slot 0 is unused (id 0 means "not interned"); store an empty FlyString there.
    table.append(FlyString {});
    table.ensure_capacity(count + 1);
    for (size_t i = 0; i < count; ++i) {
        uint8_t const* ptr = nullptr;
        size_t len = 0;
        fetch(static_cast<uint16_t>(i + 1), &ptr, &len);
        if (ptr == nullptr || len == 0) {
            table.append(FlyString {});
            continue;
        }
        table.append(MUST(FlyString::from_utf8(StringView { ptr, len })));
    }
    return table;
}

static FlyString const& interned_rust_tag_name(uint16_t id)
{
    static Vector<FlyString> const s_table = build_interned_name_table(
        rust_html_tokenizer_interned_tag_name_count(),
        rust_html_tokenizer_interned_tag_name);
    if (id == 0 || id >= s_table.size())
        return s_table[0];
    return s_table[id];
}

static FlyString const& interned_rust_attr_name(uint16_t id)
{
    static Vector<FlyString> const s_table = build_interned_name_table(
        rust_html_tokenizer_interned_attr_name_count(),
        rust_html_tokenizer_interned_attr_name);
    if (id == 0 || id >= s_table.size())
        return s_table[0];
    return s_table[id];
}

HTMLTokenizer::HTMLTokenizer()
{
    m_tokenizer = create_tokenizer_from_utf8({});
    rust_html_tokenizer_set_input_stream_closed(m_tokenizer, false);
}

HTMLTokenizer::~HTMLTokenizer()
{
    if (m_tokenizer)
        rust_html_tokenizer_destroy(m_tokenizer);
}

HTMLTokenizer::HTMLTokenizer(StringView input, ByteString const& encoding)
{
    auto decoder = TextCodec::decoder_for(encoding);
    VERIFY(decoder.has_value());
    m_source = MUST(decoder->to_utf8(input));
    m_input_stream_closed = true;
    m_tokenizer = create_tokenizer_from_utf8(m_source.bytes_as_string_view());
}

Optional<HTMLToken> HTMLTokenizer::next_token(StopAtInsertionPoint stop_at_insertion_point)
{
    RustFfiToken ffi;
    bool stop = stop_at_insertion_point == StopAtInsertionPoint::Yes;
    bool cdata_allowed = m_parser != nullptr
        && m_parser->adjusted_current_node()
        && m_parser->adjusted_current_node()->namespace_uri() != Namespace::HTML;
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
            token.set_tag_name(MUST(FlyString::from_utf8(ffi_string_view(ffi.tag_name_ptr, ffi.tag_name_len))));

        token.set_self_closing(ffi.self_closing);
        for (size_t i = 0; i < ffi.attributes_len; ++i) {
            auto const& ffi_attribute = ffi.attributes_ptr[i];
            HTMLToken::Attribute attribute;
            if (ffi_attribute.name_id != 0)
                attribute.local_name = interned_rust_attr_name(ffi_attribute.name_id);
            else
                attribute.local_name = MUST(FlyString::from_utf8(ffi_string_view(ffi_attribute.name_ptr, ffi_attribute.name_len)));
            attribute.value = MUST(String::from_utf8(ffi_string_view(ffi_attribute.value_ptr, ffi_attribute.value_len)));
            attribute.name_start_position = { ffi_attribute.name_start_line, ffi_attribute.name_start_column };
            attribute.name_end_position = { ffi_attribute.name_end_line, ffi_attribute.name_end_column };
            attribute.value_start_position = { ffi_attribute.value_start_line, ffi_attribute.value_start_column };
            attribute.value_end_position = { ffi_attribute.value_end_line, ffi_attribute.value_end_column };
            token.add_attribute(move(attribute));
        }
        token.normalize_attributes();
        break;
    }
    case HTMLToken::Type::Comment:
        token.set_comment(MUST(String::from_utf8(ffi_string_view(ffi.comment_ptr, ffi.comment_len))));
        break;
    case HTMLToken::Type::DOCTYPE: {
        auto& doctype = token.ensure_doctype_data();
        if (!ffi.missing_name) {
            doctype.name = MUST(String::from_utf8(ffi_string_view(ffi.doctype_name_ptr, ffi.doctype_name_len)));
            doctype.missing_name = false;
        }
        if (!ffi.missing_public_id) {
            doctype.public_identifier = MUST(String::from_utf8(ffi_string_view(ffi.public_id_ptr, ffi.public_id_len)));
            doctype.missing_public_identifier = false;
        }
        if (!ffi.missing_system_id) {
            doctype.system_identifier = MUST(String::from_utf8(ffi_string_view(ffi.system_id_ptr, ffi.system_id_len)));
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

String HTMLTokenizer::unparsed_input() const
{
    uint8_t const* ptr = nullptr;
    size_t len = 0;
    rust_html_tokenizer_unparsed_input(m_tokenizer, &ptr, &len);
    return MUST(String::from_utf8(ffi_string_view(ptr, len)));
}

void HTMLTokenizer::append_to_input_stream(StringView input)
{
    if (input.is_empty())
        return;

    auto utf8_input = MUST(String::from_utf8(input));
    auto code_points = code_points_from_string(utf8_input);
    rust_html_tokenizer_append_input(m_tokenizer, code_points.data(), code_points.size());
}

void HTMLTokenizer::close_input_stream()
{
    m_input_stream_closed = true;
    rust_html_tokenizer_set_input_stream_closed(m_tokenizer, true);
}

void HTMLTokenizer::insert_input_at_insertion_point(StringView input)
{
    auto utf8_input = MUST(String::from_utf8(input));
    auto code_points = code_points_from_string(utf8_input);
    rust_html_tokenizer_insert_input(m_tokenizer, code_points.data(), code_points.size());
}

void HTMLTokenizer::insert_eof()
{
    close_input_stream();
    rust_html_tokenizer_insert_eof(m_tokenizer);
}

bool HTMLTokenizer::is_eof_inserted()
{
    return rust_html_tokenizer_is_eof_inserted(m_tokenizer);
}

void HTMLTokenizer::set_blocked(bool blocked)
{
    rust_html_tokenizer_set_blocked(m_tokenizer, blocked);
}

bool HTMLTokenizer::is_blocked() const
{
    return rust_html_tokenizer_is_blocked(m_tokenizer);
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

void HTMLTokenizer::switch_to(Badge<HTMLParser>, State new_state)
{
    dbgln_if(TOKENIZER_TRACE_DEBUG, "[{}] Parser switches tokenizer state to {}", state_name(m_state), state_name(new_state));
    switch_to(new_state);
}

void HTMLTokenizer::switch_to(State new_state)
{
    dbgln_if(TOKENIZER_TRACE_DEBUG, "[{}] Switch to {}", state_name(m_state), state_name(new_state));
    m_state = new_state;
    rust_html_tokenizer_switch_state(m_tokenizer, static_cast<uint8_t>(new_state));
}

void HTMLTokenizer::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_parser);
}

}
