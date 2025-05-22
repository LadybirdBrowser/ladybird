/*
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibJS/Forward.h>

namespace Web::HTML {

using DeserializationMemory = GC::RootVector<JS::Value>;
using SerializationRecord = Vector<u32>;
using SerializationMemory = HashMap<GC::Root<JS::Value>, u32>;

}
