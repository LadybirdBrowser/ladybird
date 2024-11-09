/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibJS/Contrib/Test262/AgentObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <unistd.h>

namespace JS::Test262 {

JS_DEFINE_ALLOCATOR(AgentObject);

AgentObject::AgentObject(Realm& realm)
    : Object(Object::ConstructWithoutPrototypeTag::Tag, realm)
{
}

void AgentObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, "monotonicNow", monotonic_now, 0, attr);
    define_native_function(realm, "sleep", sleep, 1, attr);
    // TODO: broadcast
    // TODO: getReport
    // TODO: start
}

JS_DEFINE_NATIVE_FUNCTION(AgentObject::monotonic_now)
{
    auto time = MonotonicTime::now();
    auto milliseconds = time.milliseconds();
    return Value(static_cast<double>(milliseconds));
}

JS_DEFINE_NATIVE_FUNCTION(AgentObject::sleep)
{
    auto milliseconds = TRY(vm.argument(0).to_i32(vm));
    ::usleep(milliseconds * 1000);
    return js_undefined();
}

}
