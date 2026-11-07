/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/StackInfo.h>
#include <AK/Time.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/DeferGC.h>
#include <LibGC/HeapBlock.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

// Touches what a realm sets up lazily or caches outside of the VM: shapes, interned strings, regular expressions, typed
// arrays in primitive storage, and enough functions and allocations for the JIT and the collector to get involved.
static constexpr auto TEST_SCRIPT = R"(
    class Point {
        constructor(x, y) { this.x = x; this.y = y; }
        get length() { return Math.hypot(this.x, this.y); }
    }

    function work(seed) {
        const points = [];
        for (let i = 0; i < 2000; ++i)
            points.push(new Point(i, seed));

        const map = new Map();
        const weak = new WeakMap();
        for (const point of points) {
            map.set(`point-${point.x}`, point);
            weak.set(point, point.length);
        }

        const bytes = new Uint8Array(4096);
        bytes.fill(seed & 0xff);

        const words = "the quick brown fox jumps over the lazy dog".replace(/\b\w/gu, c => c.toUpperCase()).split(" ");
        const json = JSON.parse(JSON.stringify({ words, sum: bytes.reduce((a, b) => a + b, 0) }));
        const date = new Date(0).toISOString();

        let total = 0;
        for (const point of points)
            total += weak.get(map.get(`point-${point.x}`)) > 0 ? 1 : 0;
        return total + json.words.length + json.sum + date.length;
    }

    let result = 0;
    for (let i = 0; i < 20; ++i)
        result = work(1);
    try {
        null.property;
    } catch (error) {
        result += error.stack.length > 0 ? 1 : 0;
    }
    result;
)"sv;

// 2000 points, 9 words, 4096 bytes of 1, the 24 characters of the date, and the error.
static constexpr double EXPECTED_RESULT = 2000 + 9 + 4096 + 24 + 1;

static void run_script_in_a_new_vm()
{
    auto vm = JS::VM::create();
    EXPECT_EQ(&JS::VM::the(), vm.ptr());
    EXPECT_EQ(&GC::Heap::the(), &vm->heap());
    vm->heap().set_incremental_sweep_enabled(false);

    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto source_text = Utf16String::from_utf8_without_validation(TEST_SCRIPT.bytes());
    auto script = JS::Script::parse(source_text.utf16_view(), *root_execution_context->realm);
    VERIFY(!script.is_error());
    auto result = vm->run(script.value());
    VERIFY(!result.is_error());
    EXPECT_EQ(result.value().as_double(), EXPECTED_RESULT);
}

template<typename Callback>
static void run_on_vm_threads(size_t thread_count, IGNORE_USE_IN_ESCAPING_LAMBDA Callback callback)
{
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    for (size_t i = 0; i < thread_count; ++i) {
        auto thread = Threading::Thread::construct("VMThreadTest"sv, [&callback] {
            callback();
            return static_cast<intptr_t>(0);
        });
        thread->set_stack_size(JS::VM::MINIMUM_THREAD_STACK_SIZE);
        thread->start();
        threads.append(move(thread));
    }
    for (auto& thread : threads)
        MUST(thread->join());
}

TEST_CASE(vm_threads_get_the_stack_they_asked_for)
{
    run_on_vm_threads(1, [] {
        EXPECT(StackInfo {}.size() >= JS::VM::MINIMUM_THREAD_STACK_SIZE);
    });
}

TEST_CASE(a_vm_runs_on_a_thread_that_is_not_the_main_thread)
{
    run_on_vm_threads(1, [] { run_script_in_a_new_vm(); });
}

TEST_CASE(several_vms_run_at_once)
{
    run_on_vm_threads(8, [] { run_script_in_a_new_vm(); });
}

TEST_CASE(vms_come_and_go_while_others_run)
{
    run_on_vm_threads(4, [] {
        for (size_t i = 0; i < 5; ++i)
            run_script_in_a_new_vm();
    });
}

TEST_CASE(a_vm_on_the_main_thread_runs_beside_vms_on_other_threads)
{
    auto vm = JS::VM::create();
    run_on_vm_threads(4, [] { run_script_in_a_new_vm(); });
    EXPECT_EQ(&JS::VM::the(), vm.ptr());
}

TEST_CASE(heap_blocks_come_back_when_vms_go_away)
{
    auto run_round = [] {
        run_on_vm_threads(4, [] { run_script_in_a_new_vm(); });
    };

    run_round();
    auto committed_end = GC::BlockAllocator::heap_region_committed_end();
    for (size_t i = 0; i < 10; ++i)
        run_round();
    // NB: Timing can make the threads of a round overlap more than those of the first one, so allow for a few chunks.
    //     A round that leaked its blocks would take more than all of this.
    EXPECT(GC::BlockAllocator::heap_region_committed_end() <= committed_end + 16 * MiB);
}

namespace {

// What a block holds, in terms that mean the same in every heap: the descriptor of a Rust type goes with its heap.
struct BlockContents {
    void const* type { nullptr };
    size_t cell_size { 0 };

    bool operator==(BlockContents const&) const = default;
};

}

static HashMap<FlatPtr, BlockContents> contents_of_blocks(GC::Heap& heap)
{
    HashMap<FlatPtr, BlockContents> blocks;
    heap.for_each_block([&](GC::HeapBlock& block) {
        auto const* foreign_type = block.foreign_type();
        auto const* type = foreign_type ? foreign_type : &block.cell_allocator().descriptor();
        blocks.set(bit_cast<FlatPtr>(&block), { type, block.cell_size() });
        return IterationDecision::Continue;
    });
    return blocks;
}

static void run_script(JS::VM& vm, JS::Realm& realm, StringView source)
{
    auto source_text = Utf16String::from_utf8_without_validation(source.bytes());
    auto script = JS::Script::parse(source_text.utf16_view(), realm);
    VERIFY(!script.is_error());
    VERIFY(!vm.run(script.value()).is_error());
}

TEST_CASE(block_addresses_stay_with_their_type_across_vms)
{
    // Keeps what it allocates, as the blocks are looked at while the VM still has them.
    static constexpr auto FIRST_SCRIPT = R"(
        globalThis.kept = [];
        for (let i = 0; i < 40000; ++i)
            kept.push({ i }, [i], `string-${i}`, () => i, new Map(), Symbol("s"));
    )"sv;
    // Different types, in different amounts.
    static constexpr auto SECOND_SCRIPT = R"(
        globalThis.kept = [];
        for (let i = 0; i < 40000; ++i)
            kept.push(new Date(i), new Set([i]), new Uint8Array(4), /a/g, new Error("e"), `string-${i}`, { i, j: i, k: i, l: i });
    )"sv;

    HashMap<FlatPtr, BlockContents> first_blocks;
    run_on_vm_threads(1, [&] {
        auto vm = JS::VM::create();
        vm->heap().set_incremental_sweep_enabled(false);
        auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
        run_script(*vm, *root_execution_context->realm, FIRST_SCRIPT);
        first_blocks = contents_of_blocks(vm->heap());
    });

    run_on_vm_threads(1, [&] {
        auto vm = JS::VM::create();
        vm->heap().set_incremental_sweep_enabled(false);
        auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
        run_script(*vm, *root_execution_context->realm, SECOND_SCRIPT);

        size_t reused_blocks = 0;
        for (auto const& [address, contents] : contents_of_blocks(vm->heap())) {
            auto first_contents = first_blocks.get(address);
            if (!first_contents.has_value())
                continue;
            ++reused_blocks;
            EXPECT(first_contents.value() == contents);
        }
        // Realms, shapes and strings, at least.
        EXPECT(reused_blocks > 10);
    });
}

TEST_CASE(a_host_class_keeps_its_blocks_across_vms)
{
    static JSHostClass const host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "TestHostObject"sv, nullptr, nullptr, nullptr, 0);

    auto allocate_host_objects = [](JS::VM& vm) {
        auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(vm);
        GC::DeferGC defer_gc { vm.heap() };
        HashTable<FlatPtr> blocks;
        while (blocks.size() < 16) {
            auto object = JS::HostObject::create(*root_execution_context->realm, host_class, nullptr);
            blocks.set(bit_cast<FlatPtr>(GC::HeapBlock::from_cell(GC::as_cell(object.ptr()))));
        }
        return blocks;
    };

    HashTable<FlatPtr> first_blocks;
    run_on_vm_threads(1, [&] {
        auto vm = JS::VM::create();
        first_blocks = allocate_host_objects(*vm);
    });
    run_on_vm_threads(1, [&] {
        auto vm = JS::VM::create();
        for (auto block : allocate_host_objects(*vm))
            EXPECT(first_blocks.contains(block));
    });
}

// Scripts that never end by themselves. Those that set `reached` show what an interrupt must not let run.
static constexpr StringView NEVER_ENDING_SCRIPTS[] = {
    "while (true) {}"sv,
    "let i = 0; for (;;) i++;"sv,
    "let i = 0; for (;;) i = (i + 1) | 0;"sv,
    "for (;;) { for (let i = 0; i < 1000; ++i) {} }"sv,
    "function spin() { let i = 0; while (true) i += 2; } spin();"sv,
    "const array = [1, 2, 3]; function sum() { let s = 0; for (let i = 0; i < array.length; ++i) s += array[i]; return s; } while (true) sum();"sv,

    // Generators and async functions.
    "function* forever() { for (;;) yield 1; } for (const value of forever()) {}"sv,
    "function* forever() { while (true) {} } forever().next();"sv,
    "async function forever() { while (true) {} } forever();"sv,

    // Nothing may catch it or run after it.
    "try { while (true) {} } catch (error) { globalThis.reached = true; }"sv,
    "try { while (true) {} } finally { globalThis.reached = true; while (true) {} }"sv,
    "function caught() { try { while (true) {} } finally { globalThis.reached = true; } } try { caught(); } catch (error) { globalThis.reached = true; }"sv,
    "while (true) { try { while (true) {} } catch (error) { globalThis.reached = true; } }"sv,
    "while (true) { try { null.property; } catch (error) {} }"sv,
    "const iterator = { [Symbol.iterator]() { return this; }, next() { return { value: 1, done: false }; }, return() { globalThis.reached = true; return {}; } }; for (const value of iterator) { while (true) {} }"sv,
    "function* withFinally() { try { yield 1; while (true) {} } finally { globalThis.reached = true; } } for (const value of withFinally()) {}"sv,

    // Native code that catches, and calls that run script.
    "while (true) { new Promise(() => { throw 1; }); }"sv,
    "[1].forEach(function again() { [1].forEach(() => {}); while (true) {} });"sv,
    "[3, 2, 1].sort(() => { while (true) {} });"sv,
    "new Promise(() => { while (true) {} }).catch(() => { globalThis.reached = true; });"sv,
    "({ get property() { while (true) {} } }).property;"sv,
    "class Thing { constructor() { while (true) {} } } new Thing();"sv,
    "eval('while (true) {}');"sv,
    "new Proxy({}, { get() { while (true) {} } }).property;"sv,
    "`${{ toString() { while (true) {} } }}`;"sv,

    // Recursion, also without any loop.
    "function deep() { deep(); } while (true) { try { deep(); } catch (error) {} }"sv,
    "function wide(n) { return n == 0 ? 0 : wide(n - 1) + wide(n - 1); } wide(100);"sv,

    // Native code that does not run script.
    "/(a|aa)+$/.test('a'.repeat(50000) + '!');"sv,
    "/(a|aa)+$/u.test('a'.repeat(50000) + '!');"sv,
    "Atomics.wait(new Int32Array(new SharedArrayBuffer(8)), 0, 0);"sv,
    "Atomics.wait(new Int32Array(new SharedArrayBuffer(8)), 0, 0, 1e9);"sv,
    "Atomics.wait(new BigInt64Array(new SharedArrayBuffer(8)), 0, 0n);"sv,
    "while (true) Atomics.wait(new Int32Array(new SharedArrayBuffer(8)), 0, 0, 1);"sv,
};

// These only run in promise jobs, after the script itself completed normally. Each job calls jobRan(), so that the
// test interrupts them only once they are in one.
static constexpr StringView NEVER_ENDING_PROMISE_JOBS[] = {
    "async function forever() { for (;;) { await 0; jobRan(); } } forever();"sv,
    "async function* forever() { for (;;) yield 1; } (async () => { for await (const value of forever()) jobRan(); })();"sv,
    "function again() { Promise.resolve().then(() => { jobRan(); again(); }); } again();"sv,
    "function again() { Promise.resolve().then(() => { throw 1; }).catch(() => { jobRan(); again(); }); } again();"sv,
};

// NB: The native function the script calls sets this, so it is shared with the function's captures.
struct JobRanFlag : AtomicRefCounted<JobRanFlag> {
    GC_HOLDS_NO_CELL_EDGES;

    Atomic<bool> value { false };
};

struct InterruptedScript {
    AK_ALLOC_WITH_KMALLOC;

    StringView source;
    bool is_promise_job { false };
    Atomic<JS::VM*> vm { nullptr };
    NonnullRefPtr<JobRanFlag> job_ran { make_ref_counted<JobRanFlag>() };
    bool did_throw { false };
    bool reached { false };
    MonotonicTime interrupted_at { MonotonicTime::now() };
    MonotonicTime stopped_at { MonotonicTime::now() };
};

static void run_script_until_it_is_interrupted(InterruptedScript& interrupted)
{
    auto vm = JS::VM::create();
    vm->heap().set_incremental_sweep_enabled(false);

    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;
    auto job_ran = JS::NativeFunction::create(realm, GC::create_function(vm->heap(), GC::captures(interrupted.job_ran), [](NonnullRefPtr<JobRanFlag>& job_ran, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
        job_ran->value = true;
        return JS::js_undefined(); }), 0, JS::PropertyKey { "jobRan"_utf16_fly_string });
    realm.global_object().define_direct_property(JS::PropertyKey { "jobRan"_utf16_fly_string }, job_ran, JS::default_attributes);
    auto source_text = Utf16String::from_utf8_without_validation(interrupted.source.bytes());
    auto script = JS::Script::parse(source_text.utf16_view(), realm);
    VERIFY(!script.is_error());

    interrupted.vm = vm.ptr();
    auto result = vm->run(script.value());
    interrupted.did_throw = result.is_error();

    // NB: Nothing runs on this VM anymore, but the property holds a plain value.
    auto reached = MUST(realm.global_object().get(JS::PropertyKey { "reached"_utf16_fly_string }));
    interrupted.reached = !reached.is_undefined();

    interrupted.stopped_at = MonotonicTime::now();
    if (!vm->interrupt_requested())
        warnln("This ended by itself: {}", interrupted.source);
    EXPECT(vm->interrupt_requested());

    // NB: The thread that interrupts this VM has to be done with it first.
    while (interrupted.vm)
        MUST(Core::System::sleep_ms(1));
}

// NB: The scripts run at the same time, and every JIT mode is another round, as VMs read LIBJS_JIT when they start.
static void interrupt_every_never_ending_script(StringView jit_options)
{
    MUST(Core::Environment::set("LIBJS_JIT"sv, jit_options, Core::Environment::Overwrite::Yes));

    Vector<NonnullOwnPtr<InterruptedScript>> scripts;
    Vector<NonnullRefPtr<Threading::Thread>> threads;
    auto start_script = [&](StringView source, bool is_promise_job) {
        auto script = make<InterruptedScript>();
        script->source = source;
        script->is_promise_job = is_promise_job;
        auto thread = Threading::Thread::construct("VMThreadTest"sv, [&interrupted = *script] {
            run_script_until_it_is_interrupted(interrupted);
            return static_cast<intptr_t>(0);
        });
        thread->set_stack_size(JS::VM::MINIMUM_THREAD_STACK_SIZE);
        thread->start();
        scripts.append(move(script));
        threads.append(move(thread));
    };
    for (auto source : NEVER_ENDING_SCRIPTS)
        start_script(source, false);
    for (auto source : NEVER_ENDING_PROMISE_JOBS)
        start_script(source, true);

    // NB: Nothing tells when a script got into compiled code, but the modes that compile without a warmup take a few
    //     iterations to do so. A slow machine may still be in the interpreter here, which this test covers as well.
    MUST(Core::System::sleep_ms(50));

    for (auto& script : scripts) {
        while (!script->vm || (script->is_promise_job && !script->job_ran->value))
            MUST(Core::System::sleep_ms(1));
        script->interrupted_at = MonotonicTime::now();
        script->vm.load()->request_interrupt();
        script->vm = nullptr;
    }
    for (auto& thread : threads)
        MUST(thread->join());
    MUST(Core::Environment::unset("LIBJS_JIT"sv));

    for (auto const& script : scripts) {
        if (script->did_throw == script->is_promise_job || script->reached)
            warnln("Interrupting this went wrong: {}", script->source);
        EXPECT_NE(script->did_throw, script->is_promise_job);
        EXPECT(!script->reached);
        // NB: This has to hold on a busy machine as well, and only a script that does not stop gets anywhere near it.
        EXPECT(script->stopped_at - script->interrupted_at < AK::Duration::from_seconds(5));
    }
}

TEST_CASE(interrupting_script_in_the_interpreter)
{
    interrupt_every_never_ending_script("off"sv);
}

TEST_CASE(interrupting_script_in_compiled_code)
{
    interrupt_every_never_ending_script("on,sync,threshold=1,warmup=0"sv);
}

TEST_CASE(interrupting_script_that_tiers_up_on_the_way)
{
    interrupt_every_never_ending_script("on,threshold=100,warmup=2"sv);
}

TEST_CASE(interrupting_script_that_has_no_feedback)
{
    interrupt_every_never_ending_script("on,sync,threshold=0,warmup=0"sv);
}

TEST_CASE(an_interrupted_vm_runs_nothing_anymore)
{
    run_on_vm_threads(1, [] {
        auto vm = JS::VM::create();
        auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
        auto run = [&](StringView source) {
            auto source_text = Utf16String::from_utf8_without_validation(source.bytes());
            auto script = JS::Script::parse(source_text.utf16_view(), *root_execution_context->realm);
            VERIFY(!script.is_error());
            return vm->run(script.value());
        };

        EXPECT(!run("globalThis.count = 1;"sv).is_error());
        vm->request_interrupt();
        EXPECT(run("globalThis.count = 2;"sv).is_error());

        auto count = MUST(root_execution_context->realm->global_object().get(JS::PropertyKey { "count"_utf16_fly_string }));
        EXPECT_EQ(count.as_i32(), 1);
    });
}

// The words of the memory that the scripts below share, each of which sees it as the Int32Array `words`. Only the word
// they wait on is touched by anything that could wake them.
static constexpr size_t WAIT_WORD = 0;
static constexpr size_t STARTED_COUNT_WORD = 1;
static constexpr size_t WOKEN_COUNT_WORD = 2;
static constexpr size_t RESULT_WORDS = 8;

// What a script that waits leaves in its result word. Word 0 stays what it is unless a test changes it.
static constexpr i32 RESULT_OK = 1;
static constexpr i32 RESULT_NOT_EQUAL = 2;
static constexpr i32 RESULT_TIMED_OUT = 3;

class SharedMemoryScripts {
public:
    SharedMemoryScripts()
    {
        // NB: The agent that created the memory keeps it for the test, as the others come and go.
        auto owner = Threading::Thread::construct("VMThreadTestOwner"sv, [this] {
            own_memory();
            return static_cast<intptr_t>(0);
        });
        owner->set_stack_size(JS::VM::MINIMUM_THREAD_STACK_SIZE);
        owner->start();
        m_threads.append(move(owner));
        while (!m_words.load())
            MUST(Core::System::sleep_ms(1));
    }

    ~SharedMemoryScripts()
    {
        m_may_destroy_vms = true;
        for (auto& thread : m_threads)
            MUST(thread->join());
    }

    // Runs a script on a VM and thread of its own, which shares the memory as a worker's agent would.
    void start(ByteString source, Optional<size_t> index = {})
    {
        auto thread = Threading::Thread::construct("VMThreadTest"sv, [this, source = move(source), index] {
            run(source, index);
            return static_cast<intptr_t>(0);
        });
        thread->set_stack_size(JS::VM::MINIMUM_THREAD_STACK_SIZE);
        thread->start();
        m_threads.append(move(thread));
    }

    // Starts a script that waits on WAIT_WORD, and returns once it is about to. Its result goes in RESULT_WORDS + index,
    // after it counted itself as woken: the result is the count, plus the outcome in the upper bits.
    void start_waiter(size_t index, StringView timeout = "Infinity"sv)
    {
        auto started = word(STARTED_COUNT_WORD).load();
        start(ByteString::formatted(R"(
            const outcomes = {{ ok: {}, "not-equal": {}, "timed-out": {} }};
            Atomics.add(words, {}, 1);
            const outcome = outcomes[Atomics.wait(words, {}, 0, {})];
            const position = Atomics.add(words, {}, 1) + 1;
            Atomics.store(words, {}, outcome * 256 + position);
        )",
                  RESULT_OK, RESULT_NOT_EQUAL, RESULT_TIMED_OUT, STARTED_COUNT_WORD, WAIT_WORD, timeout, WOKEN_COUNT_WORD, RESULT_WORDS + index),
            index);
        while (word(STARTED_COUNT_WORD).load() == started)
            MUST(Core::System::sleep_ms(1));
        // NB: A wait with a short timeout may be over before this sees it.
        while (!has_result(index) && !m_vms[index].load()->is_waiting_on_shared_memory())
            MUST(Core::System::sleep_ms(1));
    }

    // What Atomics.notify() returned, in a VM of its own.
    i32 notify(i32 count)
    {
        static constexpr size_t NOTIFY_RESULT_WORD = RESULT_WORDS - 1;
        word(NOTIFY_RESULT_WORD).store(-1);
        start(ByteString::formatted("Atomics.store(words, {}, Atomics.notify(words, {}, {}));", NOTIFY_RESULT_WORD, WAIT_WORD, count));
        while (word(NOTIFY_RESULT_WORD).load() == -1)
            MUST(Core::System::sleep_ms(1));
        return word(NOTIFY_RESULT_WORD).load();
    }

    i32 wait_for_result(size_t index)
    {
        while (word(RESULT_WORDS + index).load() == 0)
            MUST(Core::System::sleep_ms(1));
        return word(RESULT_WORDS + index).load();
    }

    bool has_result(size_t index) { return word(RESULT_WORDS + index).load() != 0; }

    // Stops a script from start_waiter(), and returns whether its wait ended as an interrupt: the script threw, and ran
    // nothing past the wait.
    bool interrupt_and_check_that_it_ended(size_t index)
    {
        m_vms[index].load()->request_interrupt();
        while (!m_is_finished[index])
            MUST(Core::System::sleep_ms(1));
        return m_did_throw[index] && !has_result(index);
    }

    AK::Atomic<i32>& word(size_t index) { return m_words.load()[index]; }

private:
    void own_memory()
    {
        auto vm = JS::VM::create();
        vm->heap().set_incremental_sweep_enabled(false);
        auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
        auto& realm = *root_execution_context->realm;

        auto source_text = "new SharedArrayBuffer(4096)"_utf16;
        auto script = JS::Script::parse(source_text.utf16_view(), realm);
        VERIFY(!script.is_error());
        auto& buffer = as<JS::ArrayBuffer>(vm->run(script.value()).value().as_object());
        EXPECT_NE(buffer.shared_store_id(), 0u);
        m_shared_store_id = buffer.shared_store_id();
        m_words = reinterpret_cast<AK::Atomic<i32>*>(const_cast<u8*>(buffer.data_at(0)));

        while (!m_may_destroy_vms)
            MUST(Core::System::sleep_ms(1));
    }

    void run(ByteString const& source, Optional<size_t> index)
    {
        auto vm = JS::VM::create();
        vm->heap().set_incremental_sweep_enabled(false);
        auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
        auto& realm = *root_execution_context->realm;

        auto buffer = JS::ArrayBuffer::create_from_shared_store(realm, m_shared_store_id.load());
        VERIFY(buffer);
        MUST(realm.global_object().set(JS::PropertyKey { "memory"_utf16_fly_string }, buffer, JS::Object::ShouldThrowExceptions::Yes));

        auto text = ByteString::formatted("const words = new Int32Array(memory); {}", source);
        auto source_text = Utf16String::from_utf8_without_validation(text.bytes());
        auto script = JS::Script::parse(source_text.utf16_view(), realm);
        VERIFY(!script.is_error());
        if (index.has_value())
            m_vms[*index] = vm.ptr();
        auto result = vm->run(script.value());
        if (!index.has_value())
            return;
        m_did_throw[*index] = result.is_error();
        m_is_finished[*index] = true;

        // NB: The thread that interrupts this VM has to be done with it first.
        while (!m_may_destroy_vms)
            MUST(Core::System::sleep_ms(1));
    }

    static constexpr size_t MAX_WAITER_COUNT = 8;

    AK::Atomic<u64> m_shared_store_id { 0 };
    AK::Atomic<AK::Atomic<i32>*> m_words { nullptr };
    Vector<NonnullRefPtr<Threading::Thread>> m_threads;
    Array<AK::Atomic<JS::VM*>, MAX_WAITER_COUNT> m_vms;
    Array<AK::Atomic<bool>, MAX_WAITER_COUNT> m_did_throw;
    Array<AK::Atomic<bool>, MAX_WAITER_COUNT> m_is_finished;
    AK::Atomic<bool> m_may_destroy_vms { false };
};

TEST_CASE(a_notify_wakes_a_waiter_of_a_word_that_did_not_change)
{
    SharedMemoryScripts scripts;
    scripts.start_waiter(0);
    EXPECT_EQ(scripts.notify(1), 1);
    EXPECT_EQ(scripts.wait_for_result(0), RESULT_OK * 256 + 1);
    EXPECT_EQ(scripts.word(WAIT_WORD).load(), 0);
}

TEST_CASE(a_word_that_changes_without_a_notify_ends_no_wait)
{
    SharedMemoryScripts scripts;
    scripts.start_waiter(0, "30"sv);
    scripts.word(WAIT_WORD).store(1);
    EXPECT_EQ(scripts.wait_for_result(0), RESULT_TIMED_OUT * 256 + 1);
}

TEST_CASE(notifies_wake_in_order_and_count_every_waiter)
{
    static constexpr size_t WAITER_COUNT = 5;
    SharedMemoryScripts scripts;
    for (size_t i = 0; i < WAITER_COUNT; ++i)
        scripts.start_waiter(i);

    EXPECT_EQ(scripts.notify(0), 0);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(scripts.notify(1), 1);
        EXPECT_EQ(scripts.wait_for_result(i), RESULT_OK * 256 + static_cast<i32>(i) + 1);
    }
    EXPECT(!scripts.has_result(2));

    EXPECT_EQ(scripts.notify(100), static_cast<i32>(WAITER_COUNT) - 2);
    for (size_t i = 2; i < WAITER_COUNT; ++i)
        EXPECT_EQ(scripts.wait_for_result(i) / 256, RESULT_OK);
    EXPECT_EQ(scripts.notify(100), 0);
}

TEST_CASE(an_interrupt_ends_a_wait_as_terminated_and_leaves_other_waiters_alone)
{
    SharedMemoryScripts scripts;
    scripts.start_waiter(0);
    scripts.start_waiter(1, "600000"sv);
    scripts.start_waiter(2);
    scripts.start_waiter(3, "600000"sv);

    EXPECT(scripts.interrupt_and_check_that_it_ended(0));
    EXPECT(scripts.interrupt_and_check_that_it_ended(1));
    EXPECT(!scripts.has_result(2));
    EXPECT(!scripts.has_result(3));

    // NB: The waiters that are left kept their places.
    EXPECT_EQ(scripts.notify(1), 1);
    EXPECT_EQ(scripts.wait_for_result(2), RESULT_OK * 256 + 1);
    EXPECT_EQ(scripts.notify(100), 1);
    EXPECT_EQ(scripts.wait_for_result(3), RESULT_OK * 256 + 2);
}
