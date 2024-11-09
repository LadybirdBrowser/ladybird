/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/SelectedFile.h>

namespace Web::HTML {

ErrorOr<SelectedFile> SelectedFile::from_file_path(ByteString const& file_path)
{
    // https://html.spec.whatwg.org/multipage/input.html#file-upload-state-(type=file):concept-input-file-path
    // Filenames must not contain path components, even in the case that a user has selected an entire directory
    // hierarchy or multiple files with the same name from different directories.
    auto name = LexicalPath::basename(file_path);

    auto file = TRY(Core::File::open(file_path, Core::File::OpenMode::Read));
    return SelectedFile { move(name), IPC::File::adopt_file(move(file)) };
}

SelectedFile::SelectedFile(ByteString name, ByteBuffer contents)
    : m_name(move(name))
    , m_file_or_contents(move(contents))
{
}

SelectedFile::SelectedFile(ByteString name, IPC::File file)
    : m_name(move(name))
    , m_file_or_contents(move(file))
{
}

ByteBuffer SelectedFile::take_contents()
{
    VERIFY(m_file_or_contents.has<ByteBuffer>());
    return move(m_file_or_contents.get<ByteBuffer>());
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SelectedFile const& file)
{
    TRY(encoder.encode(file.name()));
    TRY(encoder.encode(file.file_or_contents()));
    return {};
}

template<>
ErrorOr<Web::HTML::SelectedFile> IPC::decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<ByteString>());
    auto file_or_contents = TRY((decoder.decode<Variant<IPC::File, ByteBuffer>>()));

    ByteBuffer contents;

    if (file_or_contents.has<IPC::File>()) {
        auto file = TRY(Core::File::adopt_fd(file_or_contents.get<IPC::File>().take_fd(), Core::File::OpenMode::Read));
        contents = TRY(file->read_until_eof());
    } else {
        contents = move(file_or_contents.get<ByteBuffer>());
    }

    return Web::HTML::SelectedFile { move(name), move(contents) };
}
