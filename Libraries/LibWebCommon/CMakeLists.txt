set(SOURCES
    Clipboard/SystemClipboard.cpp
    ContentSecurityPolicy/Directives/SerializedDirective.cpp
    ContentSecurityPolicy/SerializedPolicy.cpp
    CSS/PreferredColorScheme.cpp
    CSS/StyleSheetIdentifier.cpp
    CSS/SystemColor.cpp
    Fetch/Infrastructure/HTTP/Statuses.cpp
    FileAPI/BlobURLStore.cpp
    FileAPI/SerializedBlobURLEntry.cpp
    Gamepad/GamepadSharedState.cpp
    Gamepad/GamepadSnapshot.cpp
    Gamepad/GamepadStateBuffer.cpp
    HTML/AutoplayPolicy.cpp
    HTML/BroadcastChannelMessage.cpp
    HTML/BrowsingContext.cpp
    HTML/CrossProcessId.cpp
    HTML/EmbedderPolicy.cpp
    HTML/FileFilter.cpp
    HTML/HistoryOperation.cpp
    HTML/NavigationParamsDescriptor.cpp
    HTML/NavigationPopulationRequest.cpp
    HTML/NavigationSourceSnapshot.cpp
    HTML/PostedMessageDescriptor.cpp
    HTML/POSTResource.cpp
    HTML/PreparedNavigationDescriptor.cpp
    HTML/ReplicatedNavigableState.cpp
    HTML/SameDocumentNavigationEntry.cpp
    HTML/SandboxingFlagSet.cpp
    HTML/Scripting/EnvironmentId.cpp
    HTML/Scripting/ScriptRegistryTypes.cpp
    HTML/Scripting/SerializedEnvironmentSettingsObject.cpp
    HTML/SelectedFile.cpp
    HTML/SelectItem.cpp
    HTML/SerializationRecords.cpp
    HTML/SerializedPolicyContainer.cpp
    HTML/SessionHistoryEntryDescriptor.cpp
    HTML/SessionHistoryEntryIdentity.cpp
    HTML/WebViewHints.cpp
    HTML/WorkerAgentTypes.cpp
    Infra/Strings.cpp
    Loader/DownloadFilename.cpp
    Loader/SiteCompatibility.cpp
    MimeSniff/MimeType.cpp
    Page/AsyncScrollNodeStableID.cpp
    Page/DragEvent.cpp
    Page/InputEvent.cpp
    Page/MediaContextMenu.cpp
    PixelUnits.cpp
    ReferrerPolicy/ReferrerPolicy.cpp
    StorageAPI/StorageEndpoint.cpp
    StorageAPI/StorageKey.cpp
    WebDriver/Capabilities.cpp
    WebDriver/Client.cpp
    WebDriver/Error.cpp
    WebDriver/Proxy.cpp
    WebDriver/Response.cpp
    WebDriver/TimeoutsConfiguration.cpp
    WebDriver/UserPrompt.cpp
    WebView/Attribute.cpp
    WebView/BrowsingBehavior.cpp
    WebView/ConsoleOutput.cpp
    WebView/Debugger.cpp
    WebView/DictionaryLookup.cpp
    WebView/DOMNodeProperties.cpp
    WebView/Geolocation.cpp
    WebView/Mutation.cpp
    WebView/ProcessHandle.cpp
    WebView/SiteIsolation.cpp
    WebView/Utilities.cpp
)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/CSS")
invoke_py_header_generator(
    "PseudoElement.h"
    "generate_libweb_css_pseudo_element.py"
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/CSS/PseudoElements.json"
    "CSS/PseudoElement.h"
    arguments --enum-only -j "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/CSS/PseudoElements.json"
    dependencies "${LADYBIRD_SOURCE_DIR}/Meta/Utils/utils.py"
)

invoke_py_generator(
    "PropertyList.cpp"
    "generate_libweb_css_property_id.py"
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/CSS/Properties.json"
    "CSS/PropertyList.h"
    "CSS/PropertyList.cpp"
    arguments --property-list
              -j "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/CSS/Properties.json"
              -e "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/CSS/Enums.json"
              -g "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/CSS/LogicalPropertyGroups.json"
    dependencies "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/CSS/Enums.json"
                 "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb/CSS/LogicalPropertyGroups.json"
                 "${LADYBIRD_SOURCE_DIR}/Meta/Utils/utils.py"
)

# NB: The protocols between the UI process and the processes it hosts web content in only use LibWebCommon types, so
#     both sides generate their endpoints from here.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/Services/WebContent)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/Services/WebDriver)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/Services/WebWorker)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebContentClient.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebContentClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebContentServer.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebContentServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebContentTestClient.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebContentTestClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebContentTestServer.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebContentTestServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebDriver/WebDriverBrowserClient.ipc ${CMAKE_BINARY_DIR}/Services/WebDriver/WebDriverBrowserClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebDriver/WebDriverBrowserServer.ipc ${CMAKE_BINARY_DIR}/Services/WebDriver/WebDriverBrowserServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebUIClient.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebUIClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebContent/WebUIServer.ipc ${CMAKE_BINARY_DIR}/Services/WebContent/WebUIServerEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebWorker/WebWorkerClient.ipc ${CMAKE_BINARY_DIR}/Services/WebWorker/WebWorkerClientEndpoint.h)
compile_ipc(${LADYBIRD_SOURCE_DIR}/Services/WebWorker/WebWorkerServer.ipc ${CMAKE_BINARY_DIR}/Services/WebWorker/WebWorkerServerEndpoint.h)

set(GENERATED_SOURCES
    CSS/PropertyList.cpp
    CSS/PseudoElement.h
    ../../Services/WebContent/WebContentClientEndpoint.h
    ../../Services/WebContent/WebContentServerEndpoint.h
    ../../Services/WebContent/WebContentTestClientEndpoint.h
    ../../Services/WebContent/WebContentTestServerEndpoint.h
    ../../Services/WebDriver/WebDriverBrowserClientEndpoint.h
    ../../Services/WebDriver/WebDriverBrowserServerEndpoint.h
    ../../Services/WebWorker/WebWorkerClientEndpoint.h
    ../../Services/WebWorker/WebWorkerServerEndpoint.h
    ../../Services/WebContent/WebUIClientEndpoint.h
    ../../Services/WebContent/WebUIServerEndpoint.h
)

if (NOT APPLE AND NOT CMAKE_INSTALL_LIBEXECDIR STREQUAL "libexec")
    set_source_files_properties(WebView/Utilities.cpp PROPERTIES COMPILE_DEFINITIONS LADYBIRD_LIBEXECDIR="${CMAKE_INSTALL_LIBEXECDIR}")
endif()

ladybird_lib(LibWebCommon webcommon EXPLICIT_SYMBOL_EXPORT)
# NB: The enumerations of LibWeb's IDL modules are generated into LibWebCommon along with LibWeb's bindings.
add_dependencies(LibWebCommon generate_bindings)
target_link_libraries(LibWebCommon PRIVATE LibCore LibCrypto LibFileSystem LibGfx LibHTTP LibIPC LibURL)

if (ENABLE_INSTALL_HEADERS)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CSS/PropertyList.h" "${CMAKE_CURRENT_BINARY_DIR}/CSS/PseudoElement.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibWebCommon/CSS")

    foreach(header ${GENERATED_SOURCES})
        if (NOT header MATCHES "^\\.\\./\\.\\./Services/.*\\.h$")
            continue()
        endif()

        get_filename_component(subdirectory ${header} DIRECTORY)
        string(REGEX REPLACE "^\\.\\./\\.\\./" "" subdirectory "${subdirectory}")

        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${subdirectory}")
    endforeach()
endif()
