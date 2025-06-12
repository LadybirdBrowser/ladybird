/*
 * Copyright (c) 2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/File.h>

namespace IPC {

template<>
ErrorOr<File> decode(Decoder& decoder)
{
    auto file = decoder.files().dequeue();
    TRY(Core::System::set_close_on_exec(file.fd(), true));
    return file;
}

}
