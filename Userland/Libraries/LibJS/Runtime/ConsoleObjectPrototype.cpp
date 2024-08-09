/*
 * Copyright (c) 2024, Gasim Gasimzada <gasim@gasimzada.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConsoleObjectPrototype.h"

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/ObjectPrototype.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ConsoleObjectPrototype);

ConsoleObjectPrototype::ConsoleObjectPrototype(JS::Realm& realm)
    : Object(JS::Object::ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void ConsoleObjectPrototype::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
}

}
