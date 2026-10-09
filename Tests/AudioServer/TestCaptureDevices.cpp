/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FakeRecordStream.h"
#include <AK/Vector.h>
#include <AudioServer/CaptureDevices.h>
#include <LibTest/TestCase.h>

static constexpr u32 SAMPLE_RATE = FakeRecordStream::SAMPLE_RATE;
static constexpr u32 CHANNEL_COUNT = FakeRecordStream::CHANNEL_COUNT;

struct CaptureFixture {
    Vector<Audio::SampleSpecification> requested_specifications;
    Vector<RefPtr<FakeRecordStream>> opened;
    // Opens of the "slow" device, left for the test to settle.
    Vector<NonnullRefPtr<Audio::RecordStream::CreatePromise>> pending_opens;
    NonnullRefPtr<Audio::CaptureDevices> devices;

    CaptureFixture()
        : devices(Audio::CaptureDevices::create([this](Audio::SampleSpecification const& specification, u32, StringView device_id, Audio::RecordStream::RecordCallback callback) {
            requested_specifications.append(specification);
            if (device_id == "missing"sv)
                return Audio::RecordStream::CreatePromise::rejected(Error::from_string_literal("No such device"));
            if (device_id == "slow"sv) {
                auto promise = Audio::RecordStream::CreatePromise::construct();
                pending_opens.append(promise);
                return promise;
            }
            auto stream = make_ref_counted<FakeRecordStream>(Audio::SampleSpecification(SAMPLE_RATE, Audio::ChannelMap::stereo()), move(callback));
            opened.append(stream);
            return Audio::RecordStream::CreatePromise::resolved(NonnullRefPtr<Audio::RecordStream>(stream));
        }))
    {
    }

    ~CaptureFixture()
    {
        opened.clear();
    }

    Audio::CaptureSubscriberId subscribe(StringView device_id, Optional<ErrorOr<Audio::SampleSpecification>>& result)
    {
        return devices->subscribe(device_id, [&](Audio::CaptureSubscriberId, ErrorOr<Audio::SampleSpecification> const& ready) {
            if (ready.is_error())
                result = Error::copy(ready.error());
            else
                result = ready.value();
        });
    }

    static Audio::SharedAudioFrameRing ring(size_t frame_capacity = 4096)
    {
        return MUST(Audio::SharedAudioFrameRing::create(SAMPLE_RATE, CHANNEL_COUNT, frame_capacity));
    }
};

TEST_CASE(one_stream_serves_every_subscriber_of_a_device)
{
    CaptureFixture fixture;
    Optional<ErrorOr<Audio::SampleSpecification>> first_result;
    Optional<ErrorOr<Audio::SampleSpecification>> second_result;
    auto first = fixture.subscribe("mic"sv, first_result);
    auto second = fixture.subscribe("mic"sv, second_result);

    EXPECT_EQ(fixture.opened.size(), 1u);
    EXPECT_EQ(fixture.devices->open_device_count(), 1u);
    EXPECT(first_result.has_value() && !first_result->is_error());
    EXPECT(second_result.has_value() && !second_result->is_error());
    EXPECT_EQ(second_result->value().sample_rate(), SAMPLE_RATE);

    auto first_ring = CaptureFixture::ring();
    auto second_ring = CaptureFixture::ring();
    fixture.devices->attach_ring(first, first_ring);
    fixture.devices->attach_ring(second, second_ring);

    fixture.opened[0]->capture(480, 0.5f);
    EXPECT_EQ(first_ring.frames_available(), 480u);
    EXPECT_EQ(second_ring.frames_available(), 480u);

    Vector<float> popped;
    popped.resize(480 * CHANNEL_COUNT);
    EXPECT_EQ(second_ring.try_pop(popped), 480u);
    EXPECT(all_of(popped, [](float sample) { return sample == 0.5f; }));
}

TEST_CASE(different_devices_open_different_streams)
{
    CaptureFixture fixture;
    Optional<ErrorOr<Audio::SampleSpecification>> result;
    (void)fixture.subscribe("mic"sv, result);
    (void)fixture.subscribe("line-in"sv, result);
    EXPECT_EQ(fixture.opened.size(), 2u);
    EXPECT_EQ(fixture.devices->open_device_count(), 2u);
}

TEST_CASE(the_device_closes_with_its_last_subscriber)
{
    CaptureFixture fixture;
    Optional<ErrorOr<Audio::SampleSpecification>> result;
    auto first = fixture.subscribe("mic"sv, result);
    auto second = fixture.subscribe("mic"sv, result);
    fixture.opened.clear();
    EXPECT_EQ(FakeRecordStream::live_count.load(), 1u);

    fixture.devices->unsubscribe(first);
    EXPECT_EQ(fixture.devices->open_device_count(), 1u);
    EXPECT_EQ(FakeRecordStream::live_count.load(), 1u);

    fixture.devices->unsubscribe(second);
    EXPECT_EQ(fixture.devices->open_device_count(), 0u);
    EXPECT_EQ(FakeRecordStream::live_count.load(), 0u);
}

TEST_CASE(a_device_that_fails_to_open_reports_the_error_and_is_retried_by_the_next_subscriber)
{
    CaptureFixture fixture;
    Optional<ErrorOr<Audio::SampleSpecification>> result;
    auto subscriber = fixture.subscribe("missing"sv, result);
    EXPECT(result.has_value() && result->is_error());
    EXPECT_EQ(fixture.devices->open_device_count(), 0u);
    fixture.devices->unsubscribe(subscriber);

    Optional<ErrorOr<Audio::SampleSpecification>> retry_result;
    (void)fixture.subscribe("missing"sv, retry_result);
    EXPECT_EQ(fixture.requested_specifications.size(), 2u);
    EXPECT(retry_result.has_value() && retry_result->is_error());
}

TEST_CASE(an_open_whose_entry_was_replaced_is_ignored)
{
    CaptureFixture fixture;
    Optional<ErrorOr<Audio::SampleSpecification>> first_result;
    auto first = fixture.subscribe("slow"sv, first_result);
    fixture.devices->unsubscribe(first);
    Optional<ErrorOr<Audio::SampleSpecification>> second_result;
    (void)fixture.subscribe("slow"sv, second_result);
    EXPECT_EQ(fixture.pending_opens.size(), 2u);

    // The first open resolves for an entry that is gone, so the second subscriber keeps waiting.
    auto stale_stream = make_ref_counted<FakeRecordStream>(Audio::SampleSpecification(SAMPLE_RATE, Audio::ChannelMap::stereo()), [](ReadonlyBytes, Audio::SampleSpecification const&) { });
    fixture.pending_opens[0]->resolve(NonnullRefPtr<Audio::RecordStream>(stale_stream));
    EXPECT(!second_result.has_value());
    EXPECT_EQ(fixture.devices->open_device_count(), 1u);

    // What the second subscriber hears is the fate of its own open.
    fixture.pending_opens[1]->reject(Error::from_string_literal("No such device"));
    EXPECT(second_result.has_value() && second_result->is_error());
    EXPECT_EQ(fixture.devices->open_device_count(), 0u);
}

TEST_CASE(a_subscriber_may_leave_from_inside_the_ready_callback)
{
    CaptureFixture fixture;

    // The registry forgets a device that would not open once it has told the subscribers, who may already be gone.
    bool reported = false;
    (void)fixture.devices->subscribe("missing"sv, [&](Audio::CaptureSubscriberId subscriber_id, ErrorOr<Audio::SampleSpecification> const& ready) {
        EXPECT(ready.is_error());
        fixture.devices->unsubscribe(subscriber_id);
        reported = true;
    });
    EXPECT(reported);
    EXPECT_EQ(fixture.devices->open_device_count(), 0u);

    // A device that opens closes again with the subscriber that leaves at once.
    (void)fixture.devices->subscribe("mic"sv, [&](Audio::CaptureSubscriberId subscriber_id, ErrorOr<Audio::SampleSpecification> const& ready) {
        EXPECT(!ready.is_error());
        fixture.devices->unsubscribe(subscriber_id);
    });
    EXPECT_EQ(fixture.devices->open_device_count(), 0u);
    fixture.opened.clear();
    EXPECT_EQ(FakeRecordStream::live_count.load(), 0u);
}

TEST_CASE(frames_that_do_not_fit_a_ring_are_dropped)
{
    CaptureFixture fixture;
    Optional<ErrorOr<Audio::SampleSpecification>> result;
    auto subscriber = fixture.subscribe("mic"sv, result);
    auto ring = CaptureFixture::ring(256);
    fixture.devices->attach_ring(subscriber, ring);

    fixture.opened[0]->capture(1000, 1.0f);
    EXPECT_EQ(ring.frames_available(), 256u);

    Vector<float> popped;
    popped.resize(256 * CHANNEL_COUNT);
    EXPECT_EQ(ring.try_pop(popped), 256u);
    fixture.opened[0]->capture(10, 1.0f);
    EXPECT_EQ(ring.frames_available(), 10u);
}

TEST_CASE(a_ring_detaches_with_its_subscriber_while_others_keep_receiving)
{
    CaptureFixture fixture;
    Optional<ErrorOr<Audio::SampleSpecification>> result;
    auto first = fixture.subscribe("mic"sv, result);
    auto second = fixture.subscribe("mic"sv, result);
    auto first_ring = CaptureFixture::ring();
    auto second_ring = CaptureFixture::ring();
    fixture.devices->attach_ring(first, first_ring);
    fixture.devices->attach_ring(second, second_ring);

    fixture.devices->unsubscribe(first);
    fixture.opened[0]->capture(100, 1.0f);
    EXPECT_EQ(first_ring.frames_available(), 0u);
    EXPECT_EQ(second_ring.frames_available(), 100u);
}
