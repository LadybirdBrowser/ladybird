/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PeriodicWavePrototype.h>
#include <LibWeb/WebAudio/PeriodicWave.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(PeriodicWave);

// https://webaudio.github.io/web-audio-api/#dom-periodicwave-periodicwave
WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> PeriodicWave::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext>, PeriodicWaveOptions const& options)
{
    // 1. Let p be a new PeriodicWave object. Let [[real]] and [[imag]] be two internal slots of type Float32Array, and let [[normalize]] be an internal slot.
    auto p = realm.create<PeriodicWave>(realm);

    // 2. Process options according to one of the following cases:
    {
        // 1. If both options.real and options.imag are present
        if (options.real.has_value() && options.imag.has_value()) {
            // 1. If the lengths of options.real and options.imag are different or if either length is less than 2, throw an IndexSizeError and abort this algorithm.
            if (options.real.value().size() != options.imag.value().size() || options.real.value().size() < 2)
                return WebIDL::IndexSizeError::create(realm, "Real and imaginary arrays must have the same length and contain at least 2 elements"_string);

            // 2. Set [[real]] and [[imag]] to new arrays with the same length as options.real.
            // 3. Copy all elements from options.real to [[real]] and options.imag to [[imag]].
            auto real_byte_buffer = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy({ options.real->data(), options.real->size() * sizeof(float) }));
            auto real_array_buffer = JS::ArrayBuffer::create(realm, move(real_byte_buffer));
            p->m_real = JS::Float32Array::create(realm, options.real->size(), *real_array_buffer);

            auto imag_byte_buffer = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy({ options.imag->data(), options.real->size() * sizeof(float) }));
            auto imag_array_buffer = JS::ArrayBuffer::create(realm, move(imag_byte_buffer));
            p->m_imag = JS::Float32Array::create(realm, options.real->size(), *imag_array_buffer);
        }
        // 2. If only options.real is present
        else if (options.real.has_value()) {
            // 1. If length of options.real is less than 2, throw an IndexSizeError and abort this algorithm
            if (options.real.value().size() < 2)
                return WebIDL::IndexSizeError::create(realm, "Real array must contain at least 2 elements"_string);

            // 2. Set [[real]] and [[imag]] to arrays with the same length as options.real
            // 3. Copy options.real to [[real]] and set [[imag]] to all zeros.
            auto real_byte_buffer = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy({ options.real->data(), options.real->size() * sizeof(float) }));
            auto real_array_buffer = JS::ArrayBuffer::create(realm, move(real_byte_buffer));
            p->m_real = JS::Float32Array::create(realm, options.real->size(), *real_array_buffer);

            auto imag_byte_buffer = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::create_zeroed(options.real->size() * sizeof(float)));
            auto imag_array_buffer = JS::ArrayBuffer::create(realm, move(imag_byte_buffer));
            p->m_imag = JS::Float32Array::create(realm, options.real->size(), *imag_array_buffer);
        }
        // 3. If only options.imag is present
        else if (options.imag.has_value()) {
            // 1. If length of options.imag is less than 2, throw an IndexSizeError and abort this algorithm.
            if (options.imag.value().size() < 2)
                return WebIDL::IndexSizeError::create(realm, "Imaginary array must contain at least 2 elements"_string);

            // 2. Set [[real]] and [[imag]] to arrays with the same length as options.imag.
            // 3. Copy options.imag to [[imag]] and set [[real]] to all zeros.
            auto real_byte_buffer = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::create_zeroed(options.imag->size() * sizeof(float)));
            auto real_array_buffer = JS::ArrayBuffer::create(realm, move(real_byte_buffer));
            p->m_real = JS::Float32Array::create(realm, options.imag->size(), *real_array_buffer);

            auto imag_byte_buffer = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy({ options.imag->data(), options.imag->size() * sizeof(float) }));
            auto imag_array_buffer = JS::ArrayBuffer::create(realm, move(imag_byte_buffer));
            p->m_imag = JS::Float32Array::create(realm, options.imag->size(), *imag_array_buffer);
        }
        // 4. Otherwise
        else {
            // 1. Set [[real]] and [[imag]] to zero-filled arrays of length 2.
            auto real_byte_buffer = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::create_zeroed(2 * sizeof(float)));
            auto real_array_buffer = JS::ArrayBuffer::create(realm, move(real_byte_buffer));
            p->m_real = JS::Float32Array::create(realm, 2, *real_array_buffer);

            auto imag_byte_buffer = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::create_zeroed(2 * sizeof(float)));
            auto imag_array_buffer = JS::ArrayBuffer::create(realm, move(imag_byte_buffer));
            p->m_imag = JS::Float32Array::create(realm, 2, *imag_array_buffer);

            // 2. Set element at index 1 of [[imag]] to 1.
            p->m_imag->set_value_in_buffer(1, JS::Value { 1 }, JS::ArrayBuffer::Order::SeqCst);
        }
    }

    // 3. Set element at index 0 of both [[real]] and [[imag]] to 0. (This sets the DC component to 0.)
    p->m_real->set_value_in_buffer(0, JS::Value { 0 }, JS::ArrayBuffer::Order::SeqCst);
    p->m_imag->set_value_in_buffer(0, JS::Value { 0 }, JS::ArrayBuffer::Order::SeqCst);

    // 4. Initialize [[normalize]] to the inverse of the disableNormalization attribute of the PeriodicWaveConstraints on the PeriodicWaveOptions.
    p->m_normalize = !options.disable_normalization;

    // 5. Return p.
    return p;
}

PeriodicWave::PeriodicWave(JS::Realm& realm)
    : Base(realm)
{
}

PeriodicWave::~PeriodicWave() = default;

void PeriodicWave::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PeriodicWave);
}

void PeriodicWave::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_real);
    visitor.visit(m_imag);
}

}
