/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibWebCommon/Export.h>

namespace Web {

inline constexpr size_t maximum_filename_byte_length = 255;

WEBCOMMON_API ByteString truncate_filename_to_byte_length(ByteString, size_t maximum_byte_length);
WEBCOMMON_API ByteString sanitize_suggested_download_filename(ByteString);

}
