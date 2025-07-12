/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/JavaScriptTestRunner.h>
#include <signal.h>
#include <stdio.h>

namespace Test {

TestRunner* ::Test::TestRunner::s_the = nullptr;

namespace JS {

RefPtr<::JS::VM> g_vm;
bool g_collect_on_every_allocation = false;
ByteString g_currently_running_test;
HashMap<String, FunctionWithLength> s_exposed_global_functions;
Function<void()> g_main_hook;
HashMap<bool*, Tuple<ByteString, ByteString, char>> g_extra_args;
IntermediateRunFileResult (*g_run_file)(ByteString const&, JS::Realm&, JS::ExecutionContext&) = nullptr;
ByteString g_test_root;
int g_test_argc;
char** g_test_argv;

} // namespace JS
} // namespace Test

using namespace Test::JS;

static StringView g_program_name { "test-js"sv };

static bool set_abort_action(void (*function)(int))
{
#if defined(AK_OS_WINDOWS)
    auto rc = signal(SIGABRT, function);
    if (rc == SIG_ERR) {
        perror("sigaction");
        return false;
    }
    return true;
#else
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_flags = 0;
    act.sa_handler = function;
    int rc = sigaction(SIGABRT, &act, nullptr);
    if (rc < 0) {
        perror("sigaction");
        return false;
    }
    return true;
#endif
}

static void handle_sigabrt(int)
{
    dbgln("{}: SIGABRT received, cleaning up.", g_program_name);
    Test::cleanup();
    if (!set_abort_action(SIG_DFL))
        exit(1);
    abort();
}

int main(int argc, char** argv)
{
    Vector<StringView> arguments;
    arguments.ensure_capacity(argc);
    for (auto i = 0; i < argc; ++i)
        arguments.append({ argv[i], strlen(argv[i]) });

    g_test_argc = argc;
    g_test_argv = argv;
    auto program_name = LexicalPath::basename(argv[0]);
    g_program_name = program_name;

    if (!set_abort_action(handle_sigabrt))
        return 1;

#ifdef SIGINFO
    signal(SIGINFO, [](int) {
        static char buffer[4096];
        auto& counts = ::Test::TestRunner::the()->counts();
        int len = snprintf(buffer, sizeof(buffer), "Pass: %d, Fail: %d, Skip: %d\nCurrent test: %s\n", counts.tests_passed, counts.tests_failed, counts.tests_skipped, g_currently_running_test.characters());
        write(STDOUT_FILENO, buffer, len);
    });
#endif

    bool print_times = false;
    bool print_progress = false;
    bool print_json = false;
    bool per_file = false;
    StringView specified_test_root;
    ByteString common_path;
    Vector<ByteString> test_globs;

    Core::ArgsParser args_parser;
    args_parser.add_option(print_times, "Show duration of each test", "show-time", 't');
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Show progress with OSC 9 (true, false)",
        .long_name = "show-progress",
        .short_name = 'p',
        .accept_value = [&](StringView str) {
            if ("true"sv == str)
                print_progress = true;
            else if ("false"sv == str)
                print_progress = false;
            else
                return false;
            return true;
        },
    });

    args_parser.add_option(print_json, "Show results as JSON", "json", 'j');
    args_parser.add_option(per_file, "Show detailed per-file results as JSON (implies -j)", "per-file");
    args_parser.add_option(g_collect_on_every_allocation, "Collect garbage after every allocation", "collect-often", 'g');
    args_parser.add_option(JS::Bytecode::g_dump_bytecode, "Dump the bytecode", "dump-bytecode", 'd');
    args_parser.add_option(test_globs, "Only run tests matching the given glob", "filter", 'f', "glob");
    for (auto& entry : g_extra_args)
        args_parser.add_option(*entry.key, entry.value.get<0>().characters(), entry.value.get<1>().characters(), entry.value.get<2>());
    args_parser.add_positional_argument(specified_test_root, "Tests root directory", "path", Core::ArgsParser::Required::No);
    args_parser.add_positional_argument(common_path, "Path to tests-common.js", "common-path", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    if (per_file)
        print_json = true;

    for (auto& glob : test_globs)
        glob = ByteString::formatted("*{}*", glob);
    if (test_globs.is_empty())
        test_globs.append("*"sv);

    if (Core::Environment::has("DISABLE_DBG_OUTPUT"sv)) {
        AK::set_debug_enabled(false);
    }

    ByteString test_root;

    if (!specified_test_root.is_empty()) {
        test_root = ByteString { specified_test_root };
    } else {
        auto ladybird_source_dir = Core::Environment::get("LADYBIRD_SOURCE_DIR"sv);
        if (!ladybird_source_dir.has_value()) {
            warnln("No test root given, {} requires the LADYBIRD_SOURCE_DIR environment variable to be set", g_program_name);
            return 1;
        }
        test_root = LexicalPath::join(*ladybird_source_dir, g_test_root_fragment).string();
        common_path = LexicalPath::join(*ladybird_source_dir, "Libraries"sv, "LibJS"sv, "Tests"sv, "test-common.js"sv).string();
    }
    if (!FileSystem::is_directory(test_root)) {
        warnln("Test root is not a directory: {}", test_root);
        return 1;
    }

    if (common_path.is_empty()) {
        auto ladybird_source_dir = Core::Environment::get("LADYBIRD_SOURCE_DIR"sv);
        if (!ladybird_source_dir.has_value()) {
            warnln("No test root given, {} requires the LADYBIRD_SOURCE_DIR environment variable to be set", g_program_name);
            return 1;
        }
        common_path = LexicalPath::join(*ladybird_source_dir, "Libraries"sv, "LibJS"sv, "Tests"sv, "test-common.js"sv).string();
    }

    auto test_root_or_error = FileSystem::real_path(test_root);
    if (test_root_or_error.is_error()) {
        warnln("Failed to resolve test root: {}", test_root_or_error.error());
        return 1;
    }
    test_root = test_root_or_error.release_value();

    auto common_path_or_error = FileSystem::real_path(common_path);
    if (common_path_or_error.is_error()) {
        warnln("Failed to resolve common path: {}", common_path_or_error.error());
        return 1;
    }
    common_path = common_path_or_error.release_value();

    if (auto err = Core::System::chdir(test_root); err.is_error()) {
        warnln("chdir failed: {}", err.error());
        return 1;
    }

    if (g_main_hook)
        g_main_hook();

    if (!g_vm) {
        g_vm = JS::VM::create();
        g_vm->set_dynamic_imports_allowed(true);
    }

    Test::JS::TestRunner test_runner(test_root, common_path, print_times, print_progress, print_json, per_file);
    test_runner.run(test_globs);

    g_vm = nullptr;

    return test_runner.counts().tests_failed > 0 ? 1 : 0;
}
