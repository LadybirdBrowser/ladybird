/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibHTTP/HeaderList.h>
#include <LibWebCommon/Export.h>
#include <time.h>

namespace Web {

// The headers of a response to a request for a file: or resource: URL, which has no HTTP headers of its own.
WEBCOMMON_API NonnullRefPtr<HTTP::HeaderList> response_headers_for_file(StringView path, size_t content_length, Optional<time_t> const& modified_time);

}
