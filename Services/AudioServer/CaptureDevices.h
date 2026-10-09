/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibAudio/RecordStream.h>
#include <LibAudio/SharedAudioFrameRing.h>

namespace Audio {

using CaptureSubscriberId = u64;

// One RecordStream per open device, filling the ring of every subscriber recording from it. Control thread only.
class CaptureDevices : public RefCounted<CaptureDevices> {
public:
    using RecordStreamFactory = Function<NonnullRefPtr<RecordStream::CreatePromise>(SampleSpecification const& requested_specification, u32 fragment_size_bytes, StringView device_id, RecordStream::RecordCallback)>;

    static NonnullRefPtr<CaptureDevices> create(RecordStreamFactory);
    ~CaptureDevices();

    // Runs with the device's format once it is open, or with the error that kept it from opening; at once if known.
    using ReadyCallback = Function<void(CaptureSubscriberId, ErrorOr<SampleSpecification> const&)>;
    CaptureSubscriberId subscribe(StringView device_id, ReadyCallback);
    // The ring must be in the device format; it receives every frame captured from here on.
    void attach_ring(CaptureSubscriberId, SharedAudioFrameRing);
    void unsubscribe(CaptureSubscriberId);

    size_t open_device_count() const { return m_devices.size(); }

private:
    // Shared with the device thread, which may still be delivering while the device closes.
    class Fanout : public AtomicRefCounted<Fanout> {
    public:
        void deliver(ReadonlyBytes interleaved_samples);
        void add_ring(CaptureSubscriberId, SharedAudioFrameRing);
        void remove_ring(CaptureSubscriberId);

    private:
        Mutex m_mutex;
        HashMap<CaptureSubscriberId, SharedAudioFrameRing> m_rings;
    };

    struct Device {
        NonnullRefPtr<Fanout> fanout;
        RefPtr<RecordStream> stream;
        // Both unset while the stream is opening.
        Optional<SampleSpecification> specification;
        Optional<Error> error;
        Vector<CaptureSubscriberId> subscribers;
    };

    struct Subscriber {
        ByteString device_id;
        ReadyCallback on_ready;
    };

    explicit CaptureDevices(RecordStreamFactory);

    void open_device(ByteString const& device_id, Device&);
    void device_opened(ByteString const& device_id, Fanout const& opened_for, ErrorOr<NonnullRefPtr<RecordStream>>);
    void report_ready(CaptureSubscriberId, ErrorOr<SampleSpecification> const&);
    static ErrorOr<SampleSpecification> ready_result_for(Device const&);

    RecordStreamFactory m_factory;
    HashMap<ByteString, Device> m_devices;
    HashMap<CaptureSubscriberId, Subscriber> m_subscribers;
    CaptureSubscriberId m_next_subscriber_id { 1 };
};

}
