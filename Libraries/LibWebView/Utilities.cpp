/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/Utf16String.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebView/Utilities.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif

namespace WebView {

ErrorOr<Web::HTML::SelectedFile> create_selected_file(ByteString const& file_path)
{
    // FIXME: Implement the File and Directory Entries API.
    //        https://wicg.github.io/entries-api/
    if (FileSystem::is_directory(file_path))
        return Error::from_string_literal("Only files may currently be selected");

    // https://html.spec.whatwg.org/multipage/input.html#file-upload-state-(type=file):concept-input-file-path
    // Filenames must not contain path components, even in the case that a user has selected an entire directory
    // hierarchy or multiple files with the same name from different directories.
    auto name = Utf16String::from_utf8(LexicalPath::basename(file_path));

    auto file = TRY(Core::File::open(file_path, Core::File::OpenMode::Read));
    return Web::HTML::SelectedFile { move(name), IPC::File::adopt_file(move(file)) };
}

ErrorOr<IPC::File> open_local_file_for_renderer(ByteString const& path)
{
#if defined(AK_OS_WINDOWS)
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));

    // Files and directories are disk handles, while devices and pipes are not.
    if (GetFileType(to_handle(file->fd())) != FILE_TYPE_DISK)
        return Error::from_errno(EACCES);
    return IPC::File::adopt_file(move(file));
#else
    auto fd = TRY(Core::System::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    auto file = IPC::File::adopt_fd(fd);

    auto stat = TRY(Core::System::fstat(fd));
    if (!S_ISREG(stat.st_mode) && !S_ISDIR(stat.st_mode))
        return Error::from_errno(EACCES);

    // The renderer reads the file as it would any other, so it gets a blocking descriptor.
    auto flags = TRY(Core::System::fcntl(fd, F_GETFL));
    TRY(Core::System::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK));
    return file;
#endif
}

ErrorOr<JsonObject> read_json_file(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error()) {
        if (file.error().is_errno() && file.error().code() == ENOENT)
            return JsonObject {};
        return file.release_error();
    }

    auto contents = TRY(file.value()->read_until_eof());
    auto json = TRY(JsonValue::from_string(contents));

    if (!json.is_object())
        return Error::from_string_literal("Expected parsed JSON value to be an object");
    return move(json.as_object());
}

ErrorOr<void> write_json_file(ByteString const& path, JsonValue const& value)
{
    auto directory = LexicalPath { path }.parent();
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));

    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(value.serialized()));

    return {};
}

}
