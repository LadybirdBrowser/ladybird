/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/Task.h>

namespace Web::Fetch::Infrastructure {

using TaskDestination = Variant<Empty, GC::Ref<JS::Object>, NonnullRefPtr<HTML::ParallelQueue>>;

HTML::TaskID queue_fetch_task(TaskDestination, GC::Ref<GC::Function<void()>>);
HTML::TaskID queue_fetch_task(GC::Ref<FetchController>, TaskDestination, GC::Ref<GC::Function<void()>>);

}
