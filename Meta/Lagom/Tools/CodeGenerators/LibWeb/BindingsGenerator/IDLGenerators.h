/*
 * Copyright (c) 2020-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringBuilder.h>
#include <LibIDL/Types.h>

namespace IDL {

void generate_namespace_header(IDL::Interface const&, StringBuilder&);
void generate_namespace_implementation(IDL::Interface const&, StringBuilder&);
void generate_constructor_header(IDL::Interface const&, StringBuilder&);
void generate_constructor_implementation(IDL::Interface const&, StringBuilder&);
void generate_prototype_header(IDL::Interface const&, StringBuilder&);
void generate_prototype_implementation(IDL::Interface const&, StringBuilder&);
void generate_iterator_prototype_header(IDL::Interface const&, StringBuilder&);
void generate_iterator_prototype_implementation(IDL::Interface const&, StringBuilder&);
void generate_global_mixin_header(IDL::Interface const&, StringBuilder&);
void generate_global_mixin_implementation(IDL::Interface const&, StringBuilder&);

extern Vector<StringView> g_header_search_paths;

}
