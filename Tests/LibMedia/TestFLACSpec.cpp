/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibMedia/Audio/FlacLoader.h>
#include <LibTest/TestCase.h>

struct DiscoverFLACTestsHack {
    DiscoverFLACTestsHack()
    {
        // FIXME: Also run (our own) tests in this directory.
        (void)Core::Directory::for_each_entry("./FLAC/SpecTests"sv, Core::DirIterator::Flags::SkipParentAndBaseDir, [](auto const& entry, auto const& directory) -> ErrorOr<IterationDecision> {
            auto path = LexicalPath::join(directory.path().string(), entry.name);
            if (path.extension() == "flac"sv) {
                Test::add_test_case_to_suite(adopt_ref(*new ::Test::TestCase(
                    ByteString::formatted("flac_spec_test_{}", path.basename()),
                    [path = move(path)]() {
                        auto file = Core::File::open(path.string(), Core::File::OpenMode::Read);
                        if (file.is_error()) {
                            FAIL(ByteString::formatted("{}", file.error()));
                            return;
                        }
                        auto buffered_file = Core::InputBufferedFile::create(file.release_value());
                        if (buffered_file.is_error()) {
                            FAIL(ByteString::formatted("{}", buffered_file.error()));
                            return;
                        }
                        auto result = Audio::FlacLoaderPlugin::create(buffered_file.release_value());
                        if (result.is_error()) {
                            FAIL(ByteString::formatted("{}", result.error()));
                            return;
                        }

                        auto loader = result.release_value();

                        while (true) {
                            auto maybe_samples = loader->load_chunks(2 * MiB);
                            if (maybe_samples.is_error()) {
                                FAIL(ByteString::formatted("{}", maybe_samples.error()));
                                return;
                            }
                            maybe_samples.value().remove_all_matching([](auto& chunk) { return chunk.is_empty(); });
                            if (maybe_samples.value().is_empty())
                                return;
                        }
                    },
                    false)));
            }
            return IterationDecision::Continue;
        });
    }
};
// Hack taken from TEST_CASE; the above constructor will run as part of global initialization before the tests are actually executed
static struct DiscoverFLACTestsHack hack;
