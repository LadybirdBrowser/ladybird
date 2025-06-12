/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Variant.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>

namespace Web::HTML {

enum class AllowMultipleFiles {
    No,
    Yes,
};

class SelectedFile {
public:
    static ErrorOr<SelectedFile> from_file_path(ByteString const& file_path);

    SelectedFile(ByteString name, ByteBuffer contents);
    SelectedFile(ByteString name, IPC::File file);

    ByteString const& name() const { return m_name; }
    auto const& file_or_contents() const { return m_file_or_contents; }
    ByteBuffer take_contents();

private:
    ByteString m_name;
    Variant<IPC::File, ByteBuffer> m_file_or_contents;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::SelectedFile const&);

template<>
ErrorOr<Web::HTML::SelectedFile> decode(Decoder&);

}
