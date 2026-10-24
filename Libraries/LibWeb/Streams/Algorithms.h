/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// The algorithms that the specifications that LibWeb implements in C++ set streams up with.
using PullAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>()>;
using CancelAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>;
using FlushAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>()>;
using TransformAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>;

}
