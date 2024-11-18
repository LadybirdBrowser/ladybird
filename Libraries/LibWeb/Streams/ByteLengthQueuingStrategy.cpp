/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ByteLengthQueuingStrategyPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>
#include <LibWeb/Streams/ByteLengthQueuingStrategy.h>

namespace Web::Streams {

GC_DEFINE_ALLOCATOR(ByteLengthQueuingStrategy);

// https://streams.spec.whatwg.org/#blqs-constructor
GC::Ref<ByteLengthQueuingStrategy> ByteLengthQueuingStrategy::construct_impl(JS::Realm& realm, QueuingStrategyInit const& init)
{
    // The new ByteLengthQueuingStrategy(init) constructor steps are:
    // 1. Set this.[[highWaterMark]] to init["highWaterMark"].
    return realm.create<ByteLengthQueuingStrategy>(realm, init.high_water_mark);
}

ByteLengthQueuingStrategy::ByteLengthQueuingStrategy(JS::Realm& realm, double high_water_mark)
    : PlatformObject(realm)
    , m_high_water_mark(high_water_mark)
{
}

ByteLengthQueuingStrategy::~ByteLengthQueuingStrategy() = default;

// https://streams.spec.whatwg.org/#blqs-size
GC::Ref<WebIDL::CallbackType> ByteLengthQueuingStrategy::size()
{
    // 1. Return this's relevant global object's byte length queuing strategy size function.
    auto* global = dynamic_cast<HTML::UniversalGlobalScopeMixin*>(&HTML::relevant_global_object(*this));
    VERIFY(global);
    return global->byte_length_queuing_strategy_size_function();
}

void ByteLengthQueuingStrategy::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ByteLengthQueuingStrategy);
}

}
