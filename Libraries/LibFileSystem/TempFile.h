/*
 * Copyright (c) 2020-2023, the SerenityOS developers.
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/String.h>

namespace FileSystem {

class TempFile {

public:
    static ErrorOr<NonnullOwnPtr<TempFile>> create_temp_directory();
    static ErrorOr<NonnullOwnPtr<TempFile>> create_temp_file();

    ~TempFile();

    String const& path() const { return m_path; }

private:
    enum class Type {
        Directory,
        File
    };

    TempFile(Type type, String path)
        : m_type(type)
        , m_path(move(path))
    {
    }

    Type m_type;
    String m_path;
};

}
