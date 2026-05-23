/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::HTML {

Optional<StringView> extract_character_encoding_from_meta_element(ByteString const&);
GC::Ptr<DOM::Attr> prescan_get_attribute(DOM::Document&, ReadonlyBytes input, size_t& position);
Optional<ByteString> run_prescan_byte_stream_algorithm(DOM::Document&, ReadonlyBytes input);
Optional<ByteString> run_bom_sniff(ReadonlyBytes input);

// Extracts the rightmost DNS label from a URL's host as a TLD hint for chardetng.
// Returns an empty string if the host is absent or is an IP address. chardetng treats
// an absent/empty TLD as equivalent to ".com".
ByteString extract_tld_hint(URL::URL const&);

ByteString run_encoding_sniffing_algorithm(DOM::Document&, ReadonlyBytes input,
    Optional<MimeSniff::MimeType> maybe_mime_type = {});

}
