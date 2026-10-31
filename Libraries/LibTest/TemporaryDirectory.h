/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/LexicalPath.h>
#include <AK/Noncopyable.h>
#include <AK/StringView.h>
#include <LibTest/Export.h>

namespace Test {

// A uniquely named directory under the temp directory, removed with its contents on destruction.
class TEST_API TemporaryDirectory {
    AK_MAKE_NONCOPYABLE(TemporaryDirectory);
    AK_MAKE_NONMOVABLE(TemporaryDirectory);

public:
    explicit TemporaryDirectory(StringView name);
    ~TemporaryDirectory();

    LexicalPath const& path() const { return m_path; }

private:
    LexicalPath m_path;
};

}
