/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web {

class EditEventHandler {
public:
    explicit EditEventHandler(HTML::Navigable& navigable)
        : m_navigable(navigable)
    {
    }

    ~EditEventHandler() = default;

    void handle_delete_character_after(JS::NonnullGCPtr<DOM::Position>);
    void handle_delete(DOM::Range&);
    void handle_insert(JS::NonnullGCPtr<DOM::Position>, u32 code_point);
    void handle_insert(JS::NonnullGCPtr<DOM::Position>, String);

private:
    JS::NonnullGCPtr<HTML::Navigable> m_navigable;
};

}
