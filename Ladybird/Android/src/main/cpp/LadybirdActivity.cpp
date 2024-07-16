/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ALooperEventLoopImplementation.h"
#include "JNIHelpers.h"
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/OwnPtr.h>
#include <Ladybird/Utilities.h>
#include <LibArchive/TarStream.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Application.h>
#include <jni.h>

static ErrorOr<void> extract_tar_archive(String archive_file, ByteString output_directory);

JavaVM* global_vm;
static OwnPtr<WebView::Application> s_application;
static OwnPtr<Core::EventLoop> s_main_event_loop;
static jobject s_java_instance;
static jmethodID s_schedule_event_loop_method;

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_initNativeCode(JNIEnv* env, jobject thiz, jstring resource_dir, jstring tag_name, jobject timer_service)
{
    char const* raw_resource_dir = env->GetStringUTFChars(resource_dir, nullptr);
    s_serenity_resource_root = raw_resource_dir;
    env->ReleaseStringUTFChars(resource_dir, raw_resource_dir);

    char const* raw_tag_name = env->GetStringUTFChars(tag_name, nullptr);
    AK::set_log_tag_name(raw_tag_name);
    env->ReleaseStringUTFChars(tag_name, raw_tag_name);

    dbgln("Set resource dir to {}", s_serenity_resource_root);

    auto file_or_error = Core::System::open(MUST(String::formatted("{}/res/icons/48x48/app-browser.png", s_serenity_resource_root)), O_RDONLY);
    if (file_or_error.is_error()) {
        dbgln("No resource files, extracting assets...");
        MUST(extract_tar_archive(MUST(String::formatted("{}/ladybird-assets.tar", s_serenity_resource_root)), s_serenity_resource_root));
    } else {
        dbgln("Found app-browser.png, not re-extracting assets.");
        dbgln("Hopefully no developer changed the asset files and expected them to be re-extracted!");
    }

    env->GetJavaVM(&global_vm);
    VERIFY(global_vm);

    s_java_instance = env->NewGlobalRef(thiz);
    jclass clazz = env->GetObjectClass(s_java_instance);
    VERIFY(clazz);
    s_schedule_event_loop_method = env->GetMethodID(clazz, "scheduleEventLoop", "()V");
    VERIFY(s_schedule_event_loop_method);
    env->DeleteLocalRef(clazz);

    jobject timer_service_ref = env->NewGlobalRef(timer_service);

    auto* event_loop_manager = new Ladybird::ALooperEventLoopManager(timer_service_ref);
    event_loop_manager->on_did_post_event = [] {
        Ladybird::JavaEnvironment env(global_vm);
        env.get()->CallVoidMethod(s_java_instance, s_schedule_event_loop_method);
    };
    Core::EventLoopManager::install(*event_loop_manager);
    s_main_event_loop = make<Core::EventLoop>();

    // FIXME: We are not making use of this Application object to track our processes.
    // So, right now, the Application's ProcessManager is constantly empty.
    // (However, LibWebView depends on an Application object existing, so we do have to actually create one.)
    s_application = make<WebView::Application>(0, nullptr);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_execMainEventLoop(JNIEnv*, jobject /* thiz */)
{
    if (s_main_event_loop) {
        s_main_event_loop->pump(Core::EventLoop::WaitMode::PollForEvents);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_disposeNativeCode(JNIEnv* env, jobject /* thiz */)
{
    s_main_event_loop = nullptr;
    s_schedule_event_loop_method = nullptr;
    s_application = nullptr;
    env->DeleteGlobalRef(s_java_instance);

    delete &Core::EventLoopManager::the();
}

ErrorOr<void> extract_tar_archive(String archive_file, ByteString output_directory)
{
    constexpr size_t buffer_size = 4096;

    auto file = TRY(Core::InputBufferedFile::create(TRY(Core::File::open(archive_file, Core::File::OpenMode::Read))));

    ByteString old_pwd = TRY(Core::System::getcwd());

    TRY(Core::System::chdir(output_directory));
    ScopeGuard go_back = [&old_pwd] { MUST(Core::System::chdir(old_pwd)); };

    auto tar_stream = TRY(Archive::TarInputStream::construct(move(file)));

    HashMap<ByteString, ByteString> global_overrides;
    HashMap<ByteString, ByteString> local_overrides;

    auto get_override = [&](StringView key) -> Optional<ByteString> {
        Optional<ByteString> maybe_local = local_overrides.get(key);

        if (maybe_local.has_value())
            return maybe_local;

        Optional<ByteString> maybe_global = global_overrides.get(key);

        if (maybe_global.has_value())
            return maybe_global;

        return {};
    };

    while (!tar_stream->finished()) {
        Archive::TarFileHeader const& header = tar_stream->header();

        // Handle meta-entries earlier to avoid consuming the file content stream.
        if (header.content_is_like_extended_header()) {
            switch (header.type_flag()) {
            case Archive::TarFileType::GlobalExtendedHeader: {
                TRY(tar_stream->for_each_extended_header([&](StringView key, StringView value) {
                    if (value.length() == 0)
                        global_overrides.remove(key);
                    else
                        global_overrides.set(key, value);
                }));
                break;
            }
            case Archive::TarFileType::ExtendedHeader: {
                TRY(tar_stream->for_each_extended_header([&](StringView key, StringView value) {
                    local_overrides.set(key, value);
                }));
                break;
            }
            default:
                warnln("Unknown extended header type '{}' of {}", (char)header.type_flag(), header.filename());
                VERIFY_NOT_REACHED();
            }

            TRY(tar_stream->advance());
            continue;
        }

        Archive::TarFileStream file_stream = tar_stream->file_contents();

        // Handle other header types that don't just have an effect on extraction.
        switch (header.type_flag()) {
        case Archive::TarFileType::LongName: {
            StringBuilder long_name;

            Array<u8, buffer_size> buffer;

            while (!file_stream.is_eof()) {
                auto slice = TRY(file_stream.read_some(buffer));
                long_name.append(reinterpret_cast<char*>(slice.data()), slice.size());
            }

            local_overrides.set("path", long_name.to_byte_string());
            TRY(tar_stream->advance());
            continue;
        }
        default:
            // None of the relevant headers, so continue as normal.
            break;
        }

        LexicalPath path = LexicalPath(header.filename());
        if (!header.prefix().is_empty())
            path = path.prepend(header.prefix());
        ByteString filename = get_override("path"sv).value_or(path.string());

        ByteString absolute_path = TRY(FileSystem::absolute_path(filename));
        auto parent_path = LexicalPath(absolute_path).parent();
        auto header_mode = TRY(header.mode());

        switch (header.type_flag()) {
        case Archive::TarFileType::NormalFile:
        case Archive::TarFileType::AlternateNormalFile: {
            MUST(Core::Directory::create(parent_path, Core::Directory::CreateDirectories::Yes));

            int fd = TRY(Core::System::open(absolute_path, O_CREAT | O_WRONLY, header_mode));

            Array<u8, buffer_size> buffer;
            while (!file_stream.is_eof()) {
                auto slice = TRY(file_stream.read_some(buffer));
                TRY(Core::System::write(fd, slice));
            }

            TRY(Core::System::close(fd));
            break;
        }
        case Archive::TarFileType::SymLink: {
            MUST(Core::Directory::create(parent_path, Core::Directory::CreateDirectories::Yes));

            TRY(Core::System::symlink(header.link_name(), absolute_path));
            break;
        }
        case Archive::TarFileType::Directory: {
            MUST(Core::Directory::create(parent_path, Core::Directory::CreateDirectories::Yes));

            auto result_or_error = Core::System::mkdir(absolute_path, header_mode);
            if (result_or_error.is_error() && result_or_error.error().code() != EEXIST)
                return result_or_error.release_error();
            break;
        }
        default:
            // FIXME: Implement other file types
            warnln("file type '{}' of {} is not yet supported", (char)header.type_flag(), header.filename());
            VERIFY_NOT_REACHED();
        }

        // Non-global headers should be cleared after every file.
        local_overrides.clear();

        TRY(tar_stream->advance());
    }
    return {};
}
