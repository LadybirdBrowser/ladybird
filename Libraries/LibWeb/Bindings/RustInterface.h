/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibJS/HostObjectABI.h>

namespace Web::Bindings {

// The cell of an object of an interface that is implemented in Rust, which the wrappers of the object hold as their
// host data. The Rust bindings in Libraries/LibWeb/Bindings/Rust create these cells and know what they hold.
class RustInterfaceCell : public GC::ForeignCell {
};

// The class that the wrapper class of every interface implemented in Rust derives from.
extern "C" JSHostClass const web_bindings_rust_interface_wrapper_host_class;

}
