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
    Page/DragEvent.cpp
    Page/MediaContextMenu.cpp
    ReferrerPolicy/ReferrerPolicy.cpp
    StorageAPI/StorageEndpoint.cpp
    StorageAPI/StorageKey.cpp
    WebDriver/Capabilities.cpp
    WebDriver/Error.cpp
    WebDriver/Proxy.cpp
    WebDriver/Response.cpp
    WebDriver/TimeoutsConfiguration.cpp
    WebDriver/UserPrompt.cpp
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

set(GENERATED_SOURCES
    CSS/PropertyList.cpp
    CSS/PseudoElement.h
)

ladybird_lib(LibWebCommon webcommon EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibWebCommon PUBLIC LibCompositing)
# NB: The enumerations of LibWeb's IDL modules are generated into LibWebCommon along with LibWeb's bindings.
add_dependencies(LibWebCommon generate_bindings)
target_link_libraries(LibWebCommon PRIVATE LibCore LibCrypto LibGfx LibHTTP LibIPC LibURL)

if (ENABLE_INSTALL_HEADERS)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CSS/PropertyList.h" "${CMAKE_CURRENT_BINARY_DIR}/CSS/PseudoElement.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibWebCommon/CSS")
endif()
