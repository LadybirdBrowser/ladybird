/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RenderNodes/OhNoesRenderNode.h>

#include <AK/Endian.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Debug.h>

#ifndef NDEBUG
#    include <LibCore/System.h>
#    include <LibWeb/WebAudio/Engine/Mixing.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace Web::WebAudio::Render {

#ifndef NDEBUG
struct OhNoesRenderNode::WavWriter {
    static constexpr size_t header_size = 44;

    int m_fd { -1 };
    String m_path;
    u32 m_sample_rate { 0 };
    u16 m_channel_count { 0 };
    u64 m_data_bytes_written { 0 };

    bool is_open() const { return m_fd >= 0; }

    void close_and_finalize()
    {
        if (!is_open())
            return;

        u32 data_size = 0xFFFF'FFFFu;
        if (m_data_bytes_written <= 0xFFFF'FFFFu)
            data_size = static_cast<u32>(m_data_bytes_written);
        u32 riff_size = 36u + data_size;

        // Patch RIFF chunk size.
        {
            auto seek_or_error = Core::System::lseek(m_fd, 4, SEEK_SET);
            if (!seek_or_error.is_error()) {
                u32 le = AK::convert_between_host_and_little_endian(riff_size);
                auto write_or_error = Core::System::write(m_fd, ReadonlyBytes { reinterpret_cast<u8 const*>(&le), sizeof(le) });
                (void)write_or_error;
            }
        }

        // Patch data chunk size.
        {
            auto seek_or_error = Core::System::lseek(m_fd, 40, SEEK_SET);
            if (!seek_or_error.is_error()) {
                u32 le = AK::convert_between_host_and_little_endian(data_size);
                auto write_or_error = Core::System::write(m_fd, ReadonlyBytes { reinterpret_cast<u8 const*>(&le), sizeof(le) });
                (void)write_or_error;
            }
        }

        (void)Core::System::close(m_fd);
        m_fd = -1;
        m_path = {};
        m_sample_rate = 0;
        m_channel_count = 0;
        m_data_bytes_written = 0;
    }

    static bool header_matches(ReadonlyBytes header, u32 sample_rate, u16 channel_count)
    {
        if (header.size() != header_size)
            return false;

        auto read_u16 = [&](size_t offset) {
            u16 value = 0;
            __builtin_memcpy(&value, header.data() + offset, sizeof(value));
            return AK::convert_between_host_and_little_endian(value);
        };
        auto read_u32 = [&](size_t offset) {
            u32 value = 0;
            __builtin_memcpy(&value, header.data() + offset, sizeof(value));
            return AK::convert_between_host_and_little_endian(value);
        };

        if (__builtin_memcmp(header.data() + 0, "RIFF", 4) != 0)
            return false;
        if (__builtin_memcmp(header.data() + 8, "WAVE", 4) != 0)
            return false;
        if (__builtin_memcmp(header.data() + 12, "fmt ", 4) != 0)
            return false;
        if (__builtin_memcmp(header.data() + 36, "data", 4) != 0)
            return false;

        if (read_u32(16) != 16)
            return false;
        if (read_u16(20) != 3)
            return false;
        if (read_u16(22) != channel_count)
            return false;
        if (read_u32(24) != sample_rate)
            return false;
        if (read_u16(34) != 32)
            return false;

        return true;
    }

    ErrorOr<void> open_next_available(String const& base_path, u32 sample_rate, u16 channel_count)
    {
        close_and_finalize();

        // Find the smallest N such that base_path.N.wav does not exist.
        // If base_path.N.wav exists but contains only a placeholder header, append to it.
        for (u64 suffix = 0; suffix < 100; ++suffix) {
            String candidate = MUST(String::formatted("{}.{}.wav", base_path, suffix));

            auto stat_or_error = Core::System::stat(candidate);
            if (!stat_or_error.is_error()) {
                struct stat const st = stat_or_error.release_value();

                if (st.st_size == static_cast<off_t>(header_size)) {
                    auto fd_or_error = Core::System::open(candidate, O_RDWR, 0644);
                    if (fd_or_error.is_error())
                        return fd_or_error.release_error();

                    int fd = fd_or_error.release_value();
                    auto seek_or_error = Core::System::lseek(fd, 0, SEEK_SET);
                    if (seek_or_error.is_error()) {
                        (void)Core::System::close(fd);
                        return seek_or_error.release_error();
                    }

                    u8 header[header_size];
                    Bytes header_bytes { header, sizeof(header) };
                    auto read_or_error = Core::System::read(fd, header_bytes);
                    if (read_or_error.is_error() || read_or_error.value() != header_size) {
                        (void)Core::System::close(fd);
                        continue;
                    }

                    if (!header_matches(ReadonlyBytes { header, sizeof(header) }, sample_rate, channel_count)) {
                        (void)Core::System::close(fd);
                        continue;
                    }

                    auto end_seek_or_error = Core::System::lseek(fd, 0, SEEK_END);
                    if (end_seek_or_error.is_error()) {
                        (void)Core::System::close(fd);
                        return end_seek_or_error.release_error();
                    }

                    m_fd = fd;
                    m_path = move(candidate);
                    m_sample_rate = sample_rate;
                    m_channel_count = channel_count;
                    m_data_bytes_written = 0;
                    return {};
                }

                continue;
            }

            if (!stat_or_error.error().is_errno() || stat_or_error.error().code() != ENOENT)
                return stat_or_error.release_error();

            auto fd_or_error = Core::System::open(candidate, O_RDWR | O_CREAT | O_EXCL, 0644);
            if (fd_or_error.is_error()) {
                if (fd_or_error.error().is_errno() && fd_or_error.error().code() == EEXIST)
                    continue;
                return fd_or_error.release_error();
            }

            m_fd = fd_or_error.release_value();
            m_path = move(candidate);
            m_sample_rate = sample_rate;
            m_channel_count = channel_count;
            m_data_bytes_written = 0;

            // Write a placeholder WAV header.
            // Format: IEEE float (3), 32-bit.
            u8 out_header[header_size] = { 0 };
            auto write_u16 = [&](size_t offset, u16 value) {
                u16 le = AK::convert_between_host_and_little_endian(value);
                __builtin_memcpy(out_header + offset, &le, sizeof(le));
            };
            auto write_u32 = [&](size_t offset, u32 value) {
                u32 le = AK::convert_between_host_and_little_endian(value);
                __builtin_memcpy(out_header + offset, &le, sizeof(le));
            };

            __builtin_memcpy(out_header + 0, "RIFF", 4);
            write_u32(4, 36); // patched on close
            __builtin_memcpy(out_header + 8, "WAVE", 4);
            __builtin_memcpy(out_header + 12, "fmt ", 4);
            write_u32(16, 16);
            write_u16(20, 3); // IEEE float
            write_u16(22, channel_count);
            write_u32(24, sample_rate);
            u32 byte_rate = sample_rate * static_cast<u32>(channel_count) * 4u;
            write_u32(28, byte_rate);
            write_u16(32, static_cast<u16>(channel_count * 4u));
            write_u16(34, 32);
            __builtin_memcpy(out_header + 36, "data", 4);
            write_u32(40, 0); // patched on close

            TRY(Core::System::write(m_fd, ReadonlyBytes { out_header, sizeof(out_header) }));
            return {};
        }

        return Error::from_string_literal("OhNoesRenderNode: could not find available suffix");
    }

    ErrorOr<void> write_interleaved_samples(ReadonlySpan<f32> interleaved)
    {
        if (!is_open())
            return Error::from_string_literal("OhNoesRenderNode: write on closed file");

        ReadonlyBytes bytes { reinterpret_cast<u8 const*>(interleaved.data()), interleaved.size() * sizeof(f32) };
        TRY(Core::System::write(m_fd, bytes));
        m_data_bytes_written += bytes.size();
        return {};
    }
};
#endif

OhNoesRenderNode::OhNoesRenderNode(NodeID node_id, size_t quantum_size)
    : RenderNode(node_id)
    , m_output(1, quantum_size, max_channel_count)
{
    m_output.set_channel_count(1);
}

OhNoesRenderNode::OhNoesRenderNode(NodeID node_id, size_t quantum_size, OhNoesGraphNode const& desc)
    : RenderNode(node_id)
    , m_is_debug_node(true)
    , m_base_path(desc.base_path)
    , m_emit_enabled(desc.emit_enabled)
    , m_strip_zero_buffers(desc.strip_zero_buffers)
    , m_output(1, quantum_size, max_channel_count)
{
    m_output.set_channel_count(1);
#ifndef NDEBUG
    m_wav_writer = make<WavWriter>();
#endif
}

OhNoesRenderNode::~OhNoesRenderNode()
{
#ifndef NDEBUG
    if (m_wav_writer)
        m_wav_writer->close_and_finalize();
#endif
}

void OhNoesRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    if (!m_is_debug_node) {
        m_output.zero();
        return;
    }

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const desired_output_channels = mixed_input ? mixed_input->channel_count() : 1;
    m_output.set_channel_count(desired_output_channels);

    if (!mixed_input) {
        m_output.zero();
        return;
    }

    size_t const frames = m_output.frame_count();
    for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
        auto in = mixed_input->channel(ch);
        auto out = m_output.channel(ch);
        for (size_t i = 0; i < frames; ++i)
            out[i] = in[i];
    }

#ifndef NDEBUG
    if (!m_emit_enabled)
        return;
    if (!m_wav_writer)
        return;
    if (m_base_path.is_empty())
        return;
    if (m_has_file_error)
        return;

    u16 channel_count = static_cast<u16>(AK::min(m_output.channel_count(), static_cast<size_t>(NumericLimits<u16>::max())));
    u32 sample_rate = static_cast<u32>(context.sample_rate);
    if (sample_rate == 0)
        sample_rate = 44100;

    if (m_strip_zero_buffers && is_all_zeros(m_output))
        return;

    if (!m_wav_writer->is_open()) {
        auto open_or_error = m_wav_writer->open_next_available(m_base_path, sample_rate, channel_count);
        if (open_or_error.is_error()) {
            dbgln("[WebAudio][OhNoes] Failed to open wav output: {}", open_or_error.error());
            m_has_file_error = true;
            return;
        }
    }

    m_planar_channels.resize(channel_count);
    for (size_t ch = 0; ch < channel_count; ++ch)
        m_planar_channels[ch] = m_output.channel(ch);

    size_t interleaved_sample_count = static_cast<size_t>(channel_count) * frames;
    if (m_interleaved_samples.size() < interleaved_sample_count)
        m_interleaved_samples.resize(interleaved_sample_count);

    copy_planar_to_interleaved(m_planar_channels.span(), m_interleaved_samples.span().slice(0, interleaved_sample_count), frames);
    (void)m_wav_writer->write_interleaved_samples(m_interleaved_samples.span().slice(0, interleaved_sample_count));
#else
    (void)context;
#endif
}

AudioBus const& OhNoesRenderNode::output(size_t) const
{
    ASSERT_RENDER_THREAD();
    return m_output;
}

void OhNoesRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();

    if (!m_is_debug_node)
        return;
    if (!node.has<OhNoesGraphNode>())
        return;

    auto const& desc = node.get<OhNoesGraphNode>();
    bool const new_emit_enabled = desc.emit_enabled;
    bool const new_strip_zero_buffers = desc.strip_zero_buffers;
    if (new_emit_enabled == m_emit_enabled && new_strip_zero_buffers == m_strip_zero_buffers)
        return;

    bool const emit_was_enabled = m_emit_enabled;
    m_emit_enabled = new_emit_enabled;
    m_strip_zero_buffers = new_strip_zero_buffers;

    if (!emit_was_enabled && m_emit_enabled)
        m_has_file_error = false;

#ifndef NDEBUG
    if (emit_was_enabled && !m_emit_enabled && m_wav_writer)
        m_wav_writer->close_and_finalize();
#endif
}

}
