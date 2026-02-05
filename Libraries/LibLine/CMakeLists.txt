set(SOURCES
    Editor.cpp
    InternalFunctions.cpp
    KeyCallbackMachine.cpp
    SuggestionManager.cpp
    XtermSuggestionDisplay.cpp
)

ladybird_lib(LibLine line)
target_link_libraries(LibLine PRIVATE LibCore LibUnicode)
