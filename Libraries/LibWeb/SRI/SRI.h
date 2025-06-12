/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace Web::SRI {

// https://w3c.github.io/webappsec-subresource-integrity/#integrity-metadata
struct Metadata {
    String algorithm;    // "alg"
    String base64_value; // "val"
    String options {};   // "opt"
};

ErrorOr<String> apply_algorithm_to_bytes(StringView algorithm, ByteBuffer const& bytes);
ErrorOr<Vector<Metadata>> parse_metadata(StringView metadata);
ErrorOr<Vector<Metadata>> get_strongest_metadata_from_set(Vector<Metadata> const& set);
ErrorOr<bool> do_bytes_match_metadata_list(ByteBuffer const& bytes, StringView metadata_list);

}
