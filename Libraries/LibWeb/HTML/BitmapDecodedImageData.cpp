/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGC/Heap.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakInlines.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/HTML/BitmapDecodedImageData.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(BitmapDecodedImageData);

// NB: A page can hold on to many more images than it shows, e.g. a map that keeps the image of every tile it has
//     uploaded to a WebGL texture. Once the frames of images that can be decoded again add up to more than this, the
//     ones that have not been used for a while are discarded, least recently used first.
static constexpr size_t discardable_frame_budget = 128 * MiB;
static constexpr AK::Duration frame_idle_time_before_discard = AK::Duration::from_seconds(5);
// NB: Frames that something else holds may be let go of at any time, which nothing tells us about, so they are looked
//     at again after this long.
static constexpr AK::Duration held_frame_recheck_delay = AK::Duration::from_seconds(30);
// NB: Discarding a small frame saves little, so the encoded data of such an image is not kept.
static constexpr size_t minimum_discardable_frame_size = 32 * KiB;
// NB: Decoding again can fail for a while, e.g. while the image decoder is restarted, so it is tried again after this,
//     with the delay doubling after each failure, until it has failed this many times.
static constexpr AK::Duration delay_before_decoding_again_after_failure = AK::Duration::from_seconds(1);
static constexpr u8 max_failed_attempts_to_decode_again = 5;

static size_t s_discardable_frame_bytes = 0;
static MonotonicTime s_next_discard_of_idle_frames;

static BitmapDecodedImageData::DiscardableFrameList& discardable_frames()
{
    static auto* list = new BitmapDecodedImageData::DiscardableFrameList;
    return *list;
}

ErrorOr<GC::Ref<BitmapDecodedImageData>> BitmapDecodedImageData::create(Vector<Frame>&& frames, size_t loop_count, bool animated, ByteBuffer encoded_data)
{
    (void)loop_count;
    (void)animated;
    if (frames.is_empty())
        return Error::from_string_literal("Bitmap image has no frames");
    // NB: Only the first frame is kept, so only an image that has just the one can be decoded again.
    if (frames.size() != 1 || frames[0].frame.bitmap().data_size() < minimum_discardable_frame_size) {
        encoded_data.clear();
    } else if (encoded_data.capacity() > encoded_data.size()) {
        // NB: The encoded data is kept for as long as the image, so it should not keep the room it was received into.
        //     Keeping that room is better than failing to create the image, though.
        if (auto copy = ByteBuffer::copy(encoded_data.bytes()); !copy.is_error())
            encoded_data = copy.release_value();
    }
    return GC::Heap::the().allocate<BitmapDecodedImageData>(move(frames[0].frame), move(encoded_data));
}

BitmapDecodedImageData::BitmapDecodedImageData(Gfx::DecodedImageFrame&& frame, ByteBuffer encoded_data)
    : m_frame(move(frame))
    , m_size(m_frame->size())
    , m_encoded_data(move(encoded_data))
    , m_frame_id(m_frame->id())
{
    if (!m_encoded_data.is_empty())
        did_decode_frame();
}

BitmapDecodedImageData::~BitmapDecodedImageData() = default;

void BitmapDecodedImageData::finalize()
{
    Base::finalize();
    // NB: Clients that die along with the image data cannot unregister from it, since their weak pointers to it are
    //     cleared first. So the image data leaves the list now, before a discard can look at those clients, instead of
    //     when it is swept.
    leave_discardable_frames();
}

void BitmapDecodedImageData::leave_discardable_frames() const
{
    if (!m_discardable_frame_list_node.is_in_list())
        return;
    s_discardable_frame_bytes -= m_frame->bitmap().data_size();
    m_discardable_frame_list_node.remove();
}

void BitmapDecodedImageData::did_decode_frame() const
{
    VERIFY(m_frame.has_value());
    VERIFY(!m_discardable_frame_list_node.is_in_list());
    discardable_frames().append(const_cast<BitmapDecodedImageData&>(*this));
    s_discardable_frame_bytes += m_frame->bitmap().data_size();
    if (s_discardable_frame_bytes > discardable_frame_budget)
        schedule_discard_of_idle_frames(frame_idle_time_before_discard);
}

void BitmapDecodedImageData::discard_frame() const
{
    VERIFY(m_discardable_frame_list_node.is_in_list());
    leave_discardable_frames();
    m_frame.clear();
}

void BitmapDecodedImageData::schedule_discard_of_idle_frames(AK::Duration delay)
{
    static auto* timer = &Core::Timer::create_single_shot(frame_idle_time_before_discard, [] {
        discard_idle_frames();
    }).leak_ref();
    // NB: A discard that is due sooner than the one already scheduled takes its place.
    auto deadline = MonotonicTime::now_coarse() + delay;
    if (timer->is_active() && s_next_discard_of_idle_frames <= deadline)
        return;
    s_next_discard_of_idle_frames = deadline;
    timer->start(delay);
}

void BitmapDecodedImageData::discard_idle_frames()
{
    auto now = MonotonicTime::now_coarse();
    bool has_frames_that_become_idle_later = false;
    bool has_held_frames = false;
    for (auto it = discardable_frames().begin(); it != discardable_frames().end() && s_discardable_frame_bytes > discardable_frame_budget;) {
        auto& image_data = *it;
        ++it;
        // NB: The list is in order of last use, so once a frame is in use, all the frames after it are as well.
        if (now - image_data.m_last_used < frame_idle_time_before_discard) {
            has_frames_that_become_idle_later = true;
            break;
        }
        // NB: The frame of an image that may be painted is needed by the next paint, which would decode it again. A
        //     bitmap that something else still holds, e.g. a display list or a pending texture upload, would stay in
        //     memory. Discarding either would save nothing.
        if (image_data.has_clients_that_may_render() || image_data.m_frame->bitmap().ref_count() > 1) {
            has_held_frames = true;
            continue;
        }
        image_data.discard_frame();
    }
    if (s_discardable_frame_bytes <= discardable_frame_budget)
        return;
    if (has_frames_that_become_idle_later)
        schedule_discard_of_idle_frames(frame_idle_time_before_discard);
    else if (has_held_frames)
        schedule_discard_of_idle_frames(held_frame_recheck_delay);
}

size_t BitmapDecodedImageData::discard_all_discardable_frames()
{
    size_t count = 0;
    auto& list = discardable_frames();
    while (!list.is_empty()) {
        list.first()->discard_frame();
        ++count;
    }
    return count;
}

Optional<Gfx::DecodedImageFrame> BitmapDecodedImageData::ensure_frame() const
{
    if (m_encoded_data.is_empty())
        return m_frame;

    auto now = MonotonicTime::now_coarse();
    m_last_used = now;

    if (m_frame.has_value()) {
        // NB: Move the frame to the end of the list, which keeps the list in order of last use.
        auto& self = const_cast<BitmapDecodedImageData&>(*this);
        m_discardable_frame_list_node.remove();
        discardable_frames().append(self);
        return m_frame;
    }

    // NB: A decode that failed is not tried again until the retry timer fires, so that a frame that cannot be had for a
    //     while does not cost a synchronous decode request on each use.
    if (m_retry_decoding_again_timer && m_retry_decoding_again_timer->is_active())
        return {};

    auto decoded_or_error = Platform::ImageCodecPlugin::the().decode_first_frame_synchronously(m_encoded_data);
    if (decoded_or_error.is_error()) {
        dbgln("BitmapDecodedImageData: Could not decode discarded frame again: {}", decoded_or_error.error());
        did_fail_to_decode_again();
        return {};
    }
    auto decoded = decoded_or_error.release_value();

    // NB: The decoder parses untrusted data, so a frame of another size than the first decode gave is not trusted.
    if (decoded.bitmap->size() != m_size) {
        dbgln("BitmapDecodedImageData: Frame decoded again has size {}, expected {}", decoded.bitmap->size(), m_size);
        give_up_decoding_again();
        return {};
    }

    m_failed_attempts_to_decode_again = 0;
    // NB: The pixels are those of the discarded frame, so the frame keeps its id, by which caches may still hold it.
    m_frame = Gfx::DecodedImageFrame::with_id(m_frame_id, move(decoded.bitmap), move(decoded.color_space));
    did_decode_frame();
    return m_frame;
}

void BitmapDecodedImageData::did_fail_to_decode_again() const
{
    if (++m_failed_attempts_to_decode_again >= max_failed_attempts_to_decode_again) {
        give_up_decoding_again();
        return;
    }
    auto delay = AK::Duration::from_milliseconds(delay_before_decoding_again_after_failure.to_milliseconds() << (m_failed_attempts_to_decode_again - 1));
    // NB: The image went without its frame, so tell its clients once it may be decoded again, which makes a client
    //     that paints it paint again.
    if (!m_retry_decoding_again_timer) {
        m_retry_decoding_again_timer = Core::Timer::create_single_shot(delay, GC::weak_callback(const_cast<BitmapDecodedImageData&>(*this), [](auto& self) {
            self.notify_clients_did_update();
        }));
    }
    m_retry_decoding_again_timer->start(delay);
}

void BitmapDecodedImageData::give_up_decoding_again() const
{
    // NB: Without its encoded data, the image is one whose frame cannot be decoded again, and it no longer keeps data it
    //     cannot use.
    m_encoded_data.clear();
    if (m_retry_decoding_again_timer)
        m_retry_decoding_again_timer->stop();
}

size_t BitmapDecodedImageData::external_memory_size() const
{
    size_t size = m_encoded_data.capacity();
    if (m_frame.has_value())
        size = JS::saturating_add_external_memory_size(size, m_frame->bitmap().data_size());
    return size;
}

Optional<Gfx::DecodedImageFrame> BitmapDecodedImageData::current_frame(Gfx::IntSize) const
{
    return ensure_frame();
}

Optional<Gfx::DecodedImageFrame> BitmapDecodedImageData::default_frame(Gfx::IntSize) const
{
    return ensure_frame();
}

Optional<CSSPixels> BitmapDecodedImageData::intrinsic_width() const
{
    return m_size.width();
}

Optional<CSSPixels> BitmapDecodedImageData::intrinsic_height() const
{
    return m_size.height();
}

Optional<CSSPixelFraction> BitmapDecodedImageData::intrinsic_aspect_ratio() const
{
    return CSSPixels(m_size.width()) / CSSPixels(m_size.height());
}

Optional<Gfx::Color> BitmapDecodedImageData::color_if_single_pixel_bitmap() const
{
    if (m_size.width() != 1 || m_size.height() != 1)
        return {};
    auto frame = ensure_frame();
    if (!frame.has_value())
        return {};
    return frame->bitmap().get_pixel(0, 0);
}

Optional<Painting::ImagePaint> BitmapDecodedImageData::image_paint(Painting::ImagePaintRequest const&) const
{
    auto frame = ensure_frame();
    if (!frame.has_value())
        return {};
    return Painting::ImagePaint { Painting::ImagePaint::DecodedFrame { .frame = frame.release_value(), .natural_size = m_size } };
}

}
