/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web {

class EditEventHandler {
public:
    explicit EditEventHandler()
    {
    }

    ~EditEventHandler() = default;

    void handle_delete_character_after(GC::Ref<DOM::Document>, GC::Ref<DOM::Position>);
    void handle_delete(GC::Ref<DOM::Document>, DOM::Range&);
    void handle_insert(GC::Ref<DOM::Document>, GC::Ref<DOM::Position>, u32 code_point);
    void handle_insert(GC::Ref<DOM::Document>, GC::Ref<DOM::Position>, String);
};

}
