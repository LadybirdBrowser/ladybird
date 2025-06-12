/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// NOTE: This file is not named $262Object.cpp because dollar signs in file names cause issues with some build tools.

#include <AK/TypeCasts.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Contrib/Test262/262Object.h>
#include <LibJS/Contrib/Test262/AgentObject.h>
#include <LibJS/Contrib/Test262/GlobalObject.h>
#include <LibJS/Contrib/Test262/IsHTMLDDA.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Script.h>

namespace JS::Test262 {

GC_DEFINE_ALLOCATOR($262Object);

$262Object::$262Object(Realm& realm)
    : Object(Object::ConstructWithoutPrototypeTag::Tag, realm)
{
}

void $262Object::initialize(Realm& realm)
{
    Base::initialize(realm);

    m_agent = realm.create<AgentObject>(realm);
    m_is_htmldda = realm.create<IsHTMLDDA>(realm);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, "clearKeptObjects"_fly_string, clear_kept_objects, 0, attr);
    define_native_function(realm, "createRealm"_fly_string, create_realm, 0, attr);
    define_native_function(realm, "detachArrayBuffer"_fly_string, detach_array_buffer, 1, attr);
    define_native_function(realm, "evalScript"_fly_string, eval_script, 1, attr);

    define_direct_property("agent"_fly_string, m_agent, attr);
    define_direct_property("gc"_fly_string, realm.global_object().get_without_side_effects("gc"_fly_string), attr);
    define_direct_property("global"_fly_string, &realm.global_object(), attr);
    define_direct_property("IsHTMLDDA"_fly_string, m_is_htmldda, attr);
}

void $262Object::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_agent);
    visitor.visit(m_is_htmldda);
}

JS_DEFINE_NATIVE_FUNCTION($262Object::clear_kept_objects)
{
    vm.finish_execution_generation();
    return js_undefined();
}

JS_DEFINE_NATIVE_FUNCTION($262Object::create_realm)
{
    GC::Ptr<JS::Test262::GlobalObject> global_object;
    auto root_execution_context = MUST(JS::Realm::initialize_host_defined_realm(
        vm,
        [&](JS::Realm& realm) -> JS::GlobalObject* {
            global_object = vm.heap().allocate<JS::Test262::GlobalObject>(realm);
            return global_object;
        },
        nullptr));
    vm.pop_execution_context();
    return Value(global_object->$262());
}

JS_DEFINE_NATIVE_FUNCTION($262Object::detach_array_buffer)
{
    auto array_buffer = vm.argument(0);
    if (!array_buffer.is_object() || !is<ArrayBuffer>(array_buffer.as_object()))
        return vm.throw_completion<TypeError>();

    auto& array_buffer_object = static_cast<ArrayBuffer&>(array_buffer.as_object());
    TRY(JS::detach_array_buffer(vm, array_buffer_object, vm.argument(1)));
    return js_null();
}

JS_DEFINE_NATIVE_FUNCTION($262Object::eval_script)
{
    auto source_text = TRY(vm.argument(0).to_string(vm));

    // 1. Let hostDefined be any host-defined values for the provided sourceText (obtained in an implementation dependent manner)

    // 2. Let realm be the current Realm Record.
    auto& realm = *vm.current_realm();

    // 3. Let s be ParseScript(sourceText, realm, hostDefined).
    auto script_or_error = Script::parse(source_text, realm);

    // 4. If s is a List of errors, then
    if (script_or_error.is_error()) {
        // a. Let error be the first element of s.
        auto& error = script_or_error.error()[0];

        // b. Return Completion { [[Type]]: throw, [[Value]]: error, [[Target]]: empty }.
        return vm.throw_completion<SyntaxError>(error.to_string());
    }

    // 5. Let status be ScriptEvaluation(s).
    auto status = vm.bytecode_interpreter().run(script_or_error.value());

    // 6. Return Completion(status).
    return status;
}

}
