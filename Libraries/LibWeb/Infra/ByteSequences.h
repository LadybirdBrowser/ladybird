/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Web::Infra {

void byte_lowercase(ByteBuffer&);
void byte_uppercase(ByteBuffer&);
bool is_prefix_of(ReadonlyBytes potential_prefix, ReadonlyBytes input);
bool is_byte_less_than(ReadonlyBytes a, ReadonlyBytes b);

}
