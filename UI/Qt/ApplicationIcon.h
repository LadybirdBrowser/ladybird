/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>

namespace Ladybird {

void set_profile_application_icon(StringView identifier, ByteString const& identity, bool is_temporary);

}
