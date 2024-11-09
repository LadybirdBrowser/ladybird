/*
 * Copyright (c) 2021, Mustafa Quraish <mustafa@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Hunks.h"
#include <AK/Error.h>

namespace Diff {

ErrorOr<Vector<Hunk>> from_text(StringView old_text, StringView new_text, size_t context = 0);

}
