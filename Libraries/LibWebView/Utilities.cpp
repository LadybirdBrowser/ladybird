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

#if defined(AK_OS_LINUX)
#    include <linux/magic.h>
#    include <sys/vfs.h>
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

#if !defined(AK_OS_WINDOWS)
// The kernel's interface trees hold no file a page could have: What a pseudo-file there reads depends on the process
// that opens it, which is the UI process here, and a device node or a descriptor link reaches whatever is behind it.
static bool is_in_kernel_interface_tree(StringView canonical_path)
{
    for (auto tree : { "/proc"sv, "/sys"sv, "/dev"sv }) {
        if (canonical_path == tree || (canonical_path.starts_with(tree) && canonical_path[tree.length()] == '/'))
            return true;
    }
    return false;
}

#    if defined(AK_OS_LINUX)
// A process information or sysfs file system can be mounted anywhere, so the trees' paths alone don't rule them out.
static ErrorOr<bool> is_on_kernel_interface_file_system(int fd)
{
    struct statfs file_system;
    if (fstatfs(fd, &file_system) < 0)
        return Error::from_syscall("fstatfs"sv, errno);
    return file_system.f_type == PROC_SUPER_MAGIC || file_system.f_type == SYSFS_MAGIC;
}
#    endif

// Opens a canonical path one component at a time, refusing a symbolic link at any of them: The path had none when it
// was resolved, so one there now was put in place since, and could lead anywhere.
static ErrorOr<int> open_without_following_links(StringView canonical_path, int options)
{
    // Each directory on the way only needs to be searched, as path resolution would.
#    if defined(O_PATH)
    static constexpr int directory_options = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
#    elif defined(O_SEARCH)
    static constexpr int directory_options = O_SEARCH | O_NOFOLLOW | O_CLOEXEC;
#    else
    static constexpr int directory_options = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
#    endif

    LexicalPath lexical_path { canonical_path };
    auto const& components = lexical_path.parts_view();
    auto fd = TRY(Core::System::open("/"sv, components.is_empty() ? options : directory_options));

    for (size_t i = 0; i < components.size(); ++i) {
        ByteString name = components[i];
        auto next_fd = ::openat(fd, name.characters(), i + 1 == components.size() ? options | O_NOFOLLOW : directory_options);
        auto error = errno;
        ::close(fd);
        if (next_fd < 0)
            return Error::from_syscall("openat"sv, error);
        fd = next_fd;
    }
    return fd;
}
#endif

ErrorOr<IPC::File> open_local_file_for_renderer(ByteString const& path)
{
#if defined(AK_OS_WINDOWS)
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));

    // Files and directories are disk handles, while devices and pipes are not.
    if (GetFileType(to_handle(file->fd())) != FILE_TYPE_DISK)
        return Error::from_errno(EACCES);
    return IPC::File::adopt_file(move(file));
#else
    // Decide on the file the path names once every symbolic link in it is resolved, so that no link leads elsewhere.
    auto canonical_path = TRY(FileSystem::real_path(path));
    if (is_in_kernel_interface_tree(canonical_path))
        return Error::from_errno(EACCES);

    auto fd = TRY(open_without_following_links(canonical_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    auto file = IPC::File::adopt_fd(fd);

    auto stat = TRY(Core::System::fstat(fd));
    if (!S_ISREG(stat.st_mode) && !S_ISDIR(stat.st_mode))
        return Error::from_errno(EACCES);

#    if defined(AK_OS_LINUX)
    if (TRY(is_on_kernel_interface_file_system(fd)))
        return Error::from_errno(EACCES);
#    endif

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
