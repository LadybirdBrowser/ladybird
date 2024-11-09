/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Forward.h>
#include <LibDiff/Forward.h>

namespace Diff {

ByteString generate_only_additions(StringView);

enum class ColorOutput {
    Yes,
    No,
};

ErrorOr<void> write_unified(Hunk const& hunk, Stream& stream, ColorOutput color_output = ColorOutput::No);
ErrorOr<void> write_unified_header(StringView old_path, StringView new_path, Stream& stream);

ErrorOr<void> write_normal(Hunk const& hunk, Stream& stream, ColorOutput color_output = ColorOutput::No);

ErrorOr<void> write_context(Hunk const& hunk, Stream& stream, ColorOutput color_output = ColorOutput::No);
ErrorOr<void> write_context_header(StringView old_path, StringView new_path, Stream& stream);
}
