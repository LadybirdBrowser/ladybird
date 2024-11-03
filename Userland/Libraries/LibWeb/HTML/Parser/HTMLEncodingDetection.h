/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

bool prescan_should_abort(ByteBuffer const& input, size_t const& position);
bool prescan_is_whitespace_or_slash(u8 const& byte);
bool prescan_skip_whitespace_and_slashes(ByteBuffer const& input, size_t& position);
Optional<StringView> extract_character_encoding_from_meta_element(ByteString const&);
JS::GCPtr<DOM::Attr> prescan_get_attribute(DOM::Document&, ByteBuffer const& input, size_t& position);
Optional<ByteString> run_prescan_byte_stream_algorithm(DOM::Document&, ByteBuffer const& input);
Optional<ByteString> run_bom_sniff(ByteBuffer const& input);
ByteString run_encoding_sniffing_algorithm(DOM::Document&, ByteBuffer const& input, Optional<MimeSniff::MimeType> maybe_mime_type = {});

}
