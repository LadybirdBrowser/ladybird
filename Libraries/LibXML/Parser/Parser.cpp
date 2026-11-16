/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibXML/Parser/Parser.h>
#include <LibXML/RustFFI.h>

namespace XML {

static ptrdiff_t resolve_html_entity(void* context, u8 const* name, size_t name_length, u8* out, size_t capacity)
{
    auto const& options = *static_cast<Parser::Options const*>(context);
    auto resolved = options.resolve_named_html_entity(StringView { name, name_length });
    if (!resolved.has_value())
        return -1;
    auto bytes = resolved->bytes();
    if (bytes.size() > capacity)
        return -1;
    bytes.copy_to({ out, capacity });
    return static_cast<ptrdiff_t>(bytes.size());
}

// A parsed document: a flat list of rows in document order, held as three columns. See Rust/src/tape.rs for what the
// rows are. A string operand is an offset and a length into the source followed by the parser's arena, so a string
// that didn't need decoding is a view of the source itself.
class Tape {
    AK_MAKE_NONCOPYABLE(Tape);
    AK_MAKE_NONMOVABLE(Tape);

public:
    Tape(StringView source, Parser::Options const& options)
        : m_source(source)
    {
        FFI::XmlParseOptions ffi_options {
            .preserve_comments = options.preserve_comments,
            .preserve_cdata = options.preserve_cdata,
            .resolve_html_entity = options.resolve_named_html_entity ? resolve_html_entity : nullptr,
            .resolver_context = const_cast<Parser::Options*>(&options),
        };
        m_document = FFI::rust_xml_parse(reinterpret_cast<u8 const*>(source.characters_without_null_termination()), source.length(), &ffi_options);
        m_view = FFI::rust_xml_document_view(m_document);
    }

    ~Tape() { FFI::rust_xml_document_free(m_document); }

    size_t row_count() const { return m_view.row_count; }
    FFI::XmlRowKind kind(size_t row) const { return static_cast<FFI::XmlRowKind>(m_view.kinds[row] & to_underlying(FFI::XmlRowKindBits::Mask)); }
    bool is(size_t row, FFI::XmlRowKind kind) const { return row < m_view.row_count && this->kind(row) == kind; }
    bool has_flag(size_t row, u8 flag) const { return (m_view.kinds[row] & flag) != 0; }
    u32 a(size_t row) const { return m_view.a[row]; }

    // For a row whose operands are a string.
    StringView string(size_t row) const { return string(m_view.a[row], m_view.b[row]); }

    // For a row whose first operand is a name.
    u32 name_id(size_t row) const { return m_view.a[row]; }
    StringView name(size_t row) const
    {
        auto const& name = m_view.names[m_view.a[row]];
        return string(name.offset, name.length);
    }
    size_t name_count() const { return m_view.name_count; }

    ReadonlySpan<FFI::XmlError> errors() const { return { m_view.errors, m_view.error_count }; }
    bool started() const { return m_view.started; }
    Version version() const { return m_view.version_1_0 ? Version::Version10 : Version::Version11; }

    Doctype doctype(size_t row) const
    {
        static constexpr u8 ABSENT = to_underlying(FFI::XmlValueFlag::Absent);
        Doctype doctype;
        doctype.type = name(row);
        auto has_public_id = !has_flag(row + 1, ABSENT);
        auto has_system_id = !has_flag(row + 2, ABSENT);
        if (has_public_id || has_system_id) {
            ExternalID external_id;
            if (has_public_id)
                external_id.public_id = PublicID { string(row + 1) };
            if (has_system_id)
                external_id.system_id = SystemID { string(row + 2) };
            doctype.external_id = move(external_id);
        }
        return doctype;
    }

private:
    StringView string(u32 offset, u32 length) const
    {
        if (offset < m_source.length())
            return m_source.substring_view(offset, length);
        return StringView { m_view.arena + (offset - m_source.length()), length };
    }

    StringView m_source;
    FFI::XmlDocument* m_document { nullptr };
    FFI::XmlDocumentView m_view {};
};

static ParseError to_parse_error(FFI::XmlError const& error)
{
    return ParseError {
        .position = LineTrackingLexer::Position {
            .offset = error.offset,
            .line = error.line,
            .column = error.column,
        },
        .error = ByteString(StringView { error.message, error.message_length }),
    };
}

ErrorOr<void, ParseError> Parser::parse_with_listener(Listener& listener)
{
    auto source_result = listener.set_source(m_source);
    if (source_result.is_error())
        return ParseError { {}, ByteString("Failed to set source") };

    Tape tape { m_source, m_options };

    // NB: The parser has checked that every string of the tape is UTF-8, so none of them is checked again here.

    // OPTIMIZATION: A document has few distinct names and uses each of them many times, so a name becomes a fly
    // string the first time a row uses it, and every later row just takes another reference to it.
    Vector<Optional<Utf16FlyString>> names;
    names.resize(tape.name_count());
    auto name = [&](size_t row) -> Utf16FlyString const& {
        auto& slot = names[tape.name_id(row)];
        if (!slot.has_value())
            slot = Utf16FlyString::from_utf8_without_validation(tape.name(row));
        return slot.value();
    };

    // Each error says which row it comes before, so that the listener hears of it at the point in the document
    // where the parser found it.
    auto errors = tape.errors();
    size_t next_error = 0;
    m_parse_errors.clear();
    auto report_errors_before = [&](size_t row) {
        while (next_error < errors.size() && errors[next_error].row <= row) {
            auto error = to_parse_error(errors[next_error++]);
            listener.error(error);
            m_parse_errors.append(move(error));
        }
    };

    // A document that is empty, or wrong before its XML declaration is over, doesn't start.
    if (tape.started())
        listener.document_start();

    auto has_error_before = [&](size_t row) {
        return next_error < errors.size() && errors[next_error].row <= row;
    };

    Vector<ListenerAttribute> attributes;
    StringBuilder text;
    auto row_count = tape.row_count();
    for (size_t row = 0; row < row_count; ++row) {
        report_errors_before(row);
        switch (tape.kind(row)) {
        case FFI::XmlRowKind::Start: {
            auto const& element_name = name(row);
            attributes.clear_with_capacity();
            while (tape.is(row + 1, FFI::XmlRowKind::Attribute)) {
                attributes.empend(name(row + 1), Utf16String::from_utf8_without_validation(tape.string(row + 2)));
                row += 2;
            }
            listener.element_start(element_name, attributes);
            break;
        }
        case FFI::XmlRowKind::End:
            listener.element_end(name(row));
            break;
        case FFI::XmlRowKind::Text:
            // OPTIMIZATION: Text with references in it is a row for each piece. A listener that builds a tree would
            // have to grow its text node once per piece, so it is given the whole run at once. The run ends where
            // an error comes in, which the listener hears of before the text that follows it.
            if (tape.is(row + 1, FFI::XmlRowKind::Text) && !has_error_before(row + 1)) {
                text.clear();
                text.append(tape.string(row));
                while (tape.is(row + 1, FFI::XmlRowKind::Text) && !has_error_before(row + 1))
                    text.append(tape.string(++row));
                listener.text(text.string_view());
                break;
            }
            listener.text(tape.string(row));
            break;
        case FFI::XmlRowKind::Cdata:
            listener.cdata_section(tape.string(row));
            break;
        case FFI::XmlRowKind::Comment:
            listener.comment(tape.string(row));
            break;
        case FFI::XmlRowKind::ProcessingInstruction:
            listener.processing_instruction(name(row), Utf16String::from_utf8_without_validation(tape.string(row + 1)));
            row += 1;
            break;
        case FFI::XmlRowKind::Doctype:
            listener.set_doctype(tape.doctype(row));
            row += 2;
            break;
        case FFI::XmlRowKind::Attribute:
        case FFI::XmlRowKind::Value:
            // These only follow the row they belong to, which consumes them.
            VERIFY_NOT_REACHED();
        }
    }
    report_errors_before(NumericLimits<size_t>::max());

    listener.document_end();

    if (!m_parse_errors.is_empty())
        return m_parse_errors.first();
    return {};
}

ErrorOr<Document, ParseError> Parser::parse()
{
    Tape tape { m_source, m_options };

    m_parse_errors.clear();
    for (auto const& error : tape.errors())
        m_parse_errors.append(to_parse_error(error));
    if (!m_parse_errors.is_empty())
        return m_parse_errors.first();

    OwnPtr<Node> root;
    Node* current = nullptr;
    Optional<Doctype> doctype;
    HashMap<Name, ByteString> processing_instructions;

    auto append_child = [&](Variant<Node::Text, Node::Comment, Node::Element> content) -> Node* {
        auto node = adopt_own(*new Node { .offset = {}, .content = move(content), .parent = current });
        auto* node_pointer = node.ptr();
        if (current)
            current->content.get<Node::Element>().children.append(move(node));
        else
            root = move(node);
        return node_pointer;
    };

    auto row_count = tape.row_count();
    for (size_t row = 0; row < row_count; ++row) {
        switch (tape.kind(row)) {
        case FFI::XmlRowKind::Start: {
            Node::Element element { .name = tape.name(row), .attributes = {}, .children = {} };
            while (tape.is(row + 1, FFI::XmlRowKind::Attribute)) {
                element.attributes.set(tape.name(row + 1), tape.string(row + 2));
                row += 2;
            }
            current = append_child(move(element));
            break;
        }
        case FFI::XmlRowKind::End:
            current = current->parent;
            break;
        case FFI::XmlRowKind::Text: {
            if (!current)
                break;
            // Text that was written in pieces (around a reference, say) is still one node.
            auto& children = current->content.get<Node::Element>().children;
            if (!children.is_empty() && children.last()->is_text()) {
                children.last()->content.get<Node::Text>().builder.append(tape.string(row));
                break;
            }
            [[fallthrough]];
        }
        case FFI::XmlRowKind::Cdata: {
            if (!current)
                break;
            Node::Text text;
            text.builder.append(tape.string(row));
            append_child(move(text));
            break;
        }
        case FFI::XmlRowKind::Comment:
            if (current)
                append_child(Node::Comment { tape.string(row) });
            break;
        case FFI::XmlRowKind::ProcessingInstruction:
            processing_instructions.set(tape.name(row), tape.string(row + 1));
            row += 1;
            break;
        case FFI::XmlRowKind::Doctype:
            doctype = tape.doctype(row);
            row += 2;
            break;
        case FFI::XmlRowKind::Attribute:
        case FFI::XmlRowKind::Value:
            // These only follow the row they belong to, which consumes them.
            VERIFY_NOT_REACHED();
        }
    }

    if (!root)
        return ParseError { {}, ByteString("No root element") };

    return Document(root.release_nonnull(), move(doctype), move(processing_instructions), tape.version());
}

}
