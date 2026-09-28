set(SOURCES
    Clipboard/SystemClipboard.cpp
    ContentSecurityPolicy/Directives/SerializedDirective.cpp
    ContentSecurityPolicy/SerializedPolicy.cpp
    CSS/PreferredColorScheme.cpp
    CSS/StyleSheetIdentifier.cpp
    CSS/SystemColor.cpp
    Fetch/Infrastructure/HTTP/Statuses.cpp
    FileAPI/SerializedBlobURLEntry.cpp
    HTML/BroadcastChannelMessage.cpp
    HTML/CrossProcessId.cpp
    HTML/EmbedderPolicy.cpp
    HTML/FileFilter.cpp
    HTML/PostedMessageDescriptor.cpp
    HTML/POSTResource.cpp
    HTML/SandboxingFlagSet.cpp
    HTML/Scripting/SerializedEnvironmentSettingsObject.cpp
    HTML/SelectedFile.cpp
    HTML/SelectItem.cpp
    HTML/SerializationRecords.cpp
    HTML/SerializedPolicyContainer.cpp
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
    WebDriver/Error.cpp
    WebDriver/Response.cpp
)

ladybird_lib(LibWebCommon webcommon EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibWebCommon PUBLIC LibCompositing)
# NB: The enumerations of LibWeb's IDL modules are generated into LibWebCommon along with LibWeb's bindings.
add_dependencies(LibWebCommon generate_bindings)
target_link_libraries(LibWebCommon PRIVATE LibCore LibCrypto LibGfx LibHTTP LibIPC LibURL)
