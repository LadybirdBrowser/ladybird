/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/FileAPI/SerializedBlobURLEntry.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>

namespace Web::FileAPI {

WEBCOMMON_API bool check_for_same_partition_blob_url_usage(URL::Origin const& blob_url_entry_origin, URL::Origin const& environment_origin);

struct TopLevelNavigation { };
struct TopLevelSelfFetch { };
WEBCOMMON_API Optional<SerializedBlobURLEntry::Object> obtain_a_blob_object(SerializedBlobURLEntry const&, Variant<StorageAPI::StorageKey, TopLevelNavigation, TopLevelSelfFetch> environment);

}
