/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CountQueuingStrategyPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Streams/CountQueuingStrategy.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(CountQueuingStrategy);

// https://streams.spec.whatwg.org/#blqs-constructor
GC::Ref<CountQueuingStrategy> CountQueuingStrategy::construct_impl(JS::Realm& realm, QueuingStrategyInit const& init)
{
    // The new CountQueuingStrategy(init) constructor steps are:
    // 1. Set this.[[highWaterMark]] to init["highWaterMark"].
    return realm.create<CountQueuingStrategy>(realm, init.high_water_mark);
}

CountQueuingStrategy::CountQueuingStrategy(JS::Realm& realm, double high_water_mark)
    : PlatformObject(realm)
    , m_high_water_mark(high_water_mark)
{
}

CountQueuingStrategy::~CountQueuingStrategy() = default;

// https://streams.spec.whatwg.org/#cqs-size
GC::Ref<WebIDL::CallbackType> CountQueuingStrategy::size()
{
    // 1. Return this's relevant global object's count queuing strategy size function.
    return verify_cast<HTML::Window>(HTML::relevant_global_object(*this)).count_queuing_strategy_size_function();
}

void CountQueuingStrategy::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CountQueuingStrategy);
}

}
