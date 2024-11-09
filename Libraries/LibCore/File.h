/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/BufferedStream.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Stream.h>
#include <LibCore/Forward.h>
#include <LibIPC/Forward.h>

namespace Core {

class File final : public SeekableStream {
    AK_MAKE_NONCOPYABLE(File);

public:
    enum class OpenMode : unsigned {
        NotOpen = 0,
        Read = 1,
        Write = 2,
        ReadWrite = 3,
        Append = 4,
        Truncate = 8,
        MustBeNew = 16,
        KeepOnExec = 32,
        Nonblocking = 64,
        DontCreate = 128,
    };

    enum class ShouldCloseFileDescriptor {
        Yes,
        No,
    };

    static ErrorOr<NonnullOwnPtr<File>> open(StringView filename, OpenMode, mode_t = 0644);
    static ErrorOr<NonnullOwnPtr<File>> adopt_fd(int fd, OpenMode, ShouldCloseFileDescriptor = ShouldCloseFileDescriptor::Yes);

    static ErrorOr<NonnullOwnPtr<File>> standard_input();
    static ErrorOr<NonnullOwnPtr<File>> standard_output();
    static ErrorOr<NonnullOwnPtr<File>> standard_error();
    static ErrorOr<NonnullOwnPtr<File>> open_file_or_standard_stream(StringView filename, OpenMode mode);

    File(File&& other) { operator=(move(other)); }

    File& operator=(File&& other)
    {
        if (&other == this)
            return *this;

        m_mode = exchange(other.m_mode, OpenMode::NotOpen);
        m_fd = exchange(other.m_fd, -1);
        m_last_read_was_eof = exchange(other.m_last_read_was_eof, false);
        return *this;
    }

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<ByteBuffer> read_until_eof(size_t block_size = 4096) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;
    virtual ErrorOr<size_t> seek(i64 offset, SeekMode) override;
    virtual ErrorOr<size_t> tell() const override;
    virtual ErrorOr<void> truncate(size_t length) override;

    // Sets the blocking mode of the file. If blocking mode is disabled, reads
    // will fail with EAGAIN when there's no data available to read, and writes
    // will fail with EAGAIN when the data cannot be written without blocking
    // (due to the send buffer being full, for example).
    // See also Socket::set_blocking.
    ErrorOr<void> set_blocking(bool enabled);

    template<OneOf<::IPC::File, ::Core::MappedFile> VIP>
    int leak_fd(Badge<VIP>)
    {
        m_should_close_file_descriptor = ShouldCloseFileDescriptor::No;
        return m_fd;
    }

    int fd() const
    {
        return m_fd;
    }

    virtual ~File() override
    {
        if (m_should_close_file_descriptor == ShouldCloseFileDescriptor::Yes)
            close();
    }

    static int open_mode_to_options(OpenMode mode);

private:
    File(OpenMode mode, ShouldCloseFileDescriptor should_close = ShouldCloseFileDescriptor::Yes)
        : m_mode(mode)
        , m_should_close_file_descriptor(should_close)
    {
    }

    ErrorOr<void> open_path(StringView filename, mode_t);

    OpenMode m_mode { OpenMode::NotOpen };
    int m_fd { -1 };
    bool m_last_read_was_eof { false };
    ShouldCloseFileDescriptor m_should_close_file_descriptor { ShouldCloseFileDescriptor::Yes };

    size_t m_file_offset { 0 };
};

AK_ENUM_BITWISE_OPERATORS(File::OpenMode)

using InputBufferedFile = InputBufferedSeekable<File>;
using OutputBufferedFile = OutputBufferedSeekable<File>;

}
