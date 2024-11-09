/*
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibDiff/Forward.h>

namespace Diff {

ErrorOr<void> apply_patch(Stream& out, Vector<StringView> const& lines, Patch const& patch, Optional<StringView> const& define = {});

}
