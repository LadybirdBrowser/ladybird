/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/AudioParamPrototype.h>

namespace Web::WebAudio {

struct AudioParamDescriptor {
    FlyString name;
    float default_value { 0.0f };
    float min_value { 0.0f };
    float max_value { 0.0f };
    Bindings::AutomationRate automation_rate { Bindings::AutomationRate::ARate };
};

}
