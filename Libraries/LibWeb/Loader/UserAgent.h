/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/StringView.h>
#include <LibWeb/Loader/NavigatorCompatibilityMode.h>

namespace Web {

#if ARCH(X86_64)
#    define CPU_STRING "x86_64"
#elif ARCH(AARCH64)
#    define CPU_STRING "AArch64"
#elif ARCH(I386)
#    define CPU_STRING "x86"
#elif ARCH(RISCV64)
#    define CPU_STRING "RISC-V 64"
#elif ARCH(PPC64) || ARCH(PPC64LE)
#    define CPU_STRING "PowerPC 64"
#elif ARCH(PPC)
#    define CPU_STRING "PowerPC"
#else
#    error Unknown architecture
#endif

#if defined(AK_OS_SERENITY)
#    define OS_STRING "SerenityOS"
#elif defined(AK_OS_ANDROID)
#    define OS_STRING "Android 10"
#elif defined(AK_OS_LINUX)
#    define OS_STRING "Linux"
#elif defined(AK_OS_MACOS)
#    define OS_STRING "macOS"
#elif defined(AK_OS_IOS)
#    define OS_STRING "iOS"
#elif defined(AK_OS_WINDOWS)
#    define OS_STRING "Windows"
#elif defined(AK_OS_FREEBSD)
#    define OS_STRING "FreeBSD"
#elif defined(AK_OS_OPENBSD)
#    define OS_STRING "OpenBSD"
#elif defined(AK_OS_NETBSD)
#    define OS_STRING "NetBSD"
#elif defined(AK_OS_DRAGONFLY)
#    define OS_STRING "DragonFly"
#elif defined(AK_OS_SOLARIS)
#    define OS_STRING "SunOS"
#elif defined(AK_OS_HAIKU)
#    define OS_STRING "Haiku"
#elif defined(AK_OS_GNU_HURD)
#    define OS_STRING "GNU/Hurd"
#else
#    error Unknown OS
#endif

#define BROWSER_NAME "Ladybird"
#define BROWSER_VERSION "1.0"

// NB: Some web servers treat us very badly unless we pretend to be one of the major browsers.
//     This token is appended to the User-Agent string to improve compatibility.
//     We will need to update this periodically to match a somewhat recent version.
#define SAD_COMPATIBILITY_HACK "Chrome/146.0.0.0 AppleWebKit/537.36 Safari/537.36"

// NB: Servers sniff the parenthesized platform token of the User-Agent string and only recognize
//     the frozen tokens sent by major browsers. Reporting the actual OS and CPU here gets us
//     served degraded or broken content (e.g. the Instagram login page), so the platform token is
//     frozen per OS and decoupled from the real platform.
#if defined(AK_OS_MACOS)
#    define UA_PLATFORM_STRING "Macintosh; Intel Mac OS X 10_15_7"
#elif defined(AK_OS_IOS)
#    define UA_PLATFORM_STRING "iPhone; CPU iPhone OS 17_5 like Mac OS X"
#elif defined(AK_OS_ANDROID)
#    define UA_PLATFORM_STRING "Linux; Android 10"
#elif defined(AK_OS_WINDOWS)
#    define UA_PLATFORM_STRING "Windows NT 10.0; Win64; x64"
#else
#    define UA_PLATFORM_STRING "X11; Linux x86_64"
#endif

constexpr auto default_user_agent = "Mozilla/5.0 (" UA_PLATFORM_STRING ") " BROWSER_NAME "/" BROWSER_VERSION " " SAD_COMPATIBILITY_HACK ""sv;
constexpr auto default_platform = OS_STRING " " CPU_STRING ""sv;
constexpr auto default_navigator_compatibility_mode = NavigatorCompatibilityMode::Chrome;

}
