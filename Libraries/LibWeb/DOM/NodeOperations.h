/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

WebIDL::ExceptionOr<GC::Ref<Node>> convert_nodes_to_single_node(Vector<Variant<GC::Root<Node>, String>> const& nodes, DOM::Document& document);

}
