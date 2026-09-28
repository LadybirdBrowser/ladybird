set(SOURCES
    Clipboard/SystemClipboard.cpp
    CSS/PreferredColorScheme.cpp
    CSS/SystemColor.cpp
    Fetch/Infrastructure/HTTP/Statuses.cpp
    FileAPI/SerializedBlobURLEntry.cpp
    HTML/CrossProcessId.cpp
    HTML/EmbedderPolicy.cpp
    HTML/FileFilter.cpp
    HTML/POSTResource.cpp
    HTML/SandboxingFlagSet.cpp
    HTML/SelectedFile.cpp
    Infra/Strings.cpp
    Loader/DownloadFilename.cpp
    Loader/SiteCompatibility.cpp
    Page/DragEvent.cpp
    ReferrerPolicy/ReferrerPolicy.cpp
    StorageAPI/StorageEndpoint.cpp
    WebDriver/Error.cpp
    WebDriver/Response.cpp
)

ladybird_lib(LibWebCommon webcommon EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibWebCommon PUBLIC LibCompositing)
target_link_libraries(LibWebCommon PRIVATE LibCore LibGfx LibIPC LibURL)
