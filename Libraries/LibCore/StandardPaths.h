/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Forward.h>

namespace Core {

class StandardPaths {
public:
    static ByteString home_directory();
    static ByteString desktop_directory();
    static ByteString documents_directory();
    static ByteString downloads_directory();
    static ByteString music_directory();
    static ByteString pictures_directory();
    static ByteString videos_directory();
    static ByteString tempfile_directory();
    static ByteString config_directory();
    static ByteString user_data_directory();
    static Vector<ByteString> system_data_directories();
    static ErrorOr<ByteString> runtime_directory();
};

}
