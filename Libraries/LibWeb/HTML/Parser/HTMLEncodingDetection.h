/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

struct MetaCharsetScanner;

namespace Web::HTML {

enum class EncodingConfidence : u8 {
    Tentative,
    Certain,
    Irrelevant,
};

struct EncodingSniffingResult {
    ByteString encoding;
    EncodingConfidence confidence;
};

Optional<StringView> extract_character_encoding_from_meta_element(ByteString const&);
GC::Ptr<DOM::Attr> prescan_get_attribute(DOM::Document&, ReadonlyBytes input, size_t& position);
Optional<ByteString> run_prescan_byte_stream_algorithm(DOM::Document&, ReadonlyBytes input);
Optional<ByteString> prescan_for_utf16_xml_declaration(ReadonlyBytes input);
Optional<ByteString> run_bom_sniff(ReadonlyBytes input);
Optional<ByteString> get_an_xml_encoding(ReadonlyBytes input);

// Extracts the rightmost DNS label from a URL's host as a TLD hint for chardetng.
// Returns an empty string if the host is absent or is an IP address. chardetng treats
// an absent/empty TLD as equivalent to ".com".
ByteString extract_tld_hint(URL::URL const&);

// Finds a document's encoding declaration by tokenizing its bytes as the HTML parser will: the prescan proposed in
// https://github.com/whatwg/html/issues/6962. It looks through the first 1024 bytes, and past them for as long as the
// document's head can still hold a declaration.
class EncodingDeclarationPrescan {
    AK_MAKE_NONCOPYABLE(EncodingDeclarationPrescan);
    AK_MAKE_NONMOVABLE(EncodingDeclarationPrescan);
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit EncodingDeclarationPrescan(bool scripting_enabled);
    ~EncodingDeclarationPrescan();

    enum class Result : u8 {
        NeedMoreInput,
        Found,
        NotFound,
    };

    // Scans the bytes that arrived since the last call.
    Result scan(ReadonlyBytes, bool input_complete);

    // The encoding the declaration names, once scan() has found one.
    StringView encoding() const { return m_encoding.value(); }

private:
    MetaCharsetScanner* m_scanner { nullptr };
    Optional<StringView> m_encoding;
};

// NB: A document that is being loaded by a navigation has no node navigable until it is activated, which can happen
//     after its parser starts. Pass the container document of the navigable that the document is being loaded into as
//     loading_container_document in that case.
EncodingSniffingResult run_encoding_sniffing_algorithm(DOM::Document&, ReadonlyBytes input,
    Optional<MimeSniff::MimeType> maybe_mime_type = {}, GC::Ptr<DOM::Document> loading_container_document = {});

// The steps of the encoding sniffing algorithm before its prescan looks for a declaration: the BOM, the transport layer,
// and the prescan's check for a UTF-16 XML declaration.
Optional<EncodingSniffingResult> sniff_encoding_before_prescan(ReadonlyBytes input, Optional<MimeSniff::MimeType> const&);

// The steps of the encoding sniffing algorithm after its prescan, for when the prescan finds no declaration.
EncodingSniffingResult sniff_encoding_after_prescan(DOM::Document&, ReadonlyBytes input, GC::Ptr<DOM::Document> loading_container_document);

}
