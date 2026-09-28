/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>

namespace Web::StorageAPI {

Optional<StorageKey> obtain_a_storage_key(HTML::Environment const&);
StorageKey obtain_a_storage_key_for_non_storage_purposes(HTML::Environment const&);

}
