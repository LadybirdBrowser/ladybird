/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/StringView.h>

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
#    define IS_MOBILE false
#elif defined(AK_OS_ANDROID)
#    define OS_STRING "Android 10"
#    define IS_MOBILE true
#elif defined(AK_OS_LINUX)
#    define OS_STRING "Linux"
#    define IS_MOBILE false
#elif defined(AK_OS_MACOS)
#    define OS_STRING "macOS"
#    define IS_MOBILE false
#elif defined(AK_OS_IOS)
#    define OS_STRING "iOS"
#    define IS_MOBILE true
#elif defined(AK_OS_WINDOWS)
#    define OS_STRING "Windows"
#    define IS_MOBILE false
#elif defined(AK_OS_FREEBSD)
#    define OS_STRING "FreeBSD"
#    define IS_MOBILE false
#elif defined(AK_OS_OPENBSD)
#    define OS_STRING "OpenBSD"
#    define IS_MOBILE false
#elif defined(AK_OS_NETBSD)
#    define OS_STRING "NetBSD"
#    define IS_MOBILE false
#elif defined(AK_OS_DRAGONFLY)
#    define OS_STRING "DragonFly"
#    define IS_MOBILE false
#elif defined(AK_OS_SOLARIS)
#    define OS_STRING "SunOS"
#    define IS_MOBILE false
#elif defined(AK_OS_HAIKU)
#    define OS_STRING "Haiku"
#    define IS_MOBILE false
#elif defined(AK_OS_GNU_HURD)
#    define OS_STRING "GNU/Hurd"
#    define IS_MOBILE false
#else
#    error Unknown OS
#endif

enum class NavigatorCompatibilityMode {
    Chrome,
    Gecko,
    WebKit
};

#define BROWSER_NAME "Ladybird"
#define BROWSER_MAJOR_VERSION "1"
#define BROWSER_MINOR_VERSION "0"
#define BROWSER_VERSION BROWSER_MAJOR_VERSION "." BROWSER_MINOR_VERSION

constexpr auto default_user_agent = "Mozilla/5.0 (" OS_STRING "; " CPU_STRING ") " BROWSER_NAME "/" BROWSER_VERSION ""sv;
constexpr auto default_sec_user_agent = "\"Not/A)Brand\";v=\"8\", \"Chromium\";v=\"116\", \"Google Chrome\";v=\"116\""sv;
constexpr auto default_platform = OS_STRING " " CPU_STRING ""sv;
constexpr auto default_os = OS_STRING ""sv;
constexpr auto default_navigator_compatibility_mode = NavigatorCompatibilityMode::Chrome;
constexpr auto default_is_mobile = IS_MOBILE;
constexpr auto default_enable_client_hints = true;

constexpr auto browser_name = BROWSER_NAME ""sv;
constexpr auto browser_major_version = BROWSER_MAJOR_VERSION ""sv;
}
