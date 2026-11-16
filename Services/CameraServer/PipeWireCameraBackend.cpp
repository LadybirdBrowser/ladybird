/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/QuickSort.h>
#include <CameraServer/CameraBackend.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wc99-extensions"
#pragma GCC diagnostic ignored "-Wgnu-statement-expression"
#pragma GCC diagnostic ignored "-Wextra-semi"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#include <dlfcn.h>
#include <link.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>

// The cameras as PipeWire has them: every node of class Video/Source, whoever provides it (V4L2, libcamera, or another
// program). This is also how a camera comes through the desktop's camera portal, which hands over a PipeWire
// connection that shows only the cameras the user allowed.
// FIXME: Ask the camera portal for the connection where there is one, so that this works where the daemon's own socket
//        is out of reach (Flatpak, e.g.).

namespace CameraServer {

namespace {

constexpr auto DEVICE_ID_PREFIX = "pipewire:"sv;

// One way a camera can be asked to work. A camera that takes any size in a range has the range's ends here, and one
// that takes only certain sizes has each as a mode of its own.
struct Mode {
    PixelFormat format { PixelFormat::I420 };
    u32 minimum_width { 0 };
    u32 minimum_height { 0 };
    u32 maximum_width { 0 };
    u32 maximum_height { 0 };
    // A camera that does a range of sizes may do only every so many pixels of it.
    u32 width_step { 1 };
    u32 height_step { 1 };
    u32 maximum_frame_rate { 0 };
};

Optional<PixelFormat> pixel_format_from(u32 spa_format)
{
    switch (spa_format) {
    case SPA_VIDEO_FORMAT_I420:
        return PixelFormat::I420;
    case SPA_VIDEO_FORMAT_NV12:
        return PixelFormat::NV12;
    case SPA_VIDEO_FORMAT_YUY2:
        return PixelFormat::YUY2;
    default:
        // FIXME: Many cameras give their larger sizes only as Motion JPEG.
        return {};
    }
}

u32 spa_format_from(PixelFormat format)
{
    switch (format) {
    case PixelFormat::I420:
        return SPA_VIDEO_FORMAT_I420;
    case PixelFormat::NV12:
        return SPA_VIDEO_FORMAT_NV12;
    case PixelFormat::YUY2:
        return SPA_VIDEO_FORMAT_YUY2;
    }
    VERIFY_NOT_REACHED();
}

// The values a property of a format can take: the one value, or the alternatives, or the ends of a range (with the
// default first in both of the latter).
template<typename T>
struct Choices {
    u32 kind { SPA_CHOICE_None };
    ReadonlySpan<T> values;
};

template<typename T>
Choices<T> choices_of(spa_pod const* format, u32 key, u32 type)
{
    auto const* property = spa_pod_find_prop(format, nullptr, key);
    if (!property)
        return {};
    u32 count = 0;
    u32 kind = SPA_CHOICE_None;
    auto const* values = spa_pod_get_values(&property->value, &count, &kind);
    if (!values || values->type != type || count == 0)
        return {};
    return { .kind = kind, .values = { static_cast<T const*>(SPA_POD_BODY_CONST(values)), count } };
}

void append_modes_of(spa_pod const* format, Vector<Mode>& modes)
{
    u32 media_type = 0;
    u32 media_subtype = 0;
    if (spa_format_parse(format, &media_type, &media_subtype) < 0)
        return;
    if (media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    auto formats = choices_of<u32>(format, SPA_FORMAT_VIDEO_format, SPA_TYPE_Id);
    auto sizes = choices_of<spa_rectangle>(format, SPA_FORMAT_VIDEO_size, SPA_TYPE_Rectangle);
    auto frame_rates = choices_of<spa_fraction>(format, SPA_FORMAT_VIDEO_framerate, SPA_TYPE_Fraction);
    if (formats.values.is_empty() || sizes.values.is_empty())
        return;

    u32 maximum_frame_rate = 0;
    for (auto const& rate : frame_rates.values) {
        if (rate.denom != 0)
            maximum_frame_rate = max(maximum_frame_rate, rate.num / rate.denom);
    }

    for (auto spa_format : formats.values) {
        auto pixel_format = pixel_format_from(spa_format);
        if (!pixel_format.has_value())
            continue;
        auto add = [&](spa_rectangle const& minimum, spa_rectangle const& maximum, spa_rectangle step = { 1, 1 }) {
            if (minimum.width == 0 || minimum.height == 0 || minimum.width > Camera::MAXIMUM_PICTURE_DIMENSION || minimum.height > Camera::MAXIMUM_PICTURE_DIMENSION)
                return;
            modes.append(Mode {
                .format = *pixel_format,
                .minimum_width = minimum.width,
                .minimum_height = minimum.height,
                .maximum_width = min(max(minimum.width, maximum.width), Camera::MAXIMUM_PICTURE_DIMENSION),
                .maximum_height = min(max(minimum.height, maximum.height), Camera::MAXIMUM_PICTURE_DIMENSION),
                .width_step = max(step.width, 1u),
                .height_step = max(step.height, 1u),
                .maximum_frame_rate = maximum_frame_rate,
            });
        };
        // A range is its default, minimum and maximum, and a stepped one its step after those.
        if (sizes.kind == SPA_CHOICE_Step && sizes.values.size() >= 4) {
            add(sizes.values[1], sizes.values[2], sizes.values[3]);
            continue;
        }
        if ((sizes.kind == SPA_CHOICE_Range || sizes.kind == SPA_CHOICE_Step) && sizes.values.size() >= 3) {
            add(sizes.values[1], sizes.values[2]);
            continue;
        }
        // The default of a list of alternatives is one of them too.
        for (size_t i = sizes.kind == SPA_CHOICE_Enum && sizes.values.size() > 1 ? 1 : 0; i < sizes.values.size(); ++i)
            add(sizes.values[i], sizes.values[i]);
    }
}

struct Selection {
    PixelFormat format { PixelFormat::I420 };
    u32 width { 0 };
    u32 height { 0 };
};

// The size the camera does that is closest to the one asked for: the smallest that covers it, or else the largest there
// is. Where that is larger than what was asked for, the pictures are scaled down on their way to the client. A
// dimension that wasn't asked for need not be covered, and its default is only what a range of sizes is gone into by.
Optional<Selection> select_mode(Vector<Mode> const& modes, u32 requested_width, u32 requested_height)
{
    auto wanted_width = requested_width ? requested_width : DEFAULT_WIDTH;
    auto wanted_height = requested_height ? requested_height : DEFAULT_HEIGHT;
    if (!requested_width && !requested_height) {
        requested_width = DEFAULT_WIDTH;
        requested_height = DEFAULT_HEIGHT;
    }
    Optional<Selection> best;
    auto is_better = [&](Selection const& candidate) {
        if (!best.has_value())
            return true;
        auto covers = [&](Selection const& selection) { return selection.width >= requested_width && selection.height >= requested_height; };
        auto area = [](Selection const& selection) { return static_cast<u64>(selection.width) * selection.height; };
        if (covers(candidate) != covers(*best))
            return covers(candidate);
        return covers(candidate) ? area(candidate) < area(*best) : area(candidate) > area(*best);
    };
    // The size the mode does that covers the one wanted, or else the largest it does. SPA takes a size in a stepped
    // range to be a multiple of the step, which a mode whose range has none of does no size of.
    auto size_in = [](u32 wanted, u32 minimum, u32 maximum, u32 step) -> Optional<u32> {
        u64 lowest = (static_cast<u64>(minimum) + step - 1) / step * step;
        u64 highest = static_cast<u64>(maximum) / step * step;
        if (lowest > highest)
            return {};
        auto size = clamp(static_cast<u64>(wanted), lowest, highest);
        return static_cast<u32>(min((size + step - 1) / step * step, highest));
    };
    for (auto const& mode : modes) {
        auto width = size_in(wanted_width, mode.minimum_width, mode.maximum_width, mode.width_step);
        auto height = size_in(wanted_height, mode.minimum_height, mode.maximum_height, mode.height_step);
        if (!width.has_value() || !height.has_value())
            continue;
        Selection candidate {
            .format = mode.format,
            .width = *width,
            .height = *height,
        };
        if (is_better(candidate))
            best = candidate;
    }
    return best;
}

class PipeWireCameras;

struct Node {
    AK_ALLOC_WITH_KMALLOC;

    PipeWireCameras* cameras { nullptr };
    u32 id { 0 };
    ByteString name;
    ByteString description;
    pw_proxy* proxy { nullptr };
    spa_hook listener {};
    Vector<Mode> modes;
};

class PipeWireStream final : public CameraStream {
public:
    PipeWireStream(pw_thread_loop* loop, NonnullRefPtr<CameraStreamListener> listener)
        : m_loop(loop)
        , m_listener(move(listener))
        , m_started_at(MonotonicTime::now())
    {
    }

    // With the loop locked.
    ErrorOr<void> connect(pw_core* core, Node const& node, Camera::StreamRequest request)
    {
        static constexpr u32 MINIMUM_FRAME_RATE = 1;
        static constexpr u32 MAXIMUM_FRAME_RATE = 120;

        auto wanted_frame_rate = clamp(request.frame_rate ? request.frame_rate : DEFAULT_FRAME_RATE, MINIMUM_FRAME_RATE, MAXIMUM_FRAME_RATE);
        auto selection = select_mode(node.modes, request.width, request.height);
        if (!selection.has_value())
            return Error::from_string_literal("The camera gives no pictures in a format that can be used");

        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Camera",
            PW_KEY_TARGET_OBJECT, node.name.characters(),
            // The camera that was chosen and none other, even when it went away before the stream reached it.
            "node.dont-fallback", "true",
            nullptr);
        m_stream = pw_stream_new(core, "Ladybird camera", properties);
        if (!m_stream)
            return Error::from_errno(errno);

        static constexpr pw_stream_events stream_events = [] {
            pw_stream_events events {};
            events.version = PW_VERSION_STREAM_EVENTS;
            events.state_changed = [](void* data, pw_stream_state, pw_stream_state state, char const* error) {
                static_cast<PipeWireStream*>(data)->state_changed(state, error);
            };
            events.param_changed = [](void* data, u32 id, spa_pod const* param) {
                static_cast<PipeWireStream*>(data)->param_changed(id, param);
            };
            events.process = [](void* data) {
                static_cast<PipeWireStream*>(data)->process();
            };
            return events;
        }();
        pw_stream_add_listener(m_stream, &m_stream_listener, &stream_events, this);

        // Request an exact size; a range would let the camera pick its first listed size, usually its largest.
        u8 buffer[1024];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, buffer, sizeof(buffer));
        spa_rectangle size { selection->width, selection->height };
        spa_fraction frame_rate { wanted_frame_rate, 1 };
        spa_fraction minimum_frame_rate { MINIMUM_FRAME_RATE, 1 };
        spa_fraction maximum_frame_rate { MAXIMUM_FRAME_RATE, 1 };
        spa_pod const* parameters[1];
        parameters[0] = static_cast<spa_pod const*>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format_from(selection->format)),
            SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&frame_rate, &minimum_frame_rate, &maximum_frame_rate)));

        // A stream whose camera goes away ends, where it would otherwise be moved to whichever camera is left. And the
        // pictures come as the camera gives them, in one of the formats asked for, which are the ones converted here.
        auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT | PW_STREAM_FLAG_NO_CONVERT);
        if (auto result = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, parameters, 1); result < 0)
            return Error::from_errno(-result);
        return {};
    }

    virtual ~PipeWireStream() override
    {
        // The loop's thread is in none of the callbacks while this holds its lock, and in none of them ever after.
        pw_thread_loop_lock(m_loop);
        if (m_stream) {
            spa_hook_remove(&m_stream_listener);
            pw_stream_destroy(m_stream);
        }
        pw_thread_loop_unlock(m_loop);
    }

    void daemon_lost()
    {
        m_listener->ended();
    }

private:
    void state_changed(pw_stream_state state, char const* error)
    {
        if (state == PW_STREAM_STATE_ERROR) {
            warnln("CameraServer: The camera stream failed: {}", error ? error : "unknown error");
            m_listener->ended();
        } else if (state == PW_STREAM_STATE_UNCONNECTED) {
            m_listener->ended();
        }
    }

    void param_changed(u32 id, spa_pod const* param)
    {
        if (id != SPA_PARAM_Format || !param)
            return;

        spa_video_info_raw info {};
        if (spa_format_video_raw_parse(param, &info) < 0) {
            m_listener->ended();
            return;
        }
        auto format = pixel_format_from(info.format);
        if (!format.has_value()) {
            m_listener->ended();
            return;
        }
        // FIXME: Carry the color matrix and range the camera says its pictures have (info.color_matrix and
        //        info.color_range) along with them. Everything downstream takes them for BT.601 in limited range.
        m_format = *format;
        m_width = info.size.width;
        m_height = info.size.height;
        auto frame_rate = info.framerate.denom != 0 ? info.framerate.num / info.framerate.denom : 0;
        if (frame_rate == 0 && info.max_framerate.denom != 0)
            frame_rate = info.max_framerate.num / info.max_framerate.denom;
        m_listener->format_decided(m_width, m_height, frame_rate ? frame_rate : DEFAULT_FRAME_RATE);

        // Pictures in memory this process can read, which rules out the buffers of a GPU, with all of their planes in one
        // block of it.
        u8 buffer[256];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, buffer, sizeof(buffer));
        spa_pod const* parameters[1];
        parameters[0] = static_cast<spa_pod const*>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
        pw_stream_update_params(m_stream, parameters, 1);
    }

    void process()
    {
        // Only the newest picture is of interest; the older go back unread.
        pw_buffer* newest = nullptr;
        while (auto* next = pw_stream_dequeue_buffer(m_stream)) {
            if (newest)
                pw_stream_queue_buffer(m_stream, newest);
            newest = next;
        }
        if (!newest)
            return;

        auto const* buffer = newest->buffer;
        // NB: The buffers are asked for with one block, which a source that can't give them in one refuses to connect over.
        if (m_width != 0 && buffer->n_datas == 1 && buffer->datas[0].data && buffer->datas[0].chunk) {
            auto const& data = buffer->datas[0];
            auto const& chunk = *data.chunk;
            // The chunk's offset is to be taken modulo the data's maxsize, and its frame has to fit in what is left.
            auto offset = data.maxsize != 0 ? chunk.offset % data.maxsize : 0;
            auto size = min(chunk.size, data.maxsize - offset);
            auto bytes_per_row = chunk.stride > 0 ? static_cast<size_t>(chunk.stride) : static_cast<size_t>(m_format == PixelFormat::YUY2 ? m_width * 2 : m_width);
            if (!(chunk.flags & SPA_CHUNK_FLAG_CORRUPTED) && size > 0) {
                m_listener->frame_arrived(RawFrame {
                    .format = m_format,
                    .width = m_width,
                    .height = m_height,
                    .data = { static_cast<u8 const*>(data.data) + offset, size },
                    .bytes_per_row = bytes_per_row,
                    .timestamp = MonotonicTime::now() - m_started_at,
                });
            }
        }
        pw_stream_queue_buffer(m_stream, newest);
    }

    pw_thread_loop* m_loop { nullptr };
    NonnullRefPtr<CameraStreamListener> m_listener;
    MonotonicTime m_started_at;
    pw_stream* m_stream { nullptr };
    spa_hook m_stream_listener {};
    PixelFormat m_format { PixelFormat::I420 };
    u32 m_width { 0 };
    u32 m_height { 0 };
};

class PipeWireCameras final : public CameraBackend {
public:
    PipeWireCameras()
    {
        pw_init(nullptr, nullptr);

        // A context loads modules and starts a thread, but sandboxing must precede the second thread and prevents
        // loading. Use a temporary context to preload modules, retaining them for the context created after sandboxing.
        auto* loop = pw_main_loop_new(nullptr);
        auto* context = loop ? pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0) : nullptr;
        if (!context)
            warnln("CameraServer: Unable to load PipeWire's modules: {}", strerror(errno));
        // A stream of video is wrapped in a converter, which is a plugin that is first loaded when a stream connects.
        Vector<spa_handle*> plugins;
        if (context) {
            for (auto const* factory : { SPA_NAME_VIDEO_CONVERT, SPA_NAME_VIDEO_CONVERT_DUMMY }) {
                if (auto* plugin = pw_context_load_spa_handle(context, factory, nullptr))
                    plugins.append(plugin);
            }
        }
        keep_loaded_libraries();
        for (auto* plugin : plugins)
            pw_unload_spa_handle(plugin);
        if (context)
            pw_context_destroy(context);
        if (loop)
            pw_main_loop_destroy(loop);
    }

    virtual void watch_devices(Function<void(Vector<Camera::DeviceInfo>)> on_device_list) override
    {
        m_owner_loop = &Core::EventLoop::current();
        m_on_device_list = move(on_device_list);
        // Without the daemon there are no cameras, and that is a list too, until the daemon starts.
        if (auto result = ensure_connected(); result.is_error()) {
            warnln("CameraServer: Unable to reach PipeWire: {}", result.error());
            m_on_device_list({});
            schedule_reconnect();
        }
    }

    virtual ErrorOr<NonnullOwnPtr<CameraStream>> open(ByteString const& device_id, Camera::StreamRequest request, NonnullRefPtr<CameraStreamListener> listener) override
    {
        TRY(ensure_connected());

        auto stream = make<PipeWireStream>(m_loop, move(listener));
        pw_thread_loop_lock(m_loop);
        auto result = [&]() -> ErrorOr<void> {
            if (!m_core)
                return Error::from_string_literal("PipeWire is gone");
            auto const* node = find_node(device_id);
            if (!node)
                return Error::from_string_literal("No such camera");
            TRY(stream->connect(m_core, *node, request));
            m_streams.append(stream.ptr());
            return {};
        }();
        pw_thread_loop_unlock(m_loop);
        TRY(result);
        return adopt_own<CameraStream>(*new TrackedStream(*this, move(stream)));
    }

private:
    // Keeps the list of open streams, which hear when the daemon goes.
    class TrackedStream final : public CameraStream {
    public:
        TrackedStream(PipeWireCameras& cameras, NonnullOwnPtr<PipeWireStream> stream)
            : m_cameras(cameras)
            , m_stream(move(stream))
        {
        }

        virtual ~TrackedStream() override
        {
            pw_thread_loop_lock(m_cameras.m_loop);
            m_cameras.m_streams.remove_first_matching([&](auto* stream) { return stream == m_stream.ptr(); });
            pw_thread_loop_unlock(m_cameras.m_loop);
        }

    private:
        PipeWireCameras& m_cameras;
        NonnullOwnPtr<PipeWireStream> m_stream;
    };

    // Retain library references for the process lifetime so their original owners cannot unload them.
    static void keep_loaded_libraries()
    {
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {
            if (info->dlpi_name && info->dlpi_name[0] == '/')
                (void)dlopen(info->dlpi_name, RTLD_NOW | RTLD_NOLOAD);
            return 0;
        },
            nullptr);
    }

    ErrorOr<void> ensure_connected()
    {
        if (m_is_connected)
            return {};

        if (!m_context) {
            m_loop = pw_thread_loop_new("Camera", nullptr);
            if (!m_loop)
                return Error::from_errno(errno);
            // Locked memory and real-time priority are unnecessary here, and the sandbox permits neither.
            auto* properties = pw_properties_new(
                "mem.allow-mlock", "false",
                "module.rt", "false",
                nullptr);
            m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), properties, 0);
            if (!m_context)
                return Error::from_errno(errno);
        }

        if (!m_is_running) {
            if (auto result = pw_thread_loop_start(m_loop); result < 0)
                return Error::from_errno(-result);
            m_is_running = true;
        }

        pw_thread_loop_lock(m_loop);
        auto result = [&]() -> ErrorOr<void> {
            // A daemon that went away leaves its core behind, which goes once the streams that were open on it have.
            if (m_core) {
                if (!m_streams.is_empty())
                    return Error::from_string_literal("PipeWire is gone");
                disconnect_while_locked();
            }
            m_core = pw_context_connect(m_context, nullptr, 0);
            if (!m_core)
                return Error::from_errno(errno);

            static constexpr pw_core_events core_events = [] {
                pw_core_events events {};
                events.version = PW_VERSION_CORE_EVENTS;
                events.done = [](void* data, u32 id, int sequence) {
                    static_cast<PipeWireCameras*>(data)->core_done(id, sequence);
                };
                events.error = [](void* data, u32 id, int, int result, char const* message) {
                    static_cast<PipeWireCameras*>(data)->core_error(id, result, message);
                };
                return events;
            }();
            pw_core_add_listener(m_core, &m_core_listener, &core_events, this);

            m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
            if (!m_registry) {
                auto error = Error::from_errno(errno);
                // Remove the listener before the next attempt attaches it to another core.
                disconnect_while_locked();
                return error;
            }
            static constexpr pw_registry_events registry_events = [] {
                pw_registry_events events {};
                events.version = PW_VERSION_REGISTRY_EVENTS;
                events.global = [](void* data, u32 id, u32, char const* type, u32, spa_dict const* properties) {
                    static_cast<PipeWireCameras*>(data)->global_added(id, type, properties);
                };
                events.global_remove = [](void* data, u32 id) {
                    static_cast<PipeWireCameras*>(data)->global_removed(id);
                };
                return events;
            }();
            pw_registry_add_listener(m_registry, &m_registry_listener, &registry_events, this);

            // Wait for the complete initial device list.
            m_has_initial_list = false;
            m_initial_sync = pw_core_sync(m_core, PW_ID_CORE, 0);
            m_is_connected = true;
            return {};
        }();
        pw_thread_loop_unlock(m_loop);
        return result;
    }

    // With the loop locked.
    void disconnect_while_locked()
    {
        clear_nodes_while_locked();
        if (m_registry) {
            spa_hook_remove(&m_registry_listener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
            m_registry = nullptr;
        }
        if (m_core) {
            spa_hook_remove(&m_core_listener);
            pw_core_disconnect(m_core);
            m_core = nullptr;
        }
        m_is_connected = false;
        m_report_sync = -1;
    }

    // On the owner's loop: a daemon that went away may come back, as one does when the session restarts it.
    void schedule_reconnect()
    {
        static constexpr auto MAXIMUM_RECONNECT_DELAY = AK::Duration::from_seconds(30);
        if (m_reconnect_timer && m_reconnect_timer->is_active())
            return;
        m_reconnect_timer = Core::Timer::create_single_shot(m_reconnect_delay, [this] {
            if (m_is_connected)
                return;
            if (auto result = ensure_connected(); result.is_error()) {
                m_reconnect_delay = min(m_reconnect_delay + m_reconnect_delay, MAXIMUM_RECONNECT_DELAY);
                schedule_reconnect();
                return;
            }
            m_reconnect_delay = INITIAL_RECONNECT_DELAY;
        });
        m_reconnect_timer->start();
    }

    // Everything from here on runs on the loop's thread, or with the loop locked.

    void clear_nodes_while_locked()
    {
        for (auto& [id, node] : m_nodes) {
            if (node->proxy) {
                spa_hook_remove(&node->listener);
                pw_proxy_destroy(node->proxy);
            }
        }
        m_nodes.clear();
    }

    Node const* find_node(ByteString const& device_id) const
    {
        if (device_id.is_empty())
            return default_node();
        if (!device_id.starts_with(DEVICE_ID_PREFIX))
            return nullptr;
        auto name = device_id.substring_view(DEVICE_ID_PREFIX.length());
        for (auto const& [id, node] : m_nodes) {
            if (node->name == name)
                return node.ptr();
        }
        return nullptr;
    }

    // The camera with the lowest ID of those that give pictures in a format that can be used.
    // FIXME: Follow the session manager's default (default.video.source in its metadata).
    Node const* default_node() const
    {
        Node const* first = nullptr;
        for (auto const& [id, node] : m_nodes) {
            if (node->modes.is_empty())
                continue;
            if (!first || id < first->id)
                first = node.ptr();
        }
        return first;
    }

    void global_added(u32 id, char const* type, spa_dict const* properties)
    {
        if (!properties || !type || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
            return;
        auto const* media_class = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
        auto const* name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
        if (!media_class || !name || strcmp(media_class, "Video/Source") != 0)
            return;
        auto const* description = spa_dict_lookup(properties, PW_KEY_NODE_DESCRIPTION);
        if (!description)
            description = spa_dict_lookup(properties, PW_KEY_NODE_NICK);

        auto node = make<Node>();
        node->cameras = this;
        node->id = id;
        node->name = name;
        node->description = description ? description : name;

        // The sizes the camera does are asked for now, so that they are known by the time somebody opens it.
        node->proxy = static_cast<pw_proxy*>(pw_registry_bind(m_registry, id, type, PW_VERSION_NODE, 0));
        if (node->proxy) {
            static constexpr pw_node_events node_events = [] {
                pw_node_events events {};
                events.version = PW_VERSION_NODE_EVENTS;
                events.param = [](void* data, int, u32 id, u32, u32, spa_pod const* param) {
                    if (id == SPA_PARAM_EnumFormat && param)
                        append_modes_of(param, static_cast<Node*>(data)->modes);
                };
                return events;
            }();
            pw_node_add_listener(reinterpret_cast<pw_node*>(node->proxy), &node->listener, &node_events, node.ptr());
            pw_node_enum_params(reinterpret_cast<pw_node*>(node->proxy), 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, nullptr);
        }
        m_nodes.set(id, move(node));
        devices_changed();
    }

    void global_removed(u32 id)
    {
        auto node = m_nodes.take(id);
        if (!node.has_value())
            return;
        if (node.value()->proxy) {
            spa_hook_remove(&node.value()->listener);
            pw_proxy_destroy(node.value()->proxy);
        }
        devices_changed();
    }

    void core_done(u32 id, int sequence)
    {
        if (id != PW_ID_CORE)
            return;
        if (sequence == m_initial_sync && !m_has_initial_list) {
            m_has_initial_list = true;
            devices_changed();
        } else if (sequence == m_report_sync) {
            report_devices();
        }
    }

    void core_error(u32 id, int result, char const* message)
    {
        if (id != PW_ID_CORE || result != -EPIPE)
            return;
        warnln("CameraServer: PipeWire is gone: {}", message ? message : spa_strerror(result));
        m_is_connected = false;
        clear_nodes_while_locked();
        for (auto* stream : m_streams)
            stream->daemon_lost();
        m_has_initial_list = true;
        report_devices();
        m_owner_loop->deferred_invoke([this] { schedule_reconnect(); });
    }

    void devices_changed()
    {
        // The first list is the one that has every camera in it.
        if (!m_has_initial_list)
            return;
        // Wait for the requested camera sizes before announcing devices: listeners may open them immediately.
        m_report_sync = pw_core_sync(m_core, PW_ID_CORE, 0);
    }

    void report_devices()
    {
        Vector<Node const*> nodes;
        for (auto const& [id, node] : m_nodes)
            nodes.append(node.ptr());
        quick_sort(nodes, [](auto const* a, auto const* b) { return a->id < b->id; });

        auto const* default_camera = default_node();
        Vector<Camera::DeviceInfo> devices;
        for (auto const* node : nodes) {
            devices.append(Camera::DeviceInfo {
                .dom_device_id = ByteString::formatted("{}{}", DEVICE_ID_PREFIX, node->name),
                .label = node->description,
                .group_id = {},
                .is_default = node == default_camera,
            });
        }
        m_owner_loop->deferred_invoke([this, devices = move(devices)]() mutable {
            m_on_device_list(move(devices));
        });
    }

    pw_thread_loop* m_loop { nullptr };
    pw_context* m_context { nullptr };
    pw_core* m_core { nullptr };
    pw_registry* m_registry { nullptr };
    spa_hook m_core_listener {};
    spa_hook m_registry_listener {};
    bool m_is_running { false };
    // Cleared on the loop's thread when the daemon goes, and read on the owner's.
    Atomic<bool> m_is_connected { false };
    static constexpr auto INITIAL_RECONNECT_DELAY = AK::Duration::from_seconds(1);
    AK::Duration m_reconnect_delay { INITIAL_RECONNECT_DELAY };
    RefPtr<Core::Timer> m_reconnect_timer;
    int m_initial_sync { 0 };
    int m_report_sync { -1 };
    bool m_has_initial_list { false };

    HashMap<u32, NonnullOwnPtr<Node>> m_nodes;
    Vector<PipeWireStream*> m_streams;

    Core::EventLoop* m_owner_loop { nullptr };
    Function<void(Vector<Camera::DeviceInfo>)> m_on_device_list;
};

}

// The backend lasts for the rest of the process, so nothing tears the library down.
NonnullOwnPtr<CameraBackend> create_platform_camera_backend()
{
    return make<PipeWireCameras>();
}

}

#pragma GCC diagnostic pop
