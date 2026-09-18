/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/RefCounted.h>
#include <AK/kmalloc.h>
#include <string>

struct ProjectType {
    int value { 0 };
};

class Counted : public RefCounted<Counted> {
public:
    int value { 0 };
};

// expected-error@+2 {{AllocatedWithSystemAllocator names 'ProjectType', which is defined in this project. Use AK_ALLOC_WITH_KMALLOC or AllocatedWithCustomAllocator instead}}
template<>
inline constexpr bool AllocatedWithSystemAllocator<ProjectType> = true;

// expected-error@+2 {{AllocatedWithCustomAllocator names 'Counted', which already allocates with kmalloc}}
template<>
inline constexpr bool AllocatedWithCustomAllocator<Counted> = true;

// expected-error-re@+2 {{AllocatedWithCustomAllocator names '{{.*}}basic_string<char>', which is defined outside this project. Use AllocatedWithSystemAllocator instead}}
template<>
inline constexpr bool AllocatedWithCustomAllocator<std::string> = true;
