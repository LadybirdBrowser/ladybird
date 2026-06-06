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
#include <LibWeb/Streams/ByteLengthQueuingStrategy.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(ByteLengthQueuingStrategy);

// https://streams.spec.whatwg.org/#blqs-constructor
GC::Ref<ByteLengthQueuingStrategy> ByteLengthQueuingStrategy::construct_impl(Bindings::QueuingStrategyInit const& init)
{
    // The new ByteLengthQueuingStrategy(init) constructor steps are:
    // 1. Set this.[[highWaterMark]] to init["highWaterMark"].
    return GC::Heap::the().allocate<ByteLengthQueuingStrategy>(init.high_water_mark);
}

ByteLengthQueuingStrategy::ByteLengthQueuingStrategy(double high_water_mark)
    : m_high_water_mark(high_water_mark)
{
}

ByteLengthQueuingStrategy::~ByteLengthQueuingStrategy() = default;

// https://streams.spec.whatwg.org/#blqs-size
GC::Ref<WebIDL::CallbackType> ByteLengthQueuingStrategy::size(JS::Realm& realm)
{
    // 1. Return this's relevant global object's byte length queuing strategy size function.
    auto& global = HTML::relevant_settings_object(realm.global_object()).universal_global_scope();
    return global.byte_length_queuing_strategy_size_function();
}

}
