/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/SafeFunction.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/Task.h>

namespace Web::Fetch::Infrastructure {

// FIXME: 'or a parallel queue'
using TaskDestination = Variant<Empty, JS::NonnullGCPtr<JS::Object>>;

HTML::TaskID queue_fetch_task(JS::Object&, JS::NonnullGCPtr<JS::HeapFunction<void()>>);
HTML::TaskID queue_fetch_task(JS::NonnullGCPtr<FetchController>, JS::Object&, JS::NonnullGCPtr<JS::HeapFunction<void()>>);

}
