/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibDevTools/Forward.h>
#include <LibURL/Forward.h>

namespace DevTools {

DEVTOOLS_API Optional<String> storage_host_for_url(String const&);
DEVTOOLS_API Optional<String> storage_host_for_url(URL::URL const& url);
DEVTOOLS_API Optional<String> storage_host_name(String const&);
// The storage key of the bottle a storage host's pages keep their storage in, or none when pages of that host have
// no storage (an "about:" or "javascript:" host, e.g.). A host is a URL as far as its origin goes, and the key is
// the origin's, so every "file:" host names the one "file:" bottle.
DEVTOOLS_API Optional<String> storage_key_for_host(String const&);

enum class StorageFieldType : u8 {
    Immutable,
    Mutable,
    Hidden,
    Private,
};
JsonObject define_storage_field(StringView name, StorageFieldType);

JsonObject to_storage_operation_result(Optional<String> const& error_string);
JsonObject to_storage_operation_result(ErrorOr<void> const& result);

}
