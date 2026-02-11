/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace JS {

struct FunctionParsingInsights {
    bool uses_this { false };
    bool uses_this_from_environment { false };
    bool contains_direct_call_to_eval { false };
    bool might_need_arguments_object { false };
};

}
