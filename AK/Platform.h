/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if __has_include(<features.h>)
#    include <features.h>
#endif

#ifndef USING_AK_GLOBALLY
#    define USING_AK_GLOBALLY 1
#endif

#ifdef __x86_64__
#    define AK_IS_ARCH_X86_64() 1
#else
#    define AK_IS_ARCH_X86_64() 0
#endif

#if defined(__i386__) && !defined(__x86_64__)
#    define AK_IS_ARCH_I386() 1
#else
#    define AK_IS_ARCH_I386() 0
#endif

#ifdef __aarch64__
#    define AK_IS_ARCH_AARCH64() 1
#else
#    define AK_IS_ARCH_AARCH64() 0
#endif

#if defined(__riscv) && __riscv_xlen == 64
#    define AK_IS_ARCH_RISCV64() 1
#else
#    define AK_IS_ARCH_RISCV64() 0
#endif

#if defined(__ppc64__) || defined(__PPC64__)
#    define AK_IS_ARCH_PPC64() 1
#else
#    define AK_IS_ARCH_PPC64() 0
#endif
#if defined(__ppc64le__) || defined(__PPC64LE__)
#    define AK_IS_ARCH_PPC64LE() 1
#else
#    define AK_IS_ARCH_PPC64LE() 0
#endif

#if defined(__ppc__) || defined(__PPC__) || defined(__powerpc__) || defined(__powerpc) || defined(__POWERPC__)
#    define AK_IS_ARCH_PPC() 1
#else
#    define AK_IS_ARCH_PPC() 0
#endif

#ifdef __wasm32__
#    define AK_IS_ARCH_WASM32() 1
#else
#    define AK_IS_ARCH_WASM32() 0
#endif

#if (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8) || defined(_WIN64)
#    define AK_ARCH_64_BIT
#else
#    define AK_ARCH_32_BIT
#endif

#if defined(__clang__) || defined(__CLION_IDE__) || defined(__CLION_IDE_)
#    define AK_COMPILER_CLANG
#elif defined(__GNUC__)
#    define AK_COMPILER_GCC
#endif

#if defined(AK_COMPILER_CLANG) && defined(__apple_build_version__)
#    define AK_COMPILER_APPLE_CLANG
#endif

#if defined(__GLIBC__)
#    define AK_LIBC_GLIBC
#    define AK_LIBC_GLIBC_PREREQ(maj, min) __GLIBC_PREREQ((maj), (min))
#else
#    define AK_LIBC_GLIBC_PREREQ(maj, min) 0
#endif

#if defined(__serenity__)
#    define AK_OS_SERENITY
#endif

#if defined(__linux__)
#    define AK_OS_LINUX
#endif

#if defined(__APPLE__) && defined(__MACH__) && !defined(__IOS__)
#    define AK_OS_MACOS
#    define AK_OS_BSD_GENERIC
#endif

#if defined(__IOS__)
#    define AK_OS_IOS
#    define AK_OS_BSD_GENERIC
#endif

#if defined(__FreeBSD__)
#    define AK_OS_BSD_GENERIC
#    define AK_OS_FREEBSD
#endif

#if defined(__NetBSD__)
#    define AK_OS_BSD_GENERIC
#    define AK_OS_NETBSD
#endif

#if defined(__OpenBSD__)
#    define AK_OS_BSD_GENERIC
#    define AK_OS_OPENBSD
#endif

#if defined(__DragonFly__)
#    define AK_OS_BSD_GENERIC
#    define AK_OS_DRAGONFLY
#endif

#if defined(__sun)
#    define AK_OS_BSD_GENERIC
#    define AK_OS_SOLARIS
#endif

#if defined(__HAIKU__)
#    define AK_OS_HAIKU
#endif

#if defined(__gnu_hurd__)
#    define AK_OS_GNU_HURD
#endif

#if defined(__MACH__)
#    define AK_OS_MACH
#endif

#if defined(_WIN32) || defined(_WIN64)
#    define AK_OS_WINDOWS
#endif

#if defined(__ANDROID__)
#    define STR(x) __STR(x)
#    define __STR(x) #x
#    if __ANDROID_API__ < 30
#        pragma message "Invalid android API " STR(__ANDROID_API__)
#        error "Build configuration not tested on configured Android API version"
#    endif
#    undef STR
#    undef __STR
#    define AK_OS_ANDROID
#endif

#if defined(__EMSCRIPTEN__)
#    define AK_OS_EMSCRIPTEN
#endif

#define ARCH(arch) (AK_IS_ARCH_##arch())

#if ARCH(X86_64) || ARCH(I386)
#    define VALIDATE_IS_X86()
#else
#    define VALIDATE_IS_X86() static_assert(false, "Trying to include x86 only header on non x86 platform");
#endif

#if ARCH(AARCH64)
#    define VALIDATE_IS_AARCH64()
#else
#    define VALIDATE_IS_AARCH64() static_assert(false, "Trying to include aarch64 only header on non aarch64 platform");
#endif

#if ARCH(RISCV64)
#    define VALIDATE_IS_RISCV64()
#else
#    define VALIDATE_IS_RISCV64() static_assert(false, "Trying to include riscv64 only header on non riscv64 platform");
#endif

#ifdef ALWAYS_INLINE
#    undef ALWAYS_INLINE
#endif
#define ALWAYS_INLINE __attribute__((always_inline)) inline

#ifdef NEVER_INLINE
#    undef NEVER_INLINE
#endif
#define NEVER_INLINE __attribute__((noinline))

#ifdef FLATTEN
#    undef FLATTEN
#endif
#define FLATTEN __attribute__((flatten))

#ifdef RETURNS_NONNULL
#    undef RETURNS_NONNULL
#endif
#define RETURNS_NONNULL __attribute__((returns_nonnull))

#ifdef NO_SANITIZE_COVERAGE
#    undef NO_SANITIZE_COVERAGE
#endif
#if defined(AK_COMPILER_CLANG)
#    define NO_SANITIZE_COVERAGE __attribute__((no_sanitize("coverage")))
#else
#    define NO_SANITIZE_COVERAGE __attribute__((no_sanitize_coverage))
#endif

#ifdef NO_SANITIZE_ADDRESS
#    undef NO_SANITIZE_ADDRESS
#endif
#if defined(AK_COMPILER_CLANG)
#    define NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))
#else
#    define NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address))
#endif

#ifdef NAKED
#    undef NAKED
#endif
#if !ARCH(AARCH64) || defined(AK_COMPILER_CLANG)
#    define NAKED __attribute__((naked))
#else
// GCC doesn't support __attribute__((naked)) on AArch64. We use NAKED to mark functions
// that are entirely written in inline assembly; having these be inlined would cause
// various problems, such as explicit `ret` instructions accidentally exiting the caller
// function or GCC discarding function arguments as they appear "dead".
#    define NAKED NEVER_INLINE
#endif

#ifdef DISALLOW
#    undef DISALLOW
#endif
#if defined(AK_COMPILER_CLANG)
#    define DISALLOW(message) __attribute__((diagnose_if(1, message, "error")))
#else
#    define DISALLOW(message) __attribute__((error(message)))
#endif

#ifdef NO_UNIQUE_ADDRESS
#    undef NO_UNIQUE_ADDRESS
#endif
#if defined(AK_OS_WINDOWS)
#    define NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#    define NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

// GCC doesn't have __has_feature but clang does
#ifndef __has_feature
#    define __has_feature(...) 0
#endif

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#    define HAS_ADDRESS_SANITIZER
#    define ASAN_POISON_MEMORY_REGION(addr, size) __asan_poison_memory_region(addr, size)
#    define ASAN_UNPOISON_MEMORY_REGION(addr, size) __asan_unpoison_memory_region(addr, size)
#    define LSAN_REGISTER_ROOT_REGION(base, size) __lsan_register_root_region(base, size)
#    define LSAN_UNREGISTER_ROOT_REGION(base, size) __lsan_unregister_root_region(base, size)
#else
#    define ASAN_POISON_MEMORY_REGION(addr, size)
#    define ASAN_UNPOISON_MEMORY_REGION(addr, size)
#    define LSAN_REGISTER_ROOT_REGION(base, size)
#    define LSAN_UNREGISTER_ROOT_REGION(base, size)
#endif

#if __has_feature(blocks)
#    define AK_HAS_BLOCKS
#endif

#if __has_feature(objc_arc)
#    define AK_HAS_OBJC_ARC
#endif

#ifndef AK_OS_SERENITY
#    ifdef AK_OS_WINDOWS
// FIXME: No idea where to get this, but it's 4096 anyway :^)
#        define PAGE_SIZE 4096
#    else
#        include <unistd.h>
#        undef PAGE_SIZE
#        define PAGE_SIZE sysconf(_SC_PAGESIZE)
#    endif
#endif

#if defined(AK_OS_WINDOWS)
#    define MSG_NOSIGNAL 0
#endif

#if defined(AK_OS_BSD_GENERIC) && !defined(AK_OS_FREEBSD) || defined(AK_OS_HAIKU) || defined(AK_OS_WINDOWS)
#    define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#    define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif

#ifndef AK_SYSTEM_CACHE_ALIGNMENT_SIZE
#    if ARCH(AARCH64) || ARCH(X86_64)
#        define AK_SYSTEM_CACHE_ALIGNMENT_SIZE 64
#    else
#        define AK_SYSTEM_CACHE_ALIGNMENT_SIZE 128
#    endif
#endif /* AK_SYSTEM_CACHE_ALIGNMENT_SIZE */

#ifdef AK_CACHE_ALIGNED
#    undef AK_CACHE_ALIGNED
#endif
#define AK_CACHE_ALIGNED alignas(AK_SYSTEM_CACHE_ALIGNMENT_SIZE)
