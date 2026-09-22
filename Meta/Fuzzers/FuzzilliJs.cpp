/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf8View.h>
#include <LibGC/Heap.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <errno.h>

#include <stddef.h>
#include <stdint.h>

#include <sys/mman.h>

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// These are hooks into sancov's internals, their declaration isn't available in public headers.
// See compiler-rt/lib/sanitizer_common/sanitizer_interface_internal.h
extern "C" {
void __sanitizer_cov_trace_pc_guard_init(uint32_t*, uint32_t*);
void __sanitizer_cov_trace_pc_guard(uint32_t*);
}

//
// BEGIN FUZZING CODE
//

#define REPRL_CRFD 100
#define REPRL_CWFD 101
#define REPRL_DRFD 102
#define REPRL_DWFD 103
#define REPRL_MAX_DATA_SIZE (16 * 1024 * 1024)

#define SHM_SIZE 0x200000
// Fuzzilli reserves bit zero and rounds bitmap reads to eight-byte words.
#define MAX_EDGES (((SHM_SIZE - 4) / 8) * 64 - 1)

#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            fprintf(stderr, "REPRL harness: %s failed\n", #cond); \
            _exit(1);                                             \
        }                                                         \
    } while (0)

struct shmem_data {
    uint32_t num_edges;
    unsigned char edges[];
};

static shmem_data* s_shmem;
static uint32_t *s_edges_start, *s_edges_stop;

static void reset_edgeguards()
{
    uint32_t edge = 0;
    for (uint32_t* guard = s_edges_start; guard < s_edges_stop; ++guard)
        __atomic_store_n(guard, ++edge, __ATOMIC_RELAXED);
}

extern "C" void __sanitizer_cov_trace_pc_guard_init(uint32_t* start, uint32_t* stop)
{
    // Avoid duplicate initialization
    if (start == stop || (start == s_edges_start && stop == s_edges_stop))
        return;

    CHECK(s_edges_start == nullptr && s_edges_stop == nullptr);
    CHECK(static_cast<size_t>(stop - start) <= MAX_EDGES);
    s_edges_start = start;
    s_edges_stop = stop;

    // Map the shared memory region
    auto const* shm_key = getenv("SHM_ID");
    CHECK(shm_key && *shm_key);
    int fd = shm_open(shm_key, O_RDWR, 0);
    CHECK(fd >= 0);
    struct stat metadata {};
    CHECK(fstat(fd, &metadata) == 0 && metadata.st_size >= SHM_SIZE);
    s_shmem = static_cast<shmem_data*>(mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    CHECK(s_shmem != MAP_FAILED);
    CHECK(close(fd) == 0);

    reset_edgeguards();
    s_shmem->num_edges = static_cast<uint32_t>(stop - start);
}

extern "C" void __sanitizer_cov_trace_pc_guard(uint32_t* guard)
{
    // Runtime support may use background threads. Neither deduplication nor two
    // edges in the same bitmap byte should race with another callback.
    uint32_t index = __atomic_exchange_n(guard, 0, __ATOMIC_RELAXED);
    // If this function is called before coverage instrumentation is properly initialized we want to return early.
    if (!index)
        return;
    __atomic_fetch_or(&s_shmem->edges[index / 8], 1 << (index % 8), __ATOMIC_RELAXED);
}

//
// END FUZZING CODE
//

class TestRunnerGlobalObject final : public JS::GlobalObject {
    JS_OBJECT(TestRunnerGlobalObject, JS::GlobalObject);
    GC_DECLARE_ALLOCATOR(TestRunnerGlobalObject);

public:
    TestRunnerGlobalObject(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~TestRunnerGlobalObject() override;

private:
    JS_DECLARE_NATIVE_FUNCTION(fuzzilli);
    JS_DECLARE_NATIVE_FUNCTION(gc);
};

GC_DEFINE_ALLOCATOR(TestRunnerGlobalObject);

TestRunnerGlobalObject::TestRunnerGlobalObject(JS::Realm& realm)
    : GlobalObject(realm)
{
}

TestRunnerGlobalObject::~TestRunnerGlobalObject()
{
}

JS_DEFINE_NATIVE_FUNCTION(TestRunnerGlobalObject::fuzzilli)
{
    if (!vm.argument_count())
        return JS::js_undefined();

    auto operation = TRY(vm.argument(0).to_utf16_string(vm));
    if (operation == "FUZZILLI_CRASH"sv) {
        auto type = TRY(vm.argument(1).to_i32(vm));
        switch (type) {
        case 0:
            *((int*)0x41414141) = 0x1337;
            break;
        default:
            VERIFY_NOT_REACHED();
            break;
        }
    } else if (operation == "FUZZILLI_PRINT"sv) {
        static FILE* fzliout = fdopen(REPRL_DWFD, "w");
        if (!fzliout) {
            dbgln("Fuzzer output not available");
            fzliout = stdout;
        }

        auto string = TRY(vm.argument(1).to_utf16_string(vm)).to_utf8_but_should_be_ported_to_utf16();
        outln(fzliout, "{}", string);
        fflush(fzliout);
    }

    return JS::js_undefined();
}

JS_DEFINE_NATIVE_FUNCTION(TestRunnerGlobalObject::gc)
{
    vm.heap().collect_garbage();
    return JS::js_undefined();
}

void TestRunnerGlobalObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    define_direct_property("global"_utf16_fly_string, this, JS::Attribute::Enumerable);
    define_native_function(realm, "fuzzilli"_utf16_fly_string, fuzzilli, 2, JS::default_attributes);
    define_native_function(realm, "gc"_utf16_fly_string, gc, 0, JS::default_attributes);
}

static bool read_exact(int fd, void* data, size_t length, bool* clean_eof = nullptr)
{
    auto original_length = length;
    auto* bytes = static_cast<unsigned char*>(data);
    while (length) {
        auto count = read(fd, bytes, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (clean_eof)
                *clean_eof = count == 0 && length == original_length;
            return false;
        }
        bytes += count;
        length -= count;
    }
    return true;
}

static bool write_exact(int fd, void const* data, size_t length)
{
    auto const* bytes = static_cast<unsigned char const*>(data);
    while (length) {
        auto count = write(fd, bytes, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes += count;
        length -= count;
    }
    return true;
}

static int execute_script(StringView js)
{
    if (!Utf8View(js).validate())
        return 1;

    // A REPRL process is persistent, but a test case must not inherit globals,
    // lexical declarations, prototypes, roots, or queued jobs from another case.
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<TestRunnerGlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;
    auto source_text = Utf16String::from_utf8_without_validation(js);
    auto parse_result = JS::Script::parse(source_text.utf16_view(), realm);
    if (parse_result.is_error())
        return 1;
    auto completion = vm->run(parse_result.value());
    // These queues, like script execution, require the external REPRL timeout.
    vm->run_queued_promise_jobs();
    vm->run_queued_finalization_registry_cleanup_jobs();
    vm->run_queued_promise_jobs();
    return completion.is_error() ? 1 : 0;
}

int main(int, char**)
{
    AK::set_debug_enabled(false);
    // The adapter is uninstrumented in the dedicated build; require engine guards.
    CHECK(s_shmem && s_shmem->num_edges);
    char helo[] = "HELO";
    CHECK(write_exact(REPRL_CWFD, helo, 4));
    CHECK(read_exact(REPRL_CRFD, helo, 4));
    CHECK(memcmp(helo, "HELO", 4) == 0);
    struct stat metadata {};
    CHECK(fstat(REPRL_DRFD, &metadata) == 0 && metadata.st_size >= REPRL_MAX_DATA_SIZE);
    auto* reprl_input = static_cast<unsigned char*>(mmap(nullptr, REPRL_MAX_DATA_SIZE, PROT_READ, MAP_SHARED, REPRL_DRFD, 0));
    CHECK(reprl_input != MAP_FAILED);

    while (true) {
        char action[4];
        bool clean_eof = false;
        if (!read_exact(REPRL_CRFD, action, sizeof(action), &clean_eof)) {
            CHECK(clean_eof);
            CHECK(munmap(reprl_input, REPRL_MAX_DATA_SIZE) == 0);
            return 0;
        }
        CHECK(memcmp(action, "exec", sizeof(action)) == 0);
        uint64_t script_size;
        CHECK(read_exact(REPRL_CRFD, &script_size, sizeof(script_size)));
        CHECK(script_size <= REPRL_MAX_DATA_SIZE);
        reset_edgeguards();
        auto result = execute_script(StringView(reprl_input, static_cast<size_t>(script_size)));
        fflush(stdout);
        fflush(stderr);

        uint32_t status = (result & 0xff) << 8;
        CHECK(write_exact(REPRL_CWFD, &status, sizeof(status)));
    }
}
