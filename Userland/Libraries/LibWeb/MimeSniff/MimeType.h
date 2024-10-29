/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>

namespace Web::MimeSniff {

bool is_javascript_mime_type_essence_match(StringView);

// https://mimesniff.spec.whatwg.org/#javascript-mime-type
// A JavaScript MIME type is any MIME type whose essence is one of the following:
static constexpr Array s_javascript_mime_type_essence_strings = {
    "application/ecmascript"sv,
    "application/javascript"sv,
    "application/x-ecmascript"sv,
    "application/x-javascript"sv,
    "text/ecmascript"sv,
    "text/javascript"sv,
    "text/javascript1.0"sv,
    "text/javascript1.1"sv,
    "text/javascript1.2"sv,
    "text/javascript1.3"sv,
    "text/javascript1.4"sv,
    "text/javascript1.5"sv,
    "text/jscript"sv,
    "text/livescript"sv,
    "text/x-ecmascript"sv,
    "text/x-javascript"sv
};

// https://mimesniff.spec.whatwg.org/#mime-type
class MimeType {
public:
    [[nodiscard]] static MimeType create(String type, String subtype);
    [[nodiscard]] static Optional<MimeType> parse(StringView);

    MimeType(MimeType const&);
    MimeType& operator=(MimeType const&);

    MimeType(MimeType&&);
    MimeType& operator=(MimeType&&);

    ~MimeType();

    String const& type() const { return m_type; }
    String const& subtype() const { return m_subtype; }
    OrderedHashMap<String, String> const& parameters() const { return m_parameters; }

    bool is_image() const;
    bool is_audio_or_video() const;
    bool is_font() const;
    bool is_zip_based() const;
    bool is_archive() const;
    bool is_xml() const;
    bool is_html() const;
    bool is_scriptable() const;
    bool is_javascript() const;
    bool is_json() const;

    void set_parameter(String name, String value);

    String const& essence() const;
    [[nodiscard]] String serialized() const;

private:
    MimeType(String type, String subtype);

    // https://mimesniff.spec.whatwg.org/#type
    // A MIME type’s type is a non-empty ASCII string.
    String m_type;

    // https://mimesniff.spec.whatwg.org/#subtype
    // A MIME type’s subtype is a non-empty ASCII string.
    String m_subtype;

    // https://mimesniff.spec.whatwg.org/#parameters
    // A MIME type’s parameters is an ordered map whose keys are ASCII strings and values are strings limited to HTTP quoted-string token code points. It is initially empty.
    OrderedHashMap<String, String> m_parameters;

    // Non-standard, but computed once upfront.
    String m_cached_essence;
};

String minimise_a_supported_mime_type(MimeType const&);

}
