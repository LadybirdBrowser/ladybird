/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibLine/Style.h>

namespace Line::VT {

ErrorOr<void> save_cursor(Stream&);
ErrorOr<void> restore_cursor(Stream&);
ErrorOr<void> clear_to_end_of_line(Stream&);
ErrorOr<void> clear_lines(size_t count_above, size_t count_below, Stream&);
ErrorOr<void> move_relative(int x, int y, Stream&);
ErrorOr<void> move_absolute(u32 x, u32 y, Stream&);
ErrorOr<void> apply_style(Style const&, Stream&, bool is_starting = true);

}
