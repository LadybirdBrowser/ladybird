/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Origin.h>
#include <LibWebCommon/Export.h>

namespace Web::FileAPI {

WEBCOMMON_API bool check_for_same_partition_blob_url_usage(URL::Origin const& blob_url_entry_origin, URL::Origin const& environment_origin);

}
