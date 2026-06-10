/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Transformer.h>
#include <LibWeb/Bindings/UnderlyingSink.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

using SizeAlgorithm = GC::Function<JS::Completion(JS::Value)>;
using PullAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>()>;
using CancelAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>;
using StartAlgorithm = GC::Function<WebIDL::ExceptionOr<JS::Value>()>;
using AbortAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>;
using CloseAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>()>;
using WriteAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>;
using FlushAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>()>;
using TransformAlgorithm = GC::Function<GC::Ref<WebIDL::Promise>(JS::Value)>;

struct UnderlyingSource {
    Optional<u64> auto_allocate_chunk_size {};
    GC::Ptr<WebIDL::CallbackType> cancel {};
    GC::Ptr<WebIDL::CallbackType> pull {};
    GC::Ptr<WebIDL::CallbackType> start {};
    bool is_bytes { false };
};

using UnderlyingSink = Bindings::UnderlyingSink;
using Transformer = Bindings::Transformer;

}
