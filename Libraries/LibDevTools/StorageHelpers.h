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

namespace DevTools {

Optional<String> storage_host_for_url(String const&);
Optional<String> storage_host_name(String const&);

JsonObject to_storage_operation_result(Optional<String> const& error_string);
JsonObject to_storage_operation_result(ErrorOr<void> const& result);

}
