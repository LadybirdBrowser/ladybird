/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibAudioServerClient/Client.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>
#include <LibMain/Main.h>

static ErrorOr<ByteString> find_audioserver_executable_path();
static ErrorOr<Core::Process> spawn_audioserver_with_takeover(int takeover_fd);
static ErrorOr<NonnullRefPtr<AudioServerClient::Client>> create_client_from_fd(int fd);

enum class Instrument : u8 {
    Tone,
    Kick,
    HiHat,
};

struct Voice {
    double hz { 0.0 };
    double bpm { 120.0 };
    u16 rhythm_mask { 0 };
    float pan { 0.0f };
    Instrument instrument { Instrument::Tone };
};

static double seconds_from_samples(u64 sample_index, u32 sample_rate)
{
    if (sample_rate == 0)
        return 0.0;
    return static_cast<double>(sample_index) / static_cast<double>(sample_rate);
}

static float random_bipolar(u32& state)
{
    state = state * 1664525u + 1013904223u;
    u32 value = state >> 9;
    float normalized = static_cast<float>(value) * (1.0f / static_cast<float>(0x007fffff));
    return (normalized * 2.0f) - 1.0f;
}

static float tone_sample(u64 sample_index, u32 sample_rate, double note_hz, double bpm, u16 rhythm_mask, double& phase_radians, float amplitude)
{
    if (sample_rate == 0)
        return 0.0f;

    double const seconds_per_sixteenth = (60.0 / bpm) / 4.0;
    double const bar_seconds = seconds_per_sixteenth * 16.0;
    double const t = seconds_from_samples(sample_index, sample_rate);

    double const t_in_bar = fmod(t, bar_seconds);
    int const step = static_cast<int>(t_in_bar / seconds_per_sixteenth) & 0x0f;
    bool const gate_on = ((rhythm_mask >> step) & 1u) != 0;

    float envelope = 0.0f;
    if (gate_on) {
        double const t_in_step = t_in_bar - (static_cast<double>(step) * seconds_per_sixteenth);
        double const attack_seconds = 0.005;
        double const release_seconds = 0.020;

        double const attack = min(1.0, t_in_step / attack_seconds);
        double const release = min(1.0, (seconds_per_sixteenth - t_in_step) / release_seconds);
        envelope = static_cast<float>(min(attack, release));
    }

    double const phase_step = (2.0 * M_PI * note_hz) / static_cast<double>(sample_rate);
    phase_radians += phase_step;
    if (phase_radians > 2.0 * M_PI)
        phase_radians = fmod(phase_radians, 2.0 * M_PI);

    float const osc = static_cast<float>(sin(phase_radians));
    return osc * amplitude * envelope;
}

static float kick_sample(u64 sample_index, u32 sample_rate, double bpm, u16 rhythm_mask, double& phase_radians, float amplitude)
{
    if (sample_rate == 0)
        return 0.0f;

    double const seconds_per_sixteenth = (60.0 / bpm) / 4.0;
    double const bar_seconds = seconds_per_sixteenth * 16.0;
    double const t = seconds_from_samples(sample_index, sample_rate);

    double const t_in_bar = fmod(t, bar_seconds);
    int const step = static_cast<int>(t_in_bar / seconds_per_sixteenth) & 0x0f;
    bool const gate_on = ((rhythm_mask >> step) & 1u) != 0;
    if (!gate_on)
        return 0.0f;

    double const t_in_step = t_in_bar - (static_cast<double>(step) * seconds_per_sixteenth);

    double const hit_duration = min(0.12, seconds_per_sixteenth);
    if (t_in_step < 0.0 || t_in_step > hit_duration)
        return 0.0f;

    double const attack = min(1.0, t_in_step / 0.002);
    double const decay = exp(-t_in_step * 18.0);
    float envelope = static_cast<float>(attack * decay);

    double const start_hz = 120.0;
    double const end_hz = 50.0;
    double const sweep = t_in_step / hit_duration;
    double const hz = start_hz * pow(end_hz / start_hz, sweep);

    double const phase_step = (2.0 * M_PI * hz) / static_cast<double>(sample_rate);
    phase_radians += phase_step;
    if (phase_radians > 2.0 * M_PI)
        phase_radians = fmod(phase_radians, 2.0 * M_PI);

    float osc = static_cast<float>(sin(phase_radians));
    return osc * amplitude * envelope;
}

static float one_pole_sample(u64 sample_index, u32 sample_rate, double bpm, u16 rhythm_mask, u32& noise_state, float& hp_prev_x, float& hp_prev_y, float amplitude)
{
    if (sample_rate == 0)
        return 0.0f;

    double const seconds_per_sixteenth = (60.0 / bpm) / 4.0;
    double const bar_seconds = seconds_per_sixteenth * 16.0;
    double const t = seconds_from_samples(sample_index, sample_rate);

    double const t_in_bar = fmod(t, bar_seconds);
    int const step = static_cast<int>(t_in_bar / seconds_per_sixteenth) & 0x0f;
    bool const gate_on = ((rhythm_mask >> step) & 1u) != 0;
    if (!gate_on)
        return 0.0f;

    double const t_in_step = t_in_bar - (static_cast<double>(step) * seconds_per_sixteenth);
    double const hit_duration = min(0.05, seconds_per_sixteenth);
    if (t_in_step < 0.0 || t_in_step > hit_duration)
        return 0.0f;

    double const attack = min(1.0, t_in_step / 0.0015);
    double const decay = exp(-t_in_step * 55.0);
    float envelope = static_cast<float>(attack * decay);

    float x = random_bipolar(noise_state);

    float a = 0.98f;
    float y = a * (hp_prev_y + x - hp_prev_x);
    hp_prev_x = x;
    hp_prev_y = y;

    return y * amplitude * envelope;
}

ErrorOr<int> ladybird_main(Main::Arguments)
{
    outln("ExampleAudioServerClient: AudioServer with 16 simultaneous voices");

    Core::EventLoop event_loop;

    int fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    auto audio_server_process = TRY(spawn_audioserver_with_takeover(fds[1]));

    auto kill_audio_server = AK::ArmedScopeGuard([&] {
        (void)Core::System::kill(audio_server_process.pid(), SIGTERM);
        (void)audio_server_process.wait_for_termination();
    });

    TRY(Core::System::close(fds[1]));

    auto first_client = TRY(create_client_from_fd(fds[0]));

    Vector<NonnullRefPtr<AudioServerClient::Client>> clients;
    clients.append(first_client);

    auto sockets_response = first_client->send_sync_but_allow_failure<Messages::AudioServerServer::ConnectNewClients>(15);
    if (!sockets_response)
        return Error::from_string_literal("ExampleAudioServerClient: connect_new_clients IPC failed");

    auto sockets = sockets_response->take_sockets();
    if (sockets.size() != 15)
        return Error::from_string_literal("ExampleAudioServerClient: connect_new_clients returned unexpected count");

    for (auto& file : sockets) {
        int fd = file.take_fd();
        clients.append(TRY(create_client_from_fd(fd)));
    }

    struct Session {
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
        Core::SharedSingleProducerCircularBuffer ring;
        Instrument instrument { Instrument::Tone };
        double note_hz { 0.0 };
        double bpm { 120.0 };
        u16 rhythm_mask { 0 };
        double phase_radians { 0.0 };
        u32 noise_state { 0x12345678u };
        float hp_prev_x { 0.0f };
        float hp_prev_y { 0.0f };
        float pan { 0.0f };
        u64 sample_cursor { 0 };
    };

    Vector<Session> sessions;
    sessions.ensure_capacity(clients.size());

    static constexpr u32 target_latency_ms = 50;

    static constexpr Array<Voice, 16> voices {
        Voice { .hz = 0.0, .bpm = 100.0, .rhythm_mask = 0x0101, .pan = 0.0f, .instrument = Instrument::Kick },
        Voice { .hz = 0.0, .bpm = 100.0, .rhythm_mask = 0xAAAA, .pan = -0.25f, .instrument = Instrument::HiHat },
        Voice { .hz = 0.0, .bpm = 100.0, .rhythm_mask = 0x5555, .pan = 0.25f, .instrument = Instrument::HiHat },
        Voice { .hz = 261.625565, .bpm = 96.0, .rhythm_mask = 0x1111, .pan = -0.80f, .instrument = Instrument::Tone },
        Voice { .hz = 329.627557, .bpm = 108.0, .rhythm_mask = 0x2222, .pan = -0.60f, .instrument = Instrument::Tone },
        Voice { .hz = 392.0, .bpm = 120.0, .rhythm_mask = 0x3333, .pan = -0.40f, .instrument = Instrument::Tone },
        Voice { .hz = 493.883301, .bpm = 132.0, .rhythm_mask = 0x0F0F, .pan = -0.20f, .instrument = Instrument::Tone },
        Voice { .hz = 587.329536, .bpm = 144.0, .rhythm_mask = 0x8421, .pan = 0.0f, .instrument = Instrument::Tone },
        Voice { .hz = 0.0, .bpm = 100.0, .rhythm_mask = 0x0108, .pan = 0.0f, .instrument = Instrument::Kick },
        Voice { .hz = 0.0, .bpm = 100.0, .rhythm_mask = 0xF0F0, .pan = 0.35f, .instrument = Instrument::HiHat },
        Voice { .hz = 440.0, .bpm = 156.0, .rhythm_mask = 0x00FF, .pan = 0.20f, .instrument = Instrument::Tone },
        Voice { .hz = 349.228231, .bpm = 168.0, .rhythm_mask = 0x7BDE, .pan = 0.40f, .instrument = Instrument::Tone },
        Voice { .hz = 523.251131, .bpm = 84.0, .rhythm_mask = 0x1248, .pan = 0.60f, .instrument = Instrument::Tone },
        Voice { .hz = 293.664768, .bpm = 90.0, .rhythm_mask = 0x8888, .pan = 0.80f, .instrument = Instrument::Tone },
        Voice { .hz = 659.255114, .bpm = 102.0, .rhythm_mask = 0x4444, .pan = -0.10f, .instrument = Instrument::Tone },
        Voice { .hz = 220.0, .bpm = 114.0, .rhythm_mask = 0x1357, .pan = 0.10f, .instrument = Instrument::Tone },
    };

    static constexpr float tone_amplitude = 0.040f;
    static constexpr float kick_amplitude = 0.280f;
    static constexpr float hat_amplitude = 0.120f;

    for (size_t i = 0; i < clients.size(); ++i) {
        auto session = TRY(clients[i]->create_audio_output_session(target_latency_ms));
        Voice const& voice = voices[i];
        sessions.append(Session {
            .sample_rate = session.sample_rate,
            .channel_count = session.channel_count,
            .ring = move(session.ring),
            .instrument = voice.instrument,
            .note_hz = voice.hz,
            .bpm = voice.bpm,
            .rhythm_mask = voice.rhythm_mask,
            .phase_radians = 0.0,
            .noise_state = static_cast<u32>(0x9E3779B9u ^ static_cast<u32>(i * 1103515245u)),
            .hp_prev_x = 0.0f,
            .hp_prev_y = 0.0f,
            .pan = voice.pan,
            .sample_cursor = 0,
        });
    }

    for (auto& s : sessions) {
        if (s.sample_rate == 0 || s.channel_count == 0)
            return Error::from_string_literal("ExampleAudioServerClient: invalid output format from AudioServer");
    }

    outln("ExampleAudioServerClient: format {} Hz, {} channels", sessions[0].sample_rate, sessions[0].channel_count);

    static constexpr double duration_seconds = 6.0;
    static constexpr u32 frames_per_write = 256;

    auto start_time = MonotonicTime::now();
    while ((MonotonicTime::now() - start_time) < AK::Duration::from_milliseconds(static_cast<i64>(duration_seconds * 1000.0))) {
        for (auto& session : sessions) {
            size_t bytes_per_frame = static_cast<size_t>(session.channel_count) * sizeof(float);
            size_t bytes_to_write = static_cast<size_t>(frames_per_write) * bytes_per_frame;

            if (session.ring.available_to_write() < bytes_to_write)
                continue;

            size_t const channel_count = session.channel_count;
            Vector<float> samples;
            samples.resize(static_cast<size_t>(frames_per_write) * channel_count);

            for (u32 frame = 0; frame < frames_per_write; ++frame) {
                float value = 0.0f;
                if (session.instrument == Instrument::Tone) {
                    value = tone_sample(session.sample_cursor + frame, session.sample_rate, session.note_hz, session.bpm, session.rhythm_mask, session.phase_radians, tone_amplitude);
                } else if (session.instrument == Instrument::Kick) {
                    value = kick_sample(session.sample_cursor + frame, session.sample_rate, session.bpm, session.rhythm_mask, session.phase_radians, kick_amplitude);
                } else {
                    value = one_pole_sample(session.sample_cursor + frame, session.sample_rate, session.bpm, session.rhythm_mask, session.noise_state, session.hp_prev_x, session.hp_prev_y, hat_amplitude);
                }

                if (session.channel_count >= 2) {
                    float pan = session.pan;
                    float left_gain = 0.5f * (1.0f - pan);
                    float right_gain = 0.5f * (1.0f + pan);
                    samples[(static_cast<size_t>(frame) * channel_count) + 0] = value * left_gain;
                    samples[(static_cast<size_t>(frame) * channel_count) + 1] = value * right_gain;
                    for (u32 ch = 2; ch < session.channel_count; ++ch)
                        samples[(static_cast<size_t>(frame) * channel_count) + static_cast<size_t>(ch)] = value;
                } else {
                    samples[(static_cast<size_t>(frame) * channel_count) + 0] = value;
                }
            }

            session.sample_cursor += frames_per_write;

            ReadonlyBytes bytes { reinterpret_cast<u8 const*>(samples.data()), samples.size() * sizeof(float) };
            size_t written = 0;
            while (written < bytes.size()) {
                size_t did_write = session.ring.try_write(bytes.slice(written));
                written += did_write;
                if (did_write == 0)
                    usleep(1000);
            }
        }

        usleep(1000);
    }

    kill_audio_server.disarm();
    TRY(Core::System::kill(audio_server_process.pid(), SIGTERM));
    TRY(audio_server_process.wait_for_termination());

    outln("ExampleAudioServerClient: done");
    return 0;
}

static ErrorOr<ByteString> find_audioserver_executable_path()
{
    auto current_executable_path = TRY(Core::System::current_executable_path());

    LexicalPath current_executable_lexical_path(current_executable_path);
    auto current_dir = current_executable_lexical_path.dirname();

    Vector<ByteString> candidates;
    candidates.append(LexicalPath::join(current_dir, "AudioServer"sv).string());
    candidates.append(LexicalPath::join(current_dir, "Ladybird.app"sv, "Contents"sv, "MacOS"sv, "AudioServer"sv).string());
    candidates.append(LexicalPath::join(current_dir, ".."sv, "libexec"sv, "AudioServer"sv).string());
    for (auto& candidate : candidates) {
        auto path = LexicalPath::canonicalized_path(move(candidate));
        if (!Core::System::access(path, X_OK).is_error())
            return ByteString(path);
    }

    return Error::from_string_literal("ExampleAudioServerClient: failed to locate AudioServer executable");
}

static ErrorOr<Core::Process> spawn_audioserver_with_takeover(int takeover_fd)
{
    auto audio_server_path = TRY(find_audioserver_executable_path());

    auto takeover_string = ByteString::formatted("example:{}", takeover_fd);
    TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, takeover_string, Core::Environment::Overwrite::Yes));

    Vector<ByteString> child_arguments;

    Core::ProcessSpawnOptions options {
        .name = "AudioServer"sv,
        .executable = audio_server_path,
        .search_for_executable_in_path = false,
        .arguments = child_arguments,
    };

    auto audio_server_process = TRY(Core::Process::spawn(options));
    TRY(Core::Environment::unset("SOCKET_TAKEOVER"sv));

    return audio_server_process;
}

static ErrorOr<NonnullRefPtr<AudioServerClient::Client>> create_client_from_fd(int fd)
{
    auto socket_or_error = Core::LocalSocket::adopt_fd(fd);
    if (socket_or_error.is_error())
        return socket_or_error.release_error();

    auto transport = make<IPC::Transport>(socket_or_error.release_value());
    return adopt_ref(*new AudioServerClient::Client(move(transport)));
}
