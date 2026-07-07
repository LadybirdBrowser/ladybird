/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>

#include <AK/Windows.h>

namespace Core {

// Maps the CreateFileW/GetFileAttributesExW failures that Windows-reachable callers
// match on into errno space.
// FIXME: Replace ad-hoc errno matching with a portable error taxonomy.
static Error open_error_from_windows_error()
{
    auto error = GetLastError();
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return Error::from_errno(ENOENT);
    case ERROR_ACCESS_DENIED:
        return Error::from_errno(EACCES);
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return Error::from_errno(EEXIST);
    default:
        return Error::from_windows_error(error);
    }
}

static i64 unix_time_from_filetime(FILETIME file_time)
{
    // Seconds between the Windows epoch (1601-01-01) and the Unix epoch (1970-01-01).
    static constexpr i64 windows_to_unix_epoch_offset_seconds = 11'644'473'600;
    auto hundreds_of_nanoseconds = static_cast<i64>((static_cast<u64>(file_time.dwHighDateTime) << 32) | file_time.dwLowDateTime);
    return hundreds_of_nanoseconds / 10'000'000 - windows_to_unix_epoch_offset_seconds;
}

ErrorOr<void> File::open_path(StringView filename, mode_t)
{
    VERIFY(m_fd == -1);

    // NOTE: Windows files have no POSIX permission bits, so the mode argument is ignored
    //       instead of being approximated.

    if (has_flag(m_mode, OpenMode::Nonblocking)) {
        dbgln("Core::File::OpenMode::Nonblocking is not implemented on Windows");
        VERIFY_NOT_REACHED();
    }

    DWORD desired_access = 0;
    if (has_flag(m_mode, OpenMode::Read))
        desired_access |= GENERIC_READ;
    if (has_flag(m_mode, OpenMode::Write)) {
        // FILE_APPEND_DATA without FILE_WRITE_DATA makes WriteFile() always append,
        // which matches POSIX O_APPEND semantics.
        if (has_flag(m_mode, OpenMode::Append))
            desired_access |= FILE_APPEND_DATA | SYNCHRONIZE;
        else
            desired_access |= GENERIC_WRITE;
    }

    // Mirror the POSIX backend's flag policy: write modes create unless DontCreate,
    // write-only implies truncation unless appending or exclusive.
    bool const may_create = has_flag(m_mode, OpenMode::Write) && !has_flag(m_mode, OpenMode::DontCreate);
    bool should_truncate = has_flag(m_mode, OpenMode::Truncate);
    if (!has_flag(m_mode, OpenMode::ReadWrite) && has_flag(m_mode, OpenMode::Write) && !has_any_flag(m_mode, OpenMode::Append | OpenMode::MustBeNew))
        should_truncate = true;

    DWORD creation_disposition;
    if (has_flag(m_mode, OpenMode::MustBeNew) && may_create)
        creation_disposition = CREATE_NEW;
    else if (may_create)
        creation_disposition = should_truncate ? CREATE_ALWAYS : OPEN_ALWAYS;
    else
        creation_disposition = should_truncate ? TRUNCATE_EXISTING : OPEN_EXISTING;

    SECURITY_ATTRIBUTES security_attributes = {};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = has_flag(m_mode, OpenMode::KeepOnExec);

    auto wide_filename = TRY(to_wide_string(filename));

    // Share as permissively as POSIX opens do. FILE_FLAG_BACKUP_SEMANTICS allows opening
    // directories, which Core::Directory and the file:// directory listing depend on.
    HANDLE handle = CreateFileW(
        wide_filename.data(),
        desired_access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &security_attributes,
        creation_disposition,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return open_error_from_windows_error();

    m_fd = to_fd(handle);
    return {};
}

ErrorOr<Bytes> File::read_some(Bytes buffer)
{
    if (!has_flag(m_mode, OpenMode::Read)) {
        // NOTE: The POSIX backend also refuses reads on write-only files without a syscall.
        return Error::from_errno(EBADF);
    }

    DWORD n_read = 0;
    if (!ReadFile(to_handle(m_fd), buffer.data(), buffer.size(), &n_read, nullptr)) {
        auto error = GetLastError();
        // Reading a pipe whose write end has closed fails with ERROR_BROKEN_PIPE once
        // drained; POSIX read() reports that (and end-of-file) as reading 0 bytes.
        if (error != ERROR_BROKEN_PIPE && error != ERROR_HANDLE_EOF)
            return Error::from_windows_error(error);
        n_read = 0;
    }
    m_last_read_was_eof = n_read == 0;
    m_file_offset += n_read;
    return buffer.trim(n_read);
}

ErrorOr<size_t> File::write_some(ReadonlyBytes buffer)
{
    if (!has_flag(m_mode, OpenMode::Write)) {
        // NOTE: Same deal as Read.
        return Error::from_errno(EBADF);
    }

    DWORD n_written = 0;
    if (!WriteFile(to_handle(m_fd), buffer.data(), buffer.size(), &n_written, nullptr))
        return Error::from_windows_error();
    m_file_offset += n_written;
    return n_written;
}

ErrorOr<size_t> File::seek(i64 offset, SeekMode mode)
{
    DWORD move_method;
    switch (mode) {
    case SeekMode::SetPosition:
        move_method = FILE_BEGIN;
        break;
    case SeekMode::FromCurrentPosition:
        move_method = FILE_CURRENT;
        break;
    case SeekMode::FromEndPosition:
        move_method = FILE_END;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    LARGE_INTEGER new_pointer = {};
    if (!SetFilePointerEx(to_handle(m_fd), { .QuadPart = offset }, &new_pointer, move_method))
        return Error::from_windows_error();
    m_file_offset = new_pointer.QuadPart;
    m_last_read_was_eof = false;
    return m_file_offset;
}

ErrorOr<void> File::truncate(size_t length)
{
    if (length > static_cast<size_t>(NumericLimits<i64>::max()))
        return Error::from_string_literal("Length is larger than the maximum supported length");

    // Truncates or extends without moving the file pointer; like POSIX ftruncate(),
    // the caller-visible position is left unchanged (it may end up past EOF).
    FILE_END_OF_FILE_INFO end_of_file_info = {};
    end_of_file_info.EndOfFile.QuadPart = static_cast<LONGLONG>(length);
    if (!SetFileInformationByHandle(to_handle(m_fd), FileEndOfFileInfo, &end_of_file_info, sizeof(end_of_file_info)))
        return Error::from_windows_error();
    return {};
}

ErrorOr<struct stat> File::stat() const
{
    return fstat(m_fd);
}

static struct stat stat_from_file_information(DWORD attributes, u64 file_size, FILETIME creation_time, FILETIME last_access_time, FILETIME last_write_time)
{
    struct stat st = {};
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        st.st_mode = S_IFDIR | 0755;
    else
        st.st_mode = S_IFREG | ((attributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
    st.st_size = static_cast<decltype(st.st_size)>(file_size);
    st.st_nlink = 1;
    st.st_atime = unix_time_from_filetime(last_access_time);
    st.st_mtime = unix_time_from_filetime(last_write_time);
    st.st_ctime = unix_time_from_filetime(creation_time);
    return st;
}

// FIXME: Unlike POSIX stat(), this doesn't follow symlinks (reparse points).
ErrorOr<struct stat> File::stat(StringView path)
{
    auto wide_path = TRY(to_wide_string(path));
    WIN32_FILE_ATTRIBUTE_DATA attribute_data = {};
    if (!GetFileAttributesExW(wide_path.data(), GetFileExInfoStandard, &attribute_data))
        return open_error_from_windows_error();

    return stat_from_file_information(
        attribute_data.dwFileAttributes,
        (static_cast<u64>(attribute_data.nFileSizeHigh) << 32) | attribute_data.nFileSizeLow,
        attribute_data.ftCreationTime,
        attribute_data.ftLastAccessTime,
        attribute_data.ftLastWriteTime);
}

ErrorOr<struct stat> File::fstat(int fd)
{
    BY_HANDLE_FILE_INFORMATION file_information = {};
    if (!GetFileInformationByHandle(to_handle(fd), &file_information))
        return Error::from_windows_error();

    auto st = stat_from_file_information(
        file_information.dwFileAttributes,
        (static_cast<u64>(file_information.nFileSizeHigh) << 32) | file_information.nFileSizeLow,
        file_information.ftCreationTime,
        file_information.ftLastAccessTime,
        file_information.ftLastWriteTime);
    st.st_nlink = static_cast<decltype(st.st_nlink)>(file_information.nNumberOfLinks);
    st.st_dev = file_information.dwVolumeSerialNumber;
    return st;
}

}
