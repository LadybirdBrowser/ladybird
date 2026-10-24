/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <LibTest/TestCase.h>

template<typename T>
static ErrorOr<T> decode_bytes(ReadonlyBytes bytes)
{
    FixedMemoryStream stream { bytes };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return decoder.decode<T>();
}

TEST_CASE(decode_bool)
{
    EXPECT_EQ(MUST(decode_bytes<bool>(Array<u8, 1> { 0 })), false);
    EXPECT_EQ(MUST(decode_bytes<bool>(Array<u8, 1> { 1 })), true);
    EXPECT(decode_bytes<bool>(Array<u8, 1> { 2 }).is_error());
    EXPECT(decode_bytes<bool>(Array<u8, 1> { 0xff }).is_error());
}

TEST_CASE(decode_vector_of_bool)
{
    auto vector_bytes = [](Array<u8, 3> values) {
        Vector<u8> bytes;
        u32 size = values.size();
        bytes.append(reinterpret_cast<u8 const*>(&size), sizeof(size));
        bytes.append(values.data(), values.size());
        return bytes;
    };

    EXPECT_EQ(MUST(decode_bytes<Vector<bool>>(vector_bytes({ 1, 0, 1 }))), (Vector<bool> { true, false, true }));
    EXPECT(decode_bytes<Vector<bool>>(vector_bytes({ 1, 2, 1 })).is_error());
}

static Vector<u8> bytes_with_size_prefix(u32 size, ReadonlyBytes contents)
{
    Vector<u8> bytes;
    bytes.append(reinterpret_cast<u8 const*>(&size), sizeof(size));
    bytes.append(contents.data(), contents.size());
    return bytes;
}

TEST_CASE(decode_size_refuses_a_size_larger_than_the_rest_of_the_message)
{
    auto contents = "abcd"sv.bytes();

    EXPECT_EQ(MUST(decode_bytes<ByteString>(bytes_with_size_prefix(4, contents))), "abcd"sv);
    EXPECT(decode_bytes<ByteString>(bytes_with_size_prefix(5, contents)).is_error());

    EXPECT(decode_bytes<Vector<String>>(bytes_with_size_prefix(5, contents)).is_error());
    EXPECT(decode_bytes<Vector<String>>(bytes_with_size_prefix(NumericLimits<u32>::max(), contents)).is_error());
}

#if !defined(AK_OS_WINDOWS)
TEST_CASE(decode_size_counts_elements_that_are_only_attachments)
{
    auto decode_files = [](u32 claimed_size, size_t attachment_count) {
        auto bytes = bytes_with_size_prefix(claimed_size, {});
        FixedMemoryStream stream { bytes.span() };
        Queue<IPC::Attachment> attachments;
        for (size_t i = 0; i < attachment_count; ++i)
            attachments.enqueue(IPC::Attachment::from_fd(MUST(Core::System::open("/dev/null"sv, O_RDONLY))));
        IPC::Decoder decoder { stream, attachments };
        return decoder.decode<Vector<IPC::File>>();
    };

    EXPECT_EQ(MUST(decode_files(2, 2)).size(), 2u);
    EXPECT(decode_files(3, 2).is_error());
}
#endif

TEST_CASE(decode_byte_buffer_larger_than_64_mib)
{
    static constexpr size_t size = 65 * MiB;
    auto contents = MUST(ByteBuffer::create_uninitialized(size));
    contents.bytes().fill('A');
    contents[size - 1] = 'B';

    auto buffer = MUST(decode_bytes<ByteBuffer>(bytes_with_size_prefix(size, contents)));
    EXPECT_EQ(buffer.size(), size);
    EXPECT_EQ(buffer[0], static_cast<u8>('A'));
    EXPECT_EQ(buffer[size - 1], static_cast<u8>('B'));
}

#if !defined(AK_OS_WINDOWS)
TEST_CASE(decode_anonymous_buffer_refuses_a_size_larger_than_its_backing)
{
    auto decode_with_claimed_size = [](u64 claimed_size) {
        auto fd = MUST(Core::System::anon_create(4096, O_CLOEXEC, Core::System::AllowSealing::Yes));

        Vector<u8> bytes;
        bytes.append(1);
        bytes.append(reinterpret_cast<u8 const*>(&claimed_size), sizeof(claimed_size));

        FixedMemoryStream stream { bytes.span() };
        Queue<IPC::Attachment> attachments;
        attachments.enqueue(IPC::Attachment::from_fd(fd));
        IPC::Decoder decoder { stream, attachments };
        return decoder.decode<Core::AnonymousBuffer>();
    };

    auto buffer = MUST(decode_with_claimed_size(4096));
    EXPECT_EQ(buffer.size(), 4096u);

    EXPECT(decode_with_claimed_size(1 * MiB).is_error());
}
#endif

#if defined(AK_OS_LINUX)
TEST_CASE(decode_anonymous_buffer_refuses_a_backing_that_cannot_be_sealed)
{
    // Without a seal, the sender could shrink the backing after the size check passed.
    auto fd = MUST(Core::System::anon_create(4096, O_CLOEXEC, Core::System::AllowSealing::No));
    u64 claimed_size = 4096;

    Vector<u8> bytes;
    bytes.append(1);
    bytes.append(reinterpret_cast<u8 const*>(&claimed_size), sizeof(claimed_size));

    FixedMemoryStream stream { bytes.span() };
    Queue<IPC::Attachment> attachments;
    attachments.enqueue(IPC::Attachment::from_fd(fd));
    IPC::Decoder decoder { stream, attachments };
    EXPECT(decoder.decode<Core::AnonymousBuffer>().is_error());
}
#endif

TEST_CASE(sizes_are_bounded_before_contents_and_only_within_the_value)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode("123456789"sv));
    MUST(encoder.encode(Optional<String> {}));
    MUST(encoder.encode("123456789"sv));
    auto bytes = buffer.take_data();

    {
        FixedMemoryStream stream { bytes.span() };
        Queue<IPC::Attachment> attachments;
        IPC::Decoder decoder { stream, attachments };
        EXPECT(decoder.decode_with_max_size<String>(8).is_error());
    }

    FixedMemoryStream stream { bytes.span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    EXPECT_EQ(MUST(decoder.decode_with_max_size<String>(9)), "123456789"sv);
    EXPECT(!MUST(decoder.decode_with_max_size<Optional<String>>(4)).has_value());
    EXPECT_EQ(MUST(decoder.decode<String>()), "123456789"sv);
}
