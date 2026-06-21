/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibJS/Export.h>
#include <LibJS/Position.h>
#include <LibJS/SourceCode.h>

namespace JS {

struct JS_API SourceRange {
    NonnullRefPtr<SourceCode const> code;
    Position start;

    Utf16String const& filename() const { return code->filename(); }
};

}
