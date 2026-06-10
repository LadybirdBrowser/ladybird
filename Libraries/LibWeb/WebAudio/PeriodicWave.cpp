/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebAudio/PeriodicWave.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(PeriodicWave);

static ErrorOr<Vector<float>> copy_float_vector(ReadonlySpan<float> source)
{
    Vector<float> vector;
    TRY(vector.try_append(source.data(), source.size()));
    return vector;
}

static ErrorOr<Vector<float>> zeroed_float_vector(size_t size)
{
    Vector<float> vector;
    TRY(vector.try_resize(size));
    return vector;
}

// https://webaudio.github.io/web-audio-api/#dom-periodicwave-periodicwave
WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> PeriodicWave::create_for_constructor(GC::Ref<BaseAudioContext>, PeriodicWaveOptions const& options)
{
    auto& vm = JS::VM::the();

    // 1. Let p be a new PeriodicWave object. Let [[real]] and [[imag]] be two internal slots of type Float32Array, and let [[normalize]] be an internal slot.
    auto p = GC::Heap::the().allocate<PeriodicWave>();

    // 2. Process options according to one of the following cases:
    {
        // 1. If both options.real and options.imag are present
        if (options.real.has_value() && options.imag.has_value()) {
            // 1. If the lengths of options.real and options.imag are different or if either length is less than 2, throw an IndexSizeError and abort this algorithm.
            if (options.real.value().size() != options.imag.value().size() || options.real.value().size() < 2)
                return WebIDL::IndexSizeError::create("Real and imaginary arrays must have the same length and contain at least 2 elements"_utf16);

            // 2. Set [[real]] and [[imag]] to new arrays with the same length as options.real.
            // 3. Copy all elements from options.real to [[real]] and options.imag to [[imag]].
            p->m_real = TRY_OR_THROW_OOM(vm, copy_float_vector(*options.real));
            p->m_imag = TRY_OR_THROW_OOM(vm, copy_float_vector(*options.imag));
        }
        // 2. If only options.real is present
        else if (options.real.has_value()) {
            // 1. If length of options.real is less than 2, throw an IndexSizeError and abort this algorithm
            if (options.real.value().size() < 2)
                return WebIDL::IndexSizeError::create("Real array must contain at least 2 elements"_utf16);

            // 2. Set [[real]] and [[imag]] to arrays with the same length as options.real
            // 3. Copy options.real to [[real]] and set [[imag]] to all zeros.
            p->m_real = TRY_OR_THROW_OOM(vm, copy_float_vector(*options.real));
            p->m_imag = TRY_OR_THROW_OOM(vm, zeroed_float_vector(options.real->size()));
        }
        // 3. If only options.imag is present
        else if (options.imag.has_value()) {
            // 1. If length of options.imag is less than 2, throw an IndexSizeError and abort this algorithm.
            if (options.imag.value().size() < 2)
                return WebIDL::IndexSizeError::create("Imaginary array must contain at least 2 elements"_utf16);

            // 2. Set [[real]] and [[imag]] to arrays with the same length as options.imag.
            // 3. Copy options.imag to [[imag]] and set [[real]] to all zeros.
            p->m_real = TRY_OR_THROW_OOM(vm, zeroed_float_vector(options.imag->size()));
            p->m_imag = TRY_OR_THROW_OOM(vm, copy_float_vector(*options.imag));
        }
        // 4. Otherwise
        else {
            // 1. Set [[real]] and [[imag]] to zero-filled arrays of length 2.
            p->m_real = TRY_OR_THROW_OOM(vm, zeroed_float_vector(2));
            p->m_imag = TRY_OR_THROW_OOM(vm, zeroed_float_vector(2));

            // 2. Set element at index 1 of [[imag]] to 1.
            p->m_imag[1] = 1;
        }
    }

    // 3. Set element at index 0 of both [[real]] and [[imag]] to 0. (This sets the DC component to 0.)
    p->m_real[0] = 0;
    p->m_imag[0] = 0;

    // 4. Initialize [[normalize]] to the inverse of the disableNormalization attribute of the PeriodicWaveConstraints on the PeriodicWaveOptions.
    p->m_normalize = !options.disable_normalization;

    // 5. Return p.
    return p;
}

PeriodicWave::PeriodicWave()
{
}

PeriodicWave::~PeriodicWave() = default;

size_t PeriodicWave::external_memory_size() const
{
    auto size = JS::saturating_add_external_memory_size(Base::external_memory_size(), JS::vector_external_memory_size(m_real));
    size = JS::saturating_add_external_memory_size(size, JS::vector_external_memory_size(m_imag));
    return size;
}

}
