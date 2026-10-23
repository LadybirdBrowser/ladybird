/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <AK/Utf8View.h>
#include <LibWebCommon/MimeSniff/MimeType.h>
#include <LibWebCommon/RustFFI.h>

namespace Web::MimeSniff {

static FFI::FfiStringInput ffi_string_input(StringView string)
{
    return { reinterpret_cast<u8 const*>(string.characters_without_null_termination()), nullptr, string.length() };
}

static FFI::FfiStringInput ffi_string_input(Utf16View string)
{
    return {
        string.has_ascii_storage() ? reinterpret_cast<u8 const*>(string.ascii_span().data()) : nullptr,
        string.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(string.utf16_span().data()),
        string.length_in_code_units(),
    };
}

static String string_from_ffi(ReadonlyBytes bytes)
{
    return String::from_utf8_without_validation(bytes);
}

static void set_string_from_ffi(void* ctx, ReadonlyBytes string)
{
    *static_cast<String*>(ctx) = string_from_ffi(string);
}

bool is_javascript_mime_type_essence_match(Utf16View string)
{
    return FFI::mime_sniff_is_javascript_mime_type_essence_match(ffi_string_input(string));
}

static bool contains_only_http_quoted_string_token_code_points(StringView string)
{
    // https://mimesniff.spec.whatwg.org/#http-quoted-string-token-code-point
    // An HTTP quoted-string token code point is U+0009 TAB, a code point in the range U+0020 SPACE to U+007E (~), inclusive,
    // or a code point in the range U+0080 through U+00FF (ÿ), inclusive.
    for (auto ch : Utf8View(string)) {
        if (!(ch == '\t' || (ch >= 0x20 && ch <= 0x7E) || (ch >= 0x80 && ch <= 0xFF)))
            return false;
    }
    return true;
}

static bool contains_only_http_token_code_points(StringView string)
{
    // https://mimesniff.spec.whatwg.org/#http-token-code-point
    // An HTTP token code point is U+0021 (!), U+0023 (#), U+0024 ($), U+0025 (%), U+0026 (&), U+0027 ('), U+002A (*),
    // U+002B (+), U+002D (-), U+002E (.), U+005E (^), U+005F (_), U+0060 (`), U+007C (|), U+007E (~), or an ASCII alphanumeric.
    constexpr auto is_certain_non_ascii_alphanumeric = is_any_of("!#$%&'*+-.^_`|~"sv);
    for (char ch : string) {
        if (!is_certain_non_ascii_alphanumeric(ch) && !is_ascii_alphanumeric(ch))
            return false;
    }
    return true;
}

MimeType::MimeType(String type, String subtype)
    : m_type(move(type))
    , m_subtype(move(subtype))
{
    // NOTE: type and subtype are expected to be non-empty and contain only
    // http token code points in the MIME type parsing algorithm. That's
    // why we are performing the same checks here.
    VERIFY(!m_type.is_empty() && contains_only_http_token_code_points(m_type));
    VERIFY(!m_subtype.is_empty() && contains_only_http_token_code_points(m_subtype));
}

MimeType::MimeType(MimeType const& other) = default;
MimeType& MimeType::operator=(MimeType const& other) = default;

MimeType::MimeType(MimeType&& other) = default;
MimeType& MimeType::operator=(MimeType&& other) = default;

MimeType::~MimeType() = default;

MimeType MimeType::create(String type, String subtype)
{
    auto mime_type = MimeType { move(type), move(subtype) };
    mime_type.m_cached_essence = MUST(String::formatted("{}/{}", mime_type.m_type, mime_type.m_subtype));
    return mime_type;
}

MimeType MimeType::from_ffi(FFI::FfiMimeType const& ffi_mime_type)
{
    auto mime_type = create(string_from_ffi(ffi_mime_type.type_), string_from_ffi(ffi_mime_type.subtype));
    for (auto const& parameter : ffi_mime_type.parameters)
        mime_type.m_parameters.set(string_from_ffi(parameter.name), string_from_ffi(parameter.value));
    return mime_type;
}

void MimeType::lend_to_ffi(Function<void(FFI::FfiMimeType const&)> const& callback) const
{
    Vector<FFI::FfiMimeTypeParameter> parameters;
    parameters.ensure_capacity(m_parameters.size());
    for (auto const& [name, value] : m_parameters)
        parameters.unchecked_append({ name.bytes(), value.bytes() });
    callback({ m_type.bytes(), m_subtype.bytes(), parameters.span() });
}

static void set_mime_type_from_ffi(void* ctx, FFI::FfiMimeType const* ffi_mime_type)
{
    *static_cast<Optional<MimeType>*>(ctx) = MimeType::from_ffi(*ffi_mime_type);
}

static Optional<MimeType> parse_mime_type(FFI::FfiStringInput input)
{
    Optional<MimeType> mime_type;
    FFI::mime_sniff_parse_mime_type(input, &mime_type, set_mime_type_from_ffi);
    return mime_type;
}

// https://mimesniff.spec.whatwg.org/#parse-a-mime-type
Optional<MimeType> MimeType::parse(StringView string)
{
    return parse_mime_type(ffi_string_input(string));
}

// https://mimesniff.spec.whatwg.org/#parse-a-mime-type
Optional<MimeType> MimeType::parse(Utf16View string)
{
    return parse_mime_type(ffi_string_input(string));
}

// https://mimesniff.spec.whatwg.org/#mime-type-essence
String const& MimeType::essence() const
{
    // The essence of a MIME type mimeType is mimeType’s type, followed by U+002F (/), followed by mimeType’s subtype.
    return m_cached_essence;
}

// https://mimesniff.spec.whatwg.org/#serialize-a-mime-type
String MimeType::serialized() const
{
    String serialization;
    lend_to_ffi([&](FFI::FfiMimeType const& ffi_mime_type) {
        FFI::mime_sniff_serialize_mime_type(&ffi_mime_type, &serialization, set_string_from_ffi);
    });
    return serialization;
}

// https://mimesniff.spec.whatwg.org/#serialize-a-mime-type
Utf16String MimeType::serialized_as_utf16() const
{
    return Utf16String::from_utf8_without_validation(serialized());
}

void MimeType::set_parameter(String name, String value)
{
    // https://mimesniff.spec.whatwg.org/#parameters
    // A MIME type’s parameters is an ordered map whose keys are ASCII strings and values are strings limited to HTTP quoted-string token code points.
    VERIFY(contains_only_http_quoted_string_token_code_points(name));
    VERIFY(contains_only_http_quoted_string_token_code_points(value));
    m_parameters.set(move(name), move(value));
}

// https://mimesniff.spec.whatwg.org/#image-mime-type
bool MimeType::is_image() const
{
    return FFI::mime_sniff_mime_type_is_in_group(FFI::MimeTypeGroup::Image, m_type.bytes(), m_subtype.bytes());
}

// https://mimesniff.spec.whatwg.org/#audio-or-video-mime-type
bool MimeType::is_audio_or_video() const
{
    return FFI::mime_sniff_mime_type_is_in_group(FFI::MimeTypeGroup::AudioOrVideo, m_type.bytes(), m_subtype.bytes());
}

// https://mimesniff.spec.whatwg.org/#font-mime-type
bool MimeType::is_font() const
{
    return FFI::mime_sniff_mime_type_is_in_group(FFI::MimeTypeGroup::Font, m_type.bytes(), m_subtype.bytes());
}

// https://mimesniff.spec.whatwg.org/#xml-mime-type
bool MimeType::is_xml() const
{
    return FFI::mime_sniff_mime_type_is_in_group(FFI::MimeTypeGroup::Xml, m_type.bytes(), m_subtype.bytes());
}

// https://mimesniff.spec.whatwg.org/#html-mime-type
bool MimeType::is_html() const
{
    return FFI::mime_sniff_mime_type_is_in_group(FFI::MimeTypeGroup::Html, m_type.bytes(), m_subtype.bytes());
}

// https://mimesniff.spec.whatwg.org/#javascript-mime-type
bool MimeType::is_javascript() const
{
    return FFI::mime_sniff_mime_type_is_in_group(FFI::MimeTypeGroup::JavaScript, m_type.bytes(), m_subtype.bytes());
}

// https://mimesniff.spec.whatwg.org/#json-mime-type
bool MimeType::is_json() const
{
    return FFI::mime_sniff_mime_type_is_in_group(FFI::MimeTypeGroup::Json, m_type.bytes(), m_subtype.bytes());
}

// https://mimesniff.spec.whatwg.org/#minimize-a-supported-mime-type
String minimise_a_supported_mime_type(MimeType const& mime_type)
{
    String minimized;
    FFI::mime_sniff_minimize_a_supported_mime_type(mime_type.type().bytes(), mime_type.subtype().bytes(), &minimized, set_string_from_ffi);
    return minimized;
}

}
