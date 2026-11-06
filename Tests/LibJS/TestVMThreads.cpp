/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StackInfo.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/BlockAllocator.h>
#include <LibJS/Runtime/GlobalObject.h>
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
