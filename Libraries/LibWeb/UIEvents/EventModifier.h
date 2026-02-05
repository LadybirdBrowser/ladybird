/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

// https://w3c.github.io/uievents/#event-modifier-initializers
struct EventModifierInit : public UIEventInit {
    bool ctrl_key { false };
    bool shift_key { false };
    bool alt_key { false };
    bool meta_key { false };

    bool modifier_alt_graph { false };
    bool modifier_caps_lock { false };
    bool modifier_fn { false };
    bool modifier_fn_lock { false };
    bool modifier_hyper { false };
    bool modifier_num_lock { false };
    bool modifier_scroll_lock { false };
    bool modifier_super { false };
    bool modifier_symbol { false };
    bool modifier_symbol_lock { false };
};

}
