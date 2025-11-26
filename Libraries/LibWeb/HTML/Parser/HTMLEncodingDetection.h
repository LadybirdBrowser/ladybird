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
#include <LibWeb/Forward.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::HTML {

Optional<StringView> extract_character_encoding_from_meta_element(ByteString const&);
GC::Ptr<DOM::Attr> prescan_get_attribute(DOM::Document&, ReadonlyBytes input, size_t& position);
Optional<ByteString> run_prescan_byte_stream_algorithm(DOM::Document&, ReadonlyBytes input);
Optional<ByteString> run_bom_sniff(ReadonlyBytes input);
ByteString run_encoding_sniffing_algorithm(DOM::Document&, ReadonlyBytes input, Optional<MimeSniff::MimeType> maybe_mime_type = {});

}
