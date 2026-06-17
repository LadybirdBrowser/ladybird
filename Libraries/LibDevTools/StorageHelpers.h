/*
 * Copyright (c) 2026, Ladybird contributors
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
