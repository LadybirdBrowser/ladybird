/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Editing/CommandNames.h>

namespace Web::Editing::CommandNames {

#define __ENUMERATE_COMMAND_NAME(name, command) \
    FlyString name = command##_fly_string;
ENUMERATE_COMMAND_NAMES
#undef __ENUMERATE_COMMAND_NAME

}
