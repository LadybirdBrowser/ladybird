/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/SourceLocation.h>
#include <ImageDecoder/Forward.h>
#include <ImageDecoder/ImageDecoderClientEndpoint.h>
#include <ImageDecoder/ImageDecoderServerEndpoint.h>
#include <LibGfx/BitmapSequence.h>
#include <LibGfx/ColorSpace.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/Limits.h>
#include <LibIPC/RateLimiter.h>
#include <LibThreading/BackgroundAction.h>

namespace ImageDecoder {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<ImageDecoderClientEndpoint, ImageDecoderServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override = default;

    virtual void die() override;

    struct DecodeResult {
        bool is_animated = false;
        u32 loop_count = 0;
        Gfx::FloatPoint scale { 1, 1 };
        Gfx::BitmapSequence bitmaps;
        Vector<u32> durations;
        Gfx::ColorSpace color_profile;
    };

private:
    using Job = Threading::BackgroundAction<DecodeResult>;

    explicit ConnectionFromClient(NonnullOwnPtr<IPC::Transport>);

    virtual Messages::ImageDecoderServer::DecodeImageResponse decode_image(Core::AnonymousBuffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type) override;
    virtual void cancel_decoding(i64 image_id) override;
    virtual Messages::ImageDecoderServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;
    virtual Messages::ImageDecoderServer::InitTransportResponse init_transport(int peer_pid) override;

    ErrorOr<IPC::File> connect_new_client();

    NonnullRefPtr<Job> make_decode_image_job(i64 image_id, Core::AnonymousBuffer, Optional<Gfx::IntSize> ideal_size, Optional<ByteString> mime_type);

    i64 m_next_image_id { 0 };
    HashMap<i64, NonnullRefPtr<Job>> m_pending_jobs;

    // Security validation helpers
    [[nodiscard]] bool validate_image_id(i64 image_id, SourceLocation location = SourceLocation::current())
    {
        if (!m_pending_jobs.contains(image_id)) {
            dbgln("Security: WebContent[{}] attempted access to invalid image_id {} at {}:{}",
                m_transport->peer_pid(), image_id, location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_buffer_size(size_t size, SourceLocation location = SourceLocation::current())
    {
        // 100MB maximum for image buffers
        static constexpr size_t MaxImageBufferSize = 100 * 1024 * 1024;
        if (size > MaxImageBufferSize) {
            dbgln("Security: WebContent[{}] sent oversized image buffer ({} bytes, max {}) at {}:{}",
                m_transport->peer_pid(), size, MaxImageBufferSize,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool validate_dimensions(Optional<Gfx::IntSize> const& size, SourceLocation location = SourceLocation::current())
    {
        if (!size.has_value())
            return true;

        // Maximum 32768x32768 to prevent integer overflow
        static constexpr int MaxDimension = 32768;
        if (size->width() > MaxDimension || size->height() > MaxDimension) {
            dbgln("Security: WebContent[{}] sent invalid ideal_size ({}x{}, max {}) at {}:{}",
                m_transport->peer_pid(), size->width(), size->height(), MaxDimension,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        // Prevent zero/negative dimensions
        if (size->width() <= 0 || size->height() <= 0) {
            dbgln("Security: WebContent[{}] sent invalid ideal_size ({}x{}) at {}:{}",
                m_transport->peer_pid(), size->width(), size->height(),
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool validate_mime_type(Optional<ByteString> const& mime_type, SourceLocation location = SourceLocation::current())
    {
        if (!mime_type.has_value())
            return true;

        // Maximum 256 bytes for MIME type
        if (mime_type->length() > 256) {
            dbgln("Security: WebContent[{}] sent oversized MIME type ({} bytes, max 256) at {}:{}",
                m_transport->peer_pid(), mime_type->length(),
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool validate_count(size_t count, size_t max_count, StringView field_name, SourceLocation location = SourceLocation::current())
    {
        if (count > max_count) {
            dbgln("Security: WebContent[{}] sent excessive {} ({}, max {}) at {}:{}",
                m_transport->peer_pid(), field_name, count, max_count,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool check_rate_limit(SourceLocation location = SourceLocation::current())
    {
        if (!m_rate_limiter.try_consume()) {
            dbgln("Security: WebContent[{}] exceeded rate limit at {}:{}",
                m_transport->peer_pid(), location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool check_concurrent_decode_limit(SourceLocation location = SourceLocation::current())
    {
        // Maximum 100 concurrent decode jobs per client
        static constexpr size_t MaxConcurrentDecodes = 100;
        if (m_pending_jobs.size() >= MaxConcurrentDecodes) {
            dbgln("Security: WebContent[{}] exceeded concurrent decode limit ({}, max {}) at {}:{}",
                m_transport->peer_pid(), m_pending_jobs.size(), MaxConcurrentDecodes,
                location.filename(), location.line_number());
            track_validation_failure();
            return false;
        }
        return true;
    }

    void track_validation_failure()
    {
        m_validation_failures++;
        if (m_validation_failures >= s_max_validation_failures) {
            dbgln("Security: WebContent[{}] exceeded validation failure limit ({}), terminating connection",
                m_transport->peer_pid(), s_max_validation_failures);
            die();
        }
    }

    // Security infrastructure
    IPC::RateLimiter m_rate_limiter { 1000, Duration::from_milliseconds(10) }; // 1000 messages/second
    size_t m_validation_failures { 0 };
    static constexpr size_t s_max_validation_failures = 100;
};

}
