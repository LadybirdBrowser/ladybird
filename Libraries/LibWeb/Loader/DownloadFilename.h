/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibWeb/Export.h>

namespace Web {

WEB_API ByteString sanitize_suggested_download_filename(ByteString);

}
