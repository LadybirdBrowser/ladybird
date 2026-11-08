/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/DedicatedWorkerGlobalScope.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Scripting/WorkerEnvironmentSettingsObject.h>
#include <LibWeb/HTML/WorkerLocation.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWebCommon/HTML/WorkerTypes.h>

#if !defined(AK_OS_WINDOWS)
#    include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#endif

// A worker's agent on a thread, as a WebContent process will host it, without the process and its connections around it.
// The script uses what a worker can do without a network. Every part of it reaches state that used to be the process's.
static constexpr auto AGENT_SCRIPT = R"~~~(
(() => {
    "use strict";

    function check(name, condition) {
        if (!condition)
            throw new Error(`check failed: ${name}`);
    }

    const toHex = bytes => Array.from(new Uint8Array(bytes), byte => byte.toString(16).padStart(2, "0")).join("");

    async function timers() {
        await new Promise(resolve => setTimeout(resolve, 1));

        let ticks = 0;
        await new Promise(resolve => {
            const id = setInterval(() => {
                if (++ticks === 3) {
                    clearInterval(id);
                    resolve();
                }
            }, 1);
        });

        let cancelled = true;
        clearTimeout(setTimeout(() => (cancelled = false), 0));
        await new Promise(resolve => setTimeout(resolve, 2));
        check("cancelled timer", cancelled);

        let order = [];
        queueMicrotask(() => order.push("microtask"));
        await new Promise(resolve => setTimeout(resolve, 0));
        check("microtask", order.length === 1);
    }

    async function blobs() {
        const blob = new Blob(["hello ", new Uint8Array([119, 111, 114, 108, 100])], { type: "text/plain" });
        check("blob size", blob.size === 11);
        check("blob text", (await blob.text()) === "hello world");
        check("blob slice", (await blob.slice(6).text()) === "world");

        const file = new File([blob], "name.txt", { lastModified: 1 });
        check("file", file.name === "name.txt" && file.size === 11);
        check("file bytes", new Uint8Array(await file.arrayBuffer())[0] === 104);

        const reader = new FileReaderSync();
        check("reader", reader.readAsText(blob) === "hello world");

        const urls = [];
        for (let i = 0; i < 20; ++i)
            urls.push(URL.createObjectURL(blob));
        check("object url", urls[0].startsWith("blob:https://agent.test/"));
        for (let i = 0; i < 10; ++i)
            URL.revokeObjectURL(urls[i]);
        // NB: The others stay for the agent's shutdown to remove.
    }

    async function fetches() {
        const response = await fetch("data:text/plain,hello");
        check("data fetch", (await response.text()) === "hello");

        const url = URL.createObjectURL(new Blob(["from a blob"], { type: "text/plain" }));
        const blobResponse = await fetch(url);
        check("blob fetch type", blobResponse.headers.get("Content-Type") === "text/plain");
        check("blob fetch", (await blobResponse.text()) === "from a blob");
        URL.revokeObjectURL(url);
    }

    function urls() {
        const url = new URL("https://user@example.com:8080/a/../b?x=1&y=2#z");
        check("href", url.href === "https://user@example.com:8080/b?x=1&y=2#z");
        check("search params", url.searchParams.get("y") === "2");
        url.searchParams.append("w", "a b");
        check("search", url.search === "?x=1&y=2&w=a+b");
        check("relative", new URL("../c", "https://example.com/a/b/").pathname === "/a/c");
        check("can parse", !URL.canParse("not a url"));
        check("location", location.origin === "https://agent.test");
    }

    async function messaging() {
        const { port1, port2 } = new MessageChannel();
        const bytes = new Uint8Array([1, 2, 3]);
        const inner = new MessageChannel();
        const event = await new Promise(resolve => {
            port2.onmessage = resolve;
            port1.postMessage({ value: 42, map: new Map([["a", 1]]), bytes }, [bytes.buffer, inner.port1]);
        });
        check("message", event.data.value === 42 && event.data.map.get("a") === 1);
        check("transfer", event.data.bytes[2] === 3 && bytes.length === 0);
        check("ports", event.ports.length === 1 && event.ports === event.ports);

        const echoed = await new Promise(resolve => {
            event.ports[0].onmessage = resolve;
            inner.port2.postMessage("over the inner port");
        });
        check("inner port", echoed.data === "over the inner port");

        const clone = structuredClone({ date: new Date(5), regex: /a/g, set: new Set([1]) });
        check("clone", clone.date.getTime() === 5 && clone.regex.flags === "g" && clone.set.has(1));

        const left = new BroadcastChannel("agent-threads");
        const right = new BroadcastChannel("agent-threads");
        const broadcast = await new Promise(resolve => {
            right.onmessage = resolve;
            left.postMessage("to this agent only");
        });
        check("broadcast", broadcast.data === "to this agent only");
        left.close();
        right.close();

        // NB: These stay open, for the agent's shutdown to deal with.
        self.openPorts = [port1, port2, new MessageChannel().port1];
    }

    async function cryptography() {
        const random = crypto.getRandomValues(new Uint8Array(32));
        check("random", random.some(byte => byte !== 0));
        check("uuid", crypto.randomUUID().length === 36);

        const message = new TextEncoder().encode("abc");
        const digest = await crypto.subtle.digest("SHA-256", message);
        check("digest", toHex(digest) === "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

        const key = await crypto.subtle.importKey("raw", new Uint8Array(32), { name: "HMAC", hash: "SHA-256" }, true, ["sign", "verify"]);
        check("key usages", key.usages === key.usages && key.usages.length === 2);
        const signature = await crypto.subtle.sign("HMAC", key, message);
        check("verify", await crypto.subtle.verify("HMAC", key, signature, message));

        const aes = await crypto.subtle.generateKey({ name: "AES-GCM", length: 128 }, false, ["encrypt", "decrypt"]);
        const iv = new Uint8Array(12);
        const encrypted = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, aes, message);
        const decrypted = await crypto.subtle.decrypt({ name: "AES-GCM", iv }, aes, encrypted);
        check("decrypt", new TextDecoder().decode(decrypted) === "abc");

        let rejected = false;
        await crypto.subtle.importKey("spki", new Uint8Array(4), { name: "ECDSA", namedCurve: "P-256" }, true, ["verify"]).catch(() => (rejected = true));
        check("malformed key", rejected);
    }

    async function performanceTimeline() {
        const start = performance.now();
        check("time origin", performance.timeOrigin > 0 && start >= 0);

        const types = PerformanceObserver.supportedEntryTypes;
        check("entry types", types === PerformanceObserver.supportedEntryTypes && types.includes("mark"));

        const observed = new Promise(resolve => {
            new PerformanceObserver((list, observer) => {
                observer.disconnect();
                resolve(list.getEntries());
            }).observe({ entryTypes: ["mark"] });
        });
        performance.mark("first");
        performance.mark("second");
        const measure = performance.measure("between", "first", "second");
        check("measure", measure.duration >= 0);
        check("marks", performance.getEntriesByName("first").length === 1);
        check("observer", (await observed).length >= 1);
        performance.clearMarks();
    }

    async function fonts() {
        const face = new FontFace("Test Family", "url(https://agent.test/font.woff2)", {
            style: "italic",
            weight: "700",
            stretch: "75%",
            unicodeRange: "U+0-FF",
            display: "swap",
        });
        check("family", face.family === '"Test Family"' || face.family === "Test Family");
        check("weight", face.weight === "700");
        check("style", face.style === "italic");
        check("stretch", face.stretch === "75%");
        check("status", face.status === "unloaded");

        face.weight = "bold";
        check("set weight", face.weight === "bold");

        const invalid = new FontFace("Invalid", "url(a.woff)", { weight: "not a weight" });
        const reason = await invalid.loaded.catch(error => error);
        check("invalid descriptor", reason.name === "SyntaxError");

        const broken = new FontFace("Broken", new Uint8Array(16));
        await broken.loaded.catch(() => {});
        check("broken data", broken.status === "error");

        self.fonts.add(face);
        check("font set", self.fonts.size === 1 && self.fonts.has(face));
        self.fonts.delete(face);
        await self.fonts.ready;
    }

    async function others() {
        check("text coding", new TextDecoder().decode(new TextEncoder().encode("käse")) === "käse");
        check("base64", atob(btoa("hello")) === "hello");

        const controller = new AbortController();
        const aborted = new Promise(resolve => controller.signal.addEventListener("abort", resolve));
        controller.abort();
        await aborted;
        check("abort", controller.signal.aborted);

        const target = new EventTarget();
        let dispatched = 0;
        target.addEventListener("ping", () => ++dispatched);
        target.dispatchEvent(new Event("ping"));
        check("event target", dispatched === 1);

        const stream = new ReadableStream({
            start(streamController) {
                streamController.enqueue("chunk");
                streamController.close();
            },
        });
        const reader = stream.getReader();
        check("stream", (await reader.read()).value === "chunk");

        const matrix = new DOMMatrix([1, 2, 3, 4, 5, 6]).translate(1, 2);
        check("matrix", matrix.e === 12);

        const result = await navigator.locks.request("lock", async lock => lock.name);
        check("locks", result === "lock");
        const state = await navigator.locks.query();
        check("lock state", state.held.length === 0);

        check("navigator", navigator.userAgent.length > 0 && navigator.platform.length > 0 && navigator.language.length > 0);
        check("hardware", navigator.hardwareConcurrency > 0);

        const module = new WebAssembly.Module(new Uint8Array([
            0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
            0x03, 0x02, 0x01, 0x00, 0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00, 0x0a, 0x09, 0x01, 0x07,
            0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b,
        ]));
        check("wasm", new WebAssembly.Instance(module).exports.add(2, 3) === 5);

        check("stack", new Error("x").stack.length > 0);
        check("native source", String(parseInt).includes("[native code]"));

        const status = await navigator.permissions.query({ name: "geolocation" });
        check("permissions", status.state === "prompt" || status.state === "denied");

        const context = new OffscreenCanvas(8, 8).getContext("2d");
        context.font = "italic bold 12px serif";
        check("canvas font", context.font.includes("12px"));
        check("canvas text", context.measureText("hello").width >= 0);
        // NB: Nothing here paints: that takes a compositor.
    }

    function garbage() {
        // NB: Enough to get the collector going while other agents run, with what it has to finalize.
        let kept = [];
        for (let i = 0; i < 3000; ++i) {
            kept.push({ url: new URL(`https://example.com/${i}`), blob: new Blob(["x"]) });
            if (kept.length > 100)
                kept = [];
        }
    }

    async function runPass(pass) {
        await timers();
        await blobs();
        await fetches();
        urls();
        await messaging();
        await cryptography();
        await performanceTimeline();
        await fonts();
        await others();
        garbage();
        return pass;
    }

    self.testResult = undefined;
    self.passes = (self.passes ?? 0) + 1;
    runPass(self.passes).then(
        pass => (self.testResult = `ok ${pass}`),
        error => (self.testResult = `${error}\n${error.stack}`)
    );
})();
)~~~"sv;

namespace {

class AgentPageClient final : public Web::PageClient {
    GC_CELL(AgentPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(AgentPageClient);

public:
    static GC::Ref<AgentPageClient> create()
    {
        return GC::Heap::the().allocate<AgentPageClient>();
    }

    virtual Web::PageId id() const override { return Web::PageId { 0 }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return false; }
    virtual Gfx::Palette palette() const override { return Gfx::Palette(*m_palette_impl); }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1.0; }
    virtual double device_pixel_ratio() const override { return 1.0; }
    virtual double device_pixels_per_css_pixel() const override { return 1.0; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::NoPreference; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event(Web::PageId, u64, Web::EventResult) override { VERIFY_NOT_REACHED(); }
    virtual void request_frame() override { VERIFY_NOT_REACHED(); }
    virtual void request_file(Web::FileRequest) override { VERIFY_NOT_REACHED(); }
    virtual bool is_headless() const override { return true; }

private:
    AgentPageClient()
        : m_page(Web::Page::create(*this))
    {
        auto buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)));
        m_palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(buffer);
    }

    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ref<Web::Page> m_page;
    RefPtr<Gfx::PaletteImpl> m_palette_impl;
};

GC_DEFINE_ALLOCATOR(AgentPageClient);

}

static Web::HTML::SerializedEnvironmentSettingsObject outside_settings(URL::URL const& url)
{
    return {
        .id = Web::HTML::EnvironmentId::generate(),
        .creation_url = url,
        .top_level_creation_url = url,
        .top_level_origin = url.origin(),
        .api_base_url = url,
        .origin = url.origin(),
        .has_cross_site_ancestor = false,
        .policy_container = {},
        .cross_origin_isolated_capability = Web::HTML::CanUseCrossOriginIsolatedAPIs::No,
        .prohibits_mixed_security_contexts = Web::MixedContent::ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts,
        .agent_cluster_id = 1,
        .agent_cluster_cross_origin_isolation_mode = Web::HTML::CrossOriginIsolationMode::None,
        .time_origin = 0,
        .global = Web::HTML::SerializedWorkerGlobalScope {
            .relevant_settings_object_is_secure_context = true,
            .relevant_settings_object_policy_container = {},
        },
    };
}

class TestAgent {
public:
    TestAgent()
    {
        Web::Bindings::initialize_agent(Web::HTML::AgentType::DedicatedWorker);
        Web::HTML::surrounding_agent().fetch_executor = Web::Fetch::Fetching::create_inline_fetch_executor();
        Web::HTML::current_event_loop().set_on_destroyed([] {
            Core::EventLoop::current().quit(0);
        });

        auto url = *URL::Parser::basic_parse("https://agent.test/worker.js"sv);
        auto outside = outside_settings(url);

        auto page_client = AgentPageClient::create();
        auto page = GC::Ref { page_client->page() };
        m_page = page;

        auto execution_context = Web::Bindings::create_a_new_javascript_realm(
            JS::VM::the(),
            Web::HTML::CrossOriginIsolationMode::None,
            [page](JS::Realm& realm) -> GC::Ref<JS::Object> {
                return Web::Bindings::create_global_object_wrapper(realm, GC::Ref { *GC::Heap::the().allocate<Web::HTML::DedicatedWorkerGlobalScope>(page) });
            },
            nullptr);

        m_global_scope = Web::Bindings::impl_from<Web::HTML::WorkerGlobalScope>(execution_context->realm->global_object());
        m_global_scope->set_url(url);
        m_global_scope->owner_set().append(outside.global.get<Web::HTML::SerializedWorkerGlobalScope>());

        m_settings = Web::HTML::WorkerEnvironmentSettingsObject::setup(page, move(execution_context), outside, Web::HighResolutionTime::unsafe_shared_current_time(), url.origin(), Web::HTML::EnvironmentId::generate());
        m_global_scope->set_location(GC::Heap::the().allocate<Web::HTML::WorkerLocation>(*m_global_scope));
        m_settings->execution_ready = true;
    }

    // Runs the script and the tasks it starts, until it has left its result. Returns the result.
    String run_pass()
    {
        auto source = Utf16String::from_utf8_without_validation(AGENT_SCRIPT.bytes());
        auto url = *URL::Parser::basic_parse("https://agent.test/worker.js"sv);
        auto script = Web::HTML::ClassicScript::create("worker.js"sv, move(source), *m_settings, url);
        (void)script->run();

        auto& global_object = m_settings->realm().global_object();
        JS::PropertyKey result_key { "testResult"_utf16_fly_string };
        Web::HTML::current_event_loop().spin_until(GC::create_function(GC::Heap::the(), GC::captures(global_object, result_key), [](JS::Object& global_object, JS::PropertyKey const& result_key) {
            return !global_object.get_without_side_effects(result_key).is_undefined();
        }));

        auto result = global_object.get_without_side_effects(result_key).to_utf16_string_without_side_effects();
        return MUST(result.utf16_view().to_utf8());
    }

    // What happens when a worker closes, as far as the host of the worker has a part in it.
    void close_and_shut_down()
    {
        as<Web::HTML::DedicatedWorkerGlobalScope>(*m_global_scope).close();
        Core::EventLoop::current().exec();
        shut_down();
    }

    // Starts a fetch of the URL in a task that then closes the worker, and runs the worker's event loop until it is
    // destroyed. Returns the fetch's request.
    NonnullRefPtr<Web::Fetch::Infrastructure::Request> fetch_and_close(StringView url, Web::Fetch::Infrastructure::FetchAlgorithms::Input input)
    {
        auto request = Web::Fetch::Infrastructure::Request::create();
        request->set_url_list({ *URL::Parser::basic_parse(url) });
        request->set_client(m_settings->address());
        auto algorithms = Web::Fetch::Infrastructure::FetchAlgorithms::create(move(input));

        auto steps = GC::create_function(GC::Heap::the(), GC::captures(*m_settings, *m_global_scope, request, algorithms), [](auto& settings, auto& global_scope, auto& request, auto& algorithms) {
            Web::HTML::TemporaryExecutionContext execution_context { settings.realm(), Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            (void)Web::Fetch::Fetching::fetch(settings.realm(), request, algorithms);
            as<Web::HTML::DedicatedWorkerGlobalScope>(global_scope).close();
        });
        Web::HTML::queue_global_task(Web::HTML::Task::Source::Unspecified, m_settings->realm().global_object(), steps);
        Core::EventLoop::current().exec();
        return request;
    }

    // What the host of a worker does once the worker's event loop is destroyed.
    void shut_down()
    {
        m_global_scope->clear_map_of_active_timers();
        Web::HTML::MessagePort::disentangle_ports_of_realm(Web::HTML::relevant_realm(*m_global_scope));
        m_global_scope->owner_set().clear();
        Web::FileAPI::remove_entries_of_environment(*m_settings, *m_page);

        Web::Bindings::shut_down_agent();
    }

private:
    GC::Ptr<Web::Page> m_page;
    GC::Ptr<Web::HTML::WorkerGlobalScope> m_global_scope;
    GC::Ptr<Web::HTML::WorkerEnvironmentSettingsObject> m_settings;
};

static Atomic<size_t> s_failures;

static void check_pass(String const& result, size_t pass)
{
    auto expected = MUST(String::formatted("ok {}", pass));
    if (result == expected)
        return;
    warnln("Agent pass {} failed: {}", pass, result);
    s_failures.fetch_add(1);
}

static void run_agent(size_t passes = 1)
{
    Core::EventLoop event_loop;
    TestAgent agent;
    for (size_t pass = 1; pass <= passes; ++pass)
        check_pass(agent.run_pass(), pass);
    GC::Heap::the().collect_garbage();
    agent.close_and_shut_down();
}

template<typename Callback>
static Vector<NonnullRefPtr<Threading::Thread>> start_agent_threads(size_t thread_count, Callback& callback)
{
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t i = 0; i < thread_count; ++i) {
        auto thread = Threading::Thread::construct("AgentThreadTest"sv, [&callback, i] {
            callback(i);
            return static_cast<intptr_t>(0);
        });
        thread->set_stack_size(JS::VM::MINIMUM_THREAD_STACK_SIZE);
        thread->start();
        threads.append(move(thread));
    }
    return threads;
}

template<typename Callback>
static void run_on_agent_threads(size_t thread_count, Callback callback)
{
    for (auto& thread : start_agent_threads(thread_count, callback))
        MUST(thread->join());
}

// What a process does once, before any agent starts.
static void set_up_process()
{
    static bool is_set_up = false;
    if (exchange(is_set_up, true))
        return;
    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);
    Web::Platform::FontPlugin::install(*new Web::Platform::FontPlugin(false));
}

// Each command an executor posts holds the executor until the command has run or been dropped.
static bool has_queued_commands(Web::Fetch::Engine::FetchExecutor const& executor)
{
    return executor.ref_count() > 1;
}

TEST_CASE(an_agent_runs_on_a_thread_of_its_own)
{
    set_up_process();
    run_on_agent_threads(1, [](size_t) { run_agent(); });
    EXPECT_EQ(s_failures.load(), 0u);
}

TEST_CASE(several_agents_run_at_once)
{
    set_up_process();
    run_on_agent_threads(8, [](size_t) { run_agent(3); });
    EXPECT_EQ(s_failures.load(), 0u);
}

TEST_CASE(agents_come_and_go_while_others_run)
{
    set_up_process();
    run_on_agent_threads(6, [](size_t index) {
        // NB: The lengths differ, so that agents shut down at all points of the others' lives.
        for (size_t i = 0; i < 2 + index % 3; ++i)
            run_agent(1 + (i + index) % 2);
    });
    EXPECT_EQ(s_failures.load(), 0u);
}

TEST_CASE(an_agent_keeps_working_after_another_shut_down)
{
    set_up_process();

    Atomic<bool> second_is_up { false };
    Atomic<bool> first_is_down { false };
    auto wait_for = [](Atomic<bool>& flag) {
        while (!flag.load())
            sched_yield();
    };

    run_on_agent_threads(2, [&](size_t index) {
        if (index == 0) {
            Core::EventLoop event_loop;
            TestAgent agent;
            check_pass(agent.run_pass(), 1);
            wait_for(second_is_up);
            check_pass(agent.run_pass(), 2);
            agent.close_and_shut_down();
            first_is_down.store(true);
            return;
        }

        Core::EventLoop event_loop;
        TestAgent agent;
        check_pass(agent.run_pass(), 1);
        second_is_up.store(true);
        wait_for(first_is_down);
        check_pass(agent.run_pass(), 2);
        check_pass(agent.run_pass(), 3);
        agent.close_and_shut_down();
    });
    EXPECT_EQ(s_failures.load(), 0u);
}

TEST_CASE(a_thread_hosts_one_agent_after_the_other)
{
    set_up_process();
    run_on_agent_threads(2, [](size_t) {
        for (size_t i = 0; i < 4; ++i)
            run_agent();
    });
    EXPECT_EQ(s_failures.load(), 0u);
}

TEST_CASE(a_fetch_started_just_before_close_never_runs_its_in_parallel_steps)
{
    set_up_process();
    run_on_agent_threads(1, [](size_t) {
        Core::EventLoop event_loop;
        TestAgent agent;

        auto request = agent.fetch_and_close("about:blank"sv, Web::Fetch::Infrastructure::FetchAlgorithms::Input { Web::Fetch::Engine::BodyIntent::DrainAndDiscard });

        // The worker's event loop has stopped with the command that runs the fetch's in-parallel steps still queued.
        EXPECT(has_queued_commands(*Web::HTML::surrounding_agent().fetch_executor));

        agent.shut_down();

        // Main fetch's in-parallel steps would make the response tainting of this no-cors request opaque.
        EXPECT_EQ(request->response_tainting(), Web::Fetch::Infrastructure::Request::ResponseTainting::Basic);
    });
}

TEST_CASE(navigator_compatibility_reaches_every_agent)
{
    set_up_process();

    auto mode = Web::ResourceLoader::navigator_compatibility_mode();
    Web::ResourceLoader::set_navigator_compatibility_mode(Web::NavigatorCompatibilityMode::Gecko);
    run_on_agent_threads(2, [](size_t) {
        Core::EventLoop event_loop;
        TestAgent agent;
        EXPECT(Web::ResourceLoader::navigator_compatibility_mode() == Web::NavigatorCompatibilityMode::Gecko);
        check_pass(agent.run_pass(), 1);
        agent.close_and_shut_down();
    });
    Web::ResourceLoader::set_navigator_compatibility_mode(mode);
    EXPECT_EQ(s_failures.load(), 0u);
}

// NB: Style values call Rust FFI symbols directly, which the lagom-web DLL does not export.
#if !defined(AK_OS_WINDOWS)
TEST_CASE(interned_style_values_are_shared_between_threads)
{
    set_up_process();

    // NB: A count that races shows up as a value that gets freed, or as a VERIFY in the count itself.
    run_on_agent_threads(8, [](size_t) {
        for (size_t i = 0; i < 200'000; ++i) {
            auto keyword = Web::CSS::KeywordStyleValue::create(Web::CSS::Keyword::Auto);
            auto initial_value = Web::CSS::property_initial_value(Web::CSS::PropertyID::FontWeight);
            EXPECT(keyword->is_keyword());
        }
    });
}
#endif

TEST_CASE(main_thread_vm_refuses_a_worker)
{
    set_up_process();
    EXPECT_DEATH("main_thread_vm() without a window agent", (void)Web::Bindings::main_thread_vm());
}

// NB: This comes last, as the window agent stays for the life of the process.
TEST_CASE(agents_run_beside_a_window_agent)
{
    set_up_process();

    Core::EventLoop event_loop;
    Web::Bindings::initialize_agent(Web::HTML::AgentType::SimilarOriginWindow);
    auto realm = Web::Bindings::create_a_principal_javascript_realm();
    auto& window_vm = Web::Bindings::main_thread_vm();

    Atomic<size_t> running_agents { 4 };
    auto agent_work = [&](size_t) {
        Core::EventLoop agent_event_loop;
        TestAgent agent;
        check_pass(agent.run_pass(), 1);
        EXPECT_NE(&JS::VM::the(), &window_vm);
        check_pass(agent.run_pass(), 2);
        agent.close_and_shut_down();
        running_agents.fetch_sub(1);
    };
    auto threads = start_agent_threads(4, agent_work);

    static constexpr auto WINDOW_SCRIPT = R"~~~({
        const url = new URL("https://window.test/a?b=c");
        const blob = new Blob(["hello"]);
        const params = new URLSearchParams(url.search);
        params.get("b").length + blob.size + new TextEncoder().encode("x").length + 34;
    })~~~"sv;
    auto source = Utf16String::from_utf8_without_validation(WINDOW_SCRIPT.bytes());

    size_t window_runs = 0;
    while (running_agents.load() > 0 || window_runs == 0) {
        auto script = JS::Script::parse(source.utf16_view(), *realm);
        VERIFY(!script.is_error());
        auto result = window_vm.run(script.value());
        VERIFY(!result.is_error());
        EXPECT_EQ(result.value().as_double(), 41.0);
        ++window_runs;
    }

    for (auto& thread : threads)
        MUST(thread->join());
    EXPECT_EQ(&JS::VM::the(), &window_vm);
    EXPECT_EQ(s_failures.load(), 0u);
}

TEST_CASE(main_thread_vm_refuses_the_thread_of_an_agent)
{
    run_on_agent_threads(1, [](size_t) {
        Core::EventLoop event_loop;
        TestAgent agent;
        EXPECT_DEATH("main_thread_vm() on the thread of a worker", (void)Web::Bindings::main_thread_vm());
        agent.close_and_shut_down();
    });
}
