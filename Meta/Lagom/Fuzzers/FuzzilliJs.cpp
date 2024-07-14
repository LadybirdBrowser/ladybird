/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/StringView.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Forward.h>
#include <LibJS/Lexer.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <errno.h>

#include <stddef.h>
#include <stdint.h>

#include <sys/mman.h>

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

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

#define SHM_SIZE 0x100000
#define MAX_EDGES ((SHM_SIZE - 4) * 8)

#define CHECK(cond)                                \
    if (!(cond)) {                                 \
        fprintf(stderr, "\"" #cond "\" failed\n"); \
        _exit(-1);                                 \
    }

struct shmem_data {
    uint32_t num_edges;
    unsigned char edges[];
};

struct shmem_data* __shmem;
uint32_t *__edges_start, *__edges_stop;

static void __sanitizer_cov_reset_edgeguards()
{
    uint64_t N = 0;
    for (uint32_t* x = __edges_start; x < __edges_stop && N < MAX_EDGES; x++)
        *x = ++N;
}

extern "C" void __sanitizer_cov_trace_pc_guard_init(uint32_t* start, uint32_t* stop)
{
    // Avoid duplicate initialization
    if (start == stop || *start)
        return;

    if (__edges_start != NULL || __edges_stop != NULL) {
        fprintf(stderr, "Coverage instrumentation is only supported for a single module\n");
        _exit(-1);
    }

    __edges_start = start;
    __edges_stop = stop;

    // Map the shared memory region
    char const* shm_key = getenv("SHM_ID");
    if (!shm_key) {
        puts("[COV] no shared memory bitmap available, skipping");
        __shmem = (struct shmem_data*)malloc(SHM_SIZE);
    } else {
        int fd = shm_open(shm_key, O_RDWR, S_IREAD | S_IWRITE);
        if (fd <= -1) {
            fprintf(stderr, "Failed to open shared memory region: %s\n", strerror(errno));
            _exit(-1);
        }

        __shmem = (struct shmem_data*)mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (__shmem == MAP_FAILED) {
            fprintf(stderr, "Failed to mmap shared memory region\n");
            _exit(-1);
        }
    }

    __sanitizer_cov_reset_edgeguards();

    __shmem->num_edges = stop - start;
    printf("[COV] edge counters initialized. Shared memory: %s with %u edges\n", shm_key, __shmem->num_edges);
}

extern "C" void __sanitizer_cov_trace_pc_guard(uint32_t* guard)
{
    // There's a small race condition here: if this function executes in two threads for the same
    // edge at the same time, the first thread might disable the edge (by setting the guard to zero)
    // before the second thread fetches the guard value (and thus the index). However, our
    // instrumentation ignores the first edge (see libcoverage.c) and so the race is unproblematic.
    uint32_t index = *guard;
    // If this function is called before coverage instrumentation is properly initialized we want to return early.
    if (!index)
        return;
    __shmem->edges[index / 8] |= 1 << (index % 8);
    *guard = 0;
}

//
// END FUZZING CODE
//

class TestRunnerGlobalObject final : public JS::GlobalObject {
    JS_OBJECT(TestRunnerGlobalObject, JS::GlobalObject);

public:
    TestRunnerGlobalObject(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~TestRunnerGlobalObject() override;

private:
    JS_DECLARE_NATIVE_FUNCTION(fuzzilli);
};

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

    auto operation = TRY(vm.argument(0).to_string(vm));
    if (operation == "FUZZILLI_CRASH") {
        auto type = TRY(vm.argument(1).to_i32(vm));
        switch (type) {
        case 0:
            *((int*)0x41414141) = 0x1337;
            break;
        default:
            VERIFY_NOT_REACHED();
            break;
        }
    } else if (operation == "FUZZILLI_PRINT") {
        static FILE* fzliout = fdopen(REPRL_DWFD, "w");
        if (!fzliout) {
            dbgln("Fuzzer output not available");
            fzliout = stdout;
        }

        auto string = TRY(vm.argument(1).to_string(vm));
        outln(fzliout, "{}", string);
        fflush(fzliout);
    }

    return JS::js_undefined();
}

void TestRunnerGlobalObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    define_direct_property("global", this, JS::Attribute::Enumerable);
    define_native_function(realm, "fuzzilli", fuzzilli, 2, JS::default_attributes);
}

int main(int, char**)
{
    char* reprl_input = nullptr;

    char helo[] = "HELO";
    if (write(REPRL_CWFD, helo, 4) != 4 || read(REPRL_CRFD, helo, 4) != 4) {
        VERIFY_NOT_REACHED();
    }

    VERIFY(memcmp(helo, "HELO", 4) == 0);
    reprl_input = (char*)mmap(0, REPRL_MAX_DATA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, REPRL_DRFD, 0);
    VERIFY(reprl_input != MAP_FAILED);

    auto vm = MUST(JS::VM::create());
    auto root_execution_context = JS::create_simple_execution_context<TestRunnerGlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;

    while (true) {
        unsigned action;
        VERIFY(read(REPRL_CRFD, &action, 4) == 4);
        VERIFY(action == 'cexe');

        size_t script_size;
        VERIFY(read(REPRL_CRFD, &script_size, 8) == 8);
        VERIFY(script_size < REPRL_MAX_DATA_SIZE);
        ByteBuffer data_buffer;
        data_buffer.resize(script_size);
        VERIFY(data_buffer.size() >= script_size);
        memcpy(data_buffer.data(), reprl_input, script_size);

        int result = 0;

        auto js = StringView(static_cast<unsigned char const*>(data_buffer.data()), script_size);

        // FIXME: https://github.com/SerenityOS/serenity/issues/17899
        if (!Utf8View(js).validate()) {
            result = 1;
        } else {
            auto parse_result = JS::Script::parse(js, realm);
            if (parse_result.is_error()) {
                result = 1;
            } else {
                auto completion = vm->bytecode_interpreter().run(parse_result.value());
                if (completion.is_error()) {
                    result = 1;
                }
            }
        }
        fflush(stdout);
        fflush(stderr);

        int status = (result & 0xff) << 8;
        VERIFY(write(REPRL_CWFD, &status, 4) == 4);
        __sanitizer_cov_reset_edgeguards();
    }

    return 0;
}
