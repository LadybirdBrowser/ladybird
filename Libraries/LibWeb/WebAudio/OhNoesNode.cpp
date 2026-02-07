/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/OhNoesNode.h>

#include <AK/LexicalPath.h>
#include <AK/Utf16String.h>
#include <LibCore/System.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Web::WebAudio {

// Debug-only helper node created via Internals.createOhNoesNode().
// Not exposed to normal JavaScript.

static WebIDL::ExceptionOr<void> validate_base_path(JS::Realm& realm, String const& base_path)
{
    if (base_path.is_empty())
        return {};

    AK::LexicalPath lexical_path { base_path.to_byte_string() };
    StringView parent_directory = lexical_path.dirname();

    auto stat_or_error = Core::System::stat(parent_directory);
    if (stat_or_error.is_error()) {
        if (stat_or_error.error().is_errno() && stat_or_error.error().code() == ENOENT) {
            String message = MUST(String::formatted("OhNoesNode: parent directory does not exist: {}", parent_directory));
            return WebIDL::NotFoundError::create(realm, Utf16String::from_utf8(message.bytes_as_string_view()));
        }
        String message = MUST(String::formatted("OhNoesNode: could not stat parent directory {}", parent_directory));
        return WebIDL::OperationError::create(realm, Utf16String::from_utf8(message.bytes_as_string_view()));
    }

    struct stat const st = stat_or_error.release_value();
    if (!S_ISDIR(st.st_mode)) {
        String message = MUST(String::formatted("OhNoesNode: parent directory is not a directory: {}", parent_directory));
        return WebIDL::NotAllowedError::create(realm, Utf16String::from_utf8(message.bytes_as_string_view()));
    }

    // Creating files in a directory requires both write and execute permissions.
    auto access_or_error = Core::System::access(parent_directory, W_OK | X_OK);
    if (access_or_error.is_error()) {
        if (access_or_error.error().is_errno() && access_or_error.error().code() == EACCES) {
            String message = MUST(String::formatted("OhNoesNode: parent directory is not writable: {}", parent_directory));
            return WebIDL::NotAllowedError::create(realm, Utf16String::from_utf8(message.bytes_as_string_view()));
        }
        String message = MUST(String::formatted("OhNoesNode: could not access parent directory {}", parent_directory));
        return WebIDL::OperationError::create(realm, Utf16String::from_utf8(message.bytes_as_string_view()));
    }

    return {};
}

GC_DEFINE_ALLOCATOR(OhNoesNode);

OhNoesNode::OhNoesNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, String path)
    : AudioNode(realm, context)
    , m_base_path(move(path))
{
}

OhNoesNode::~OhNoesNode() = default;

WebIDL::ExceptionOr<GC::Ref<OhNoesNode>> OhNoesNode::create_for_internals(JS::Realm& realm, GC::Ref<BaseAudioContext> context, String path)
{
    // Fail early with a clear JS exception if the output directory is invalid.
    TRY(validate_base_path(realm, path));
    return realm.create<OhNoesNode>(realm, context, move(path));
}

WebIDL::ExceptionOr<void> OhNoesNode::start()
{
    if (m_emit_enabled)
        return {};

    TRY(validate_base_path(realm(), m_base_path));

    m_emit_enabled = true;
    context()->notify_audio_graph_changed();
    return {};
}

WebIDL::ExceptionOr<void> OhNoesNode::stop()
{
    if (!m_emit_enabled)
        return {};
    m_emit_enabled = false;
    context()->notify_audio_graph_changed();
    return {};
}

WebIDL::ExceptionOr<void> OhNoesNode::set_path(String path)
{
    // Avoid changing output destination while actively emitting.
    if (m_emit_enabled)
        return WebIDL::InvalidStateError::create(realm(), "Cannot change path while emitting; call stop() first."_utf16);

    TRY(validate_base_path(realm(), path));

    if (m_base_path == path)
        return {};

    m_base_path = move(path);
    context()->notify_audio_graph_changed();
    return {};
}

WebIDL::ExceptionOr<void> OhNoesNode::set_strip_zero_buffers(bool enabled)
{
    if (m_strip_zero_buffers == enabled)
        return {};

    m_strip_zero_buffers = enabled;
    context()->notify_audio_graph_changed();
    return {};
}

void OhNoesNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OhNoesNode);
    Base::initialize(realm);
}

void OhNoesNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
