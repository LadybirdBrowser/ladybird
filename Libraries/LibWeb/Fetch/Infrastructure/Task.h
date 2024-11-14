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

// FIXME: 'or a parallel queue'
using TaskDestination = Variant<Empty, GC::Ref<JS::Object>>;

HTML::TaskID queue_fetch_task(JS::Object&, GC::Ref<GC::Function<void()>>);
HTML::TaskID queue_fetch_task(GC::Ref<FetchController>, JS::Object&, GC::Ref<GC::Function<void()>>);

}
