/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>
#include <LibWeb/Streams/CountQueuingStrategy.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(CountQueuingStrategy);

// https://streams.spec.whatwg.org/#blqs-constructor
GC::Ref<CountQueuingStrategy> CountQueuingStrategy::construct_impl(Bindings::QueuingStrategyInit const& init)
{
    // The new CountQueuingStrategy(init) constructor steps are:
    // 1. Set this.[[highWaterMark]] to init["highWaterMark"].
    return GC::Heap::the().allocate<CountQueuingStrategy>(init.high_water_mark);
}

CountQueuingStrategy::CountQueuingStrategy(double high_water_mark)
    : m_high_water_mark(high_water_mark)
{
}

CountQueuingStrategy::~CountQueuingStrategy() = default;

// https://streams.spec.whatwg.org/#cqs-size
GC::Ref<WebIDL::CallbackType> CountQueuingStrategy::size(JS::Realm& realm)
{
    // 1. Return this's relevant global object's count queuing strategy size function.
    auto& global = HTML::relevant_settings_object(realm.global_object()).universal_global_scope();
    return global.count_queuing_strategy_size_function();
}

}
