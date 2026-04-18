/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Font/WOFF/Loader.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <LibThreading/ThreadPool.h>
#include <LibWeb/CSS/FontLoading.h>

namespace Web::CSS {

static constexpr u32 woff2_signature = 0x774F4632;

bool requires_off_thread_vector_font_preparation(ByteBuffer const& data, Optional<ByteString> const& mime_type_essence)
{
    if (mime_type_essence == "font/woff2"sv || mime_type_essence == "application/font-woff2"sv)
        return true;
    if (data.size() < sizeof(u32))
        return false;
    auto signature = *bit_cast<BigEndian<u32> const*>(data.data());
    return signature == woff2_signature;
}

static ErrorOr<PreparedVectorFontData> prepare_vector_font_data(ByteBuffer data, Optional<ByteString> mime_type_essence)
{
    if (!requires_off_thread_vector_font_preparation(data, mime_type_essence))
        return PreparedVectorFontData { .data = move(data), .mime_type_essence = move(mime_type_essence) };

    return PreparedVectorFontData {
        .data = TRY(WOFF2::convert_to_ttf(data)),
        .mime_type_essence = ByteString { "font/ttf"sv }
    };
}

ErrorOr<NonnullRefPtr<Gfx::Typeface const>> try_load_vector_font(ByteBuffer const& data, Optional<ByteString> const& mime_type_essence)
{
    auto try_ttf = [&]() -> ErrorOr<NonnullRefPtr<Gfx::Typeface const>> {
        return Gfx::Typeface::try_load_from_temporary_memory(data);
    };
    auto try_woff = [&]() -> ErrorOr<NonnullRefPtr<Gfx::Typeface const>> {
        return WOFF::try_load_from_bytes(data);
    };
    auto try_woff2 = [&]() -> ErrorOr<NonnullRefPtr<Gfx::Typeface const>> {
        return WOFF2::try_load_from_bytes(data);
    };

    if (mime_type_essence.has_value()) {
        if (*mime_type_essence == "font/ttf"sv || *mime_type_essence == "application/x-font-ttf"sv || *mime_type_essence == "font/otf"sv)
            return try_ttf();
        if (*mime_type_essence == "font/woff"sv || *mime_type_essence == "application/font-woff"sv)
            return try_woff();
        if (*mime_type_essence == "font/woff2"sv || *mime_type_essence == "application/font-woff2"sv)
            return try_woff2();
    } else {
        if (auto result = try_ttf(); !result.is_error())
            return result.release_value();
        if (auto result = try_woff(); !result.is_error())
            return result.release_value();
        if (auto result = try_woff2(); !result.is_error())
            return result.release_value();
    }

    return Error::from_string_literal("Automatic format detection failed");
}

void prepare_vector_font_data_off_thread(ByteBuffer data, Function<void(ErrorOr<PreparedVectorFontData>)>&& on_complete, Optional<ByteString> mime_type_essence)
{
    // Keep the callback on the origin thread so any GC roots it captures are
    // also destroyed there.
    auto* callback = new Function<void(ErrorOr<PreparedVectorFontData>)>(move(on_complete));
    auto event_loop_weak = Core::EventLoop::current_weak();

    Threading::ThreadPool::the().submit(
        [data = move(data), callback, event_loop_weak = move(event_loop_weak), mime_type_essence = move(mime_type_essence)]() mutable {
            auto result = prepare_vector_font_data(move(data), move(mime_type_essence));

            auto origin = event_loop_weak->take();
            if (!origin)
                return;

            origin->deferred_invoke([callback, result = move(result)]() mutable {
                (*callback)(move(result));
                delete callback;
            });
        });
}

}
