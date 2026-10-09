/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/CaptureDevices.h>
#include <LibAudio/AudioDevices.h>

namespace Audio {

static constexpr u32 FRAGMENTS_PER_SECOND = 50;

NonnullRefPtr<CaptureDevices> CaptureDevices::create(RecordStreamFactory factory)
{
    return adopt_ref(*new CaptureDevices(move(factory)));
}

CaptureDevices::CaptureDevices(RecordStreamFactory factory)
    : m_factory(move(factory))
{
}

CaptureDevices::~CaptureDevices() = default;

// The device's own format as enumeration reports it, so the backend has nothing to convert.
static SampleSpecification native_specification_for(StringView device_id)
{
    Optional<AudioDeviceInfo> device;
    for (auto const& candidate : AudioDevices::the().input_devices()) {
        bool is_the_device = device_id.is_empty() ? candidate.is_default : candidate.dom_device_id == device_id;
        if (is_the_device) {
            device = candidate;
            break;
        }
    }

    u32 sample_rate = 48000;
    u32 channel_count = 2;
    if (device.has_value()) {
        if (device->sample_rate_hz != 0)
            sample_rate = device->sample_rate_hz;
        channel_count = clamp(device->channel_count, 1u, 2u);
    }
    // FIXME: Support channel layouts beyond mono and stereo.
    return SampleSpecification(sample_rate, channel_count == 1 ? ChannelMap::mono() : ChannelMap::stereo());
}

CaptureSubscriberId CaptureDevices::subscribe(StringView device_id, ReadyCallback on_ready)
{
    auto subscriber_id = m_next_subscriber_id++;
    ByteString device_key { device_id };
    m_subscribers.set(subscriber_id, Subscriber { .device_id = device_key, .on_ready = move(on_ready) });

    auto& device = m_devices.ensure(device_key, [] {
        return Device { .fanout = make_ref_counted<Fanout>(), .stream = nullptr, .specification = {}, .error = {}, .subscribers = {} };
    });
    device.subscribers.append(subscriber_id);

    if (device.specification.has_value() || device.error.has_value()) {
        report_ready(subscriber_id, ready_result_for(device));
        return subscriber_id;
    }
    if (device.subscribers.size() == 1)
        open_device(device_key, device);
    return subscriber_id;
}

void CaptureDevices::open_device(ByteString const& device_id, Device& device)
{
    auto specification = native_specification_for(device_id);
    auto fragment_size_bytes = specification.sample_rate() / FRAGMENTS_PER_SECOND * specification.channel_count() * sizeof(float);
    auto promise = m_factory(specification, static_cast<u32>(fragment_size_bytes), device_id, [fanout = device.fanout](ReadonlyBytes interleaved_samples, SampleSpecification const&) {
        fanout->deliver(interleaved_samples);
    });
    promise->when_resolved([self = NonnullRefPtr(*this), device_id, fanout = device.fanout](NonnullRefPtr<RecordStream>& stream) {
        self->device_opened(device_id, *fanout, stream);
    });
    promise->when_rejected([self = NonnullRefPtr(*this), device_id, fanout = device.fanout](Error& error) {
        self->device_opened(device_id, *fanout, Error::copy(error));
    });
}

void CaptureDevices::device_opened(ByteString const& device_id, Fanout const& opened_for, ErrorOr<NonnullRefPtr<RecordStream>> stream_or_error)
{
    auto device = m_devices.get(device_id);
    // An entry that replaced the one this open was for has an open of its own in flight.
    if (!device.has_value() || device->fanout.ptr() != &opened_for)
        return;
    // Everyone left while it was opening; the stream goes with this call.
    if (device->subscribers.is_empty()) {
        m_devices.remove(device_id);
        return;
    }

    if (stream_or_error.is_error()) {
        device->error = stream_or_error.release_error();
    } else {
        device->stream = stream_or_error.release_value();
        device->specification = device->stream->sample_specification();
    }
    auto result = ready_result_for(*device);
    auto subscribers = device->subscribers;

    // Each callback may subscribe or unsubscribe in turn, so nothing of the device is held across them.
    for (auto subscriber_id : subscribers)
        report_ready(subscriber_id, result);

    // A device that would not open is forgotten, so the next subscriber tries again.
    if (result.is_error())
        m_devices.remove(device_id);
}

ErrorOr<SampleSpecification> CaptureDevices::ready_result_for(Device const& device)
{
    if (device.error.has_value())
        return Error::copy(device.error.value());
    return device.specification.value();
}

void CaptureDevices::report_ready(CaptureSubscriberId subscriber_id, ErrorOr<SampleSpecification> const& result)
{
    auto subscriber = m_subscribers.get(subscriber_id);
    if (!subscriber.has_value() || !subscriber->on_ready)
        return;
    auto on_ready = move(subscriber->on_ready);
    if (result.is_error())
        on_ready(subscriber_id, ErrorOr<SampleSpecification>(Error::copy(result.error())));
    else
        on_ready(subscriber_id, result);
}

void CaptureDevices::attach_ring(CaptureSubscriberId subscriber_id, SharedAudioFrameRing ring)
{
    auto subscriber = m_subscribers.get(subscriber_id);
    if (!subscriber.has_value())
        return;
    auto device = m_devices.get(subscriber->device_id);
    if (!device.has_value() || !device->specification.has_value())
        return;
    VERIFY(ring.sample_rate() == device->specification->sample_rate());
    VERIFY(ring.channel_count() == device->specification->channel_count());
    device->fanout->add_ring(subscriber_id, move(ring));
}

void CaptureDevices::unsubscribe(CaptureSubscriberId subscriber_id)
{
    auto subscriber = m_subscribers.take(subscriber_id);
    if (!subscriber.has_value())
        return;
    auto device = m_devices.get(subscriber->device_id);
    if (!device.has_value())
        return;
    device->fanout->remove_ring(subscriber_id);
    device->subscribers.remove_first_matching([&](auto id) { return id == subscriber_id; });
    // The stream stops in its destructor, after any delivery in flight has returned.
    if (device->subscribers.is_empty())
        m_devices.remove(subscriber->device_id);
}

void CaptureDevices::Fanout::deliver(ReadonlyBytes interleaved_samples)
{
    ReadonlySpan<float> samples { reinterpret_cast<float const*>(interleaved_samples.data()), interleaved_samples.size() / sizeof(float) };
    MutexLocker locker(m_mutex);
    for (auto& [subscriber_id, ring] : m_rings) {
        if (samples.size() % ring.channel_count() != 0)
            continue;
        // A ring whose reader has fallen behind loses what does not fit.
        (void)ring.try_push(samples);
    }
}

void CaptureDevices::Fanout::add_ring(CaptureSubscriberId subscriber_id, SharedAudioFrameRing ring)
{
    MutexLocker locker(m_mutex);
    m_rings.set(subscriber_id, move(ring));
}

void CaptureDevices::Fanout::remove_ring(CaptureSubscriberId subscriber_id)
{
    MutexLocker locker(m_mutex);
    m_rings.remove(subscriber_id);
}

}
