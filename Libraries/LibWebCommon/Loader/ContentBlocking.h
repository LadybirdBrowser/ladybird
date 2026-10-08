/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>

namespace Web {

WEBCOMMON_API Optional<String> content_blocking_site(URL::URL const&);
WEBCOMMON_API Optional<String> canonical_content_blocking_host(StringView);

}
