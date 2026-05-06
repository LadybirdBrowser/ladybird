/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCore/Environment.h>

namespace PaintServer {

// Press cmd+shift+L on mac or ctrl+shift+L on Qt/Linux to receive all the spam for a single frame.
static constexpr u32 LOG_SNAPSHOT = static_cast<u32>(-1);

static constexpr u32 LOG_GENERAL = 1;
static constexpr u32 LOG_TIMING = 2;
static constexpr u32 LOG_RESOURCE = 4;
static constexpr u32 LOG_INGRESS = 8;
static constexpr u32 LOG_TILING = 16;
static constexpr u32 LOG_LAYER_STATE = 32;
static constexpr u32 LOG_DRAWING = 64;
static constexpr u32 LOG_SCROLL = 128;

inline Atomic<bool> g_log_next_frame_pending { false };
inline Atomic<u64> g_log_next_frame_token { 0 };

inline u32 logging_mask_env_var()
{
    static u32 const mask = [] {
        auto value = Core::Environment::get("PAINT_SERVER_LOG"sv);
        if (!value.has_value())
            return 0u;

        return value->to_number<u32>().value_or(0);
    }();
    return mask;
}

inline u32 logging_mask()
{
    if (g_log_next_frame_pending.load() || g_log_next_frame_token.load() != 0)
        return LOG_SNAPSHOT;
    return logging_mask_env_var();
}

inline bool is_debug_enabled()
{
    static u32 const grid = [] {
        auto value = Core::Environment::get("GPU_SERVER_GRID"sv);
        if (!value.has_value())
            return 0u;

        return value->to_number<u32>().value_or(0);
    }();
    return grid;
}

inline bool is_logging_enabled(u32 category = LOG_GENERAL)
{
    if (g_log_next_frame_pending.load() || g_log_next_frame_token.load() != 0)
        return true;
    return (logging_mask_env_var() & category) != 0;
}

inline void set_log_next_frame()
{
    g_log_next_frame_pending.store(true);
}

inline void set_log_next_frame(u64 release_token)
{
    if (!g_log_next_frame_pending.exchange(false))
        return;
    g_log_next_frame_token.store(release_token);
}

inline void clear_log_frame()
{
    g_log_next_frame_pending.store(false);
    g_log_next_frame_token.store(0);
}

inline void clear_log_frame(u64 release_token)
{
    if (g_log_next_frame_token.load() != release_token)
        return;
    g_log_next_frame_token.store(0);
}

enum class OnceLogDecision : u8 {
    Suppress,
    Initial,
    Expiry,
};

u32 dbglog_occurrence_count(StringView key);
bool dbg_has_elapsed(StringView key, u32 interval_ms);

template<typename... Parameters>
inline void dbgonce(u32 after, StringView key, CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    u32 const occurrence_count = dbglog_occurrence_count(key);
    if (occurrence_count != after)
        return;

    AK::VariadicFormatParams<AK::AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_parameters { parameters... };
    auto message = ByteString::vformatted(fmtstr.view(), variadic_format_parameters);
    dbgln("ONCE EXPIRY({}): {}", after, message);
}

template<typename... Parameters>
inline void dbgbefore(u32 max_count, StringView key, CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    u32 const occurrence_count = dbglog_occurrence_count(key);
    if (occurrence_count > max_count)
        return;

    AK::VariadicFormatParams<AK::AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_parameters { parameters... };
    auto message = ByteString::vformatted(fmtstr.view(), variadic_format_parameters);

    if (occurrence_count < max_count) {
        dbgln("BEFORE({}/{}): {}", occurrence_count, max_count, message);
        return;
    }

    dbgln("BEFORE EXPIRY({}/{}): {}", occurrence_count, max_count, message);
}

template<typename... Parameters>
inline void dbgevery(u32 count, StringView key, CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    u32 const occurrence_count = dbglog_occurrence_count(key);
    if (occurrence_count % count != 0)
        return;

    AK::VariadicFormatParams<AK::AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_parameters { parameters... };
    auto message = ByteString::vformatted(fmtstr.view(), variadic_format_parameters);
    dbgln("EVERY({}): {}", occurrence_count, message);
}

template<typename... Parameters>
inline void dbgtimed(u32 interval_ms, StringView key, CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    if (!dbg_has_elapsed(key, interval_ms))
        return;

    AK::VariadicFormatParams<AK::AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_parameters { parameters... };
    auto message = ByteString::vformatted(fmtstr.view(), variadic_format_parameters);
    dbgln("AT({}ms): {}", interval_ms, message);
}

void dbgtrack(StringView key, float value, Optional<u32> interval_ms = {});

}
