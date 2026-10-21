/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/TextMetrics.h>

namespace Web::HTML {

// NB: `TextMeasurements` of Bindings/Rust/src/html/text_metrics.rs is twelve doubles in this order.
static_assert(sizeof(TextMeasurements) == 12 * sizeof(double));
static_assert(offsetof(TextMeasurements, actual_bounding_box_right) == 2 * sizeof(double));
static_assert(offsetof(TextMeasurements, ideographic_baseline) == 11 * sizeof(double));

// Defined in Bindings/Rust/src/html/exports.rs.
extern "C" void* web_text_metrics_create(JSVM*, TextMeasurements const*);

GC::Ref<TextMetrics> TextMetrics::create(TextMeasurements const& measurements)
{
    return *static_cast<TextMetrics*>(web_text_metrics_create(JS::HostABI::vm_to_abi(JS::VM::the()), &measurements));
}

}
