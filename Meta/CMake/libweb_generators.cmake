function (generate_css_implementation)
    set(LIBWEB_INPUT_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}")


    invoke_cpp_generator(
        "DescriptorID.cpp"
        Lagom::GenerateCSSDescriptors
        "${LIBWEB_INPUT_FOLDER}/CSS/Descriptors.json"
        "CSS/DescriptorID.h"
        "CSS/DescriptorID.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Descriptors.json"
    )

    invoke_cpp_generator(
        "Enums.cpp"
        Lagom::GenerateCSSEnums
        "${LIBWEB_INPUT_FOLDER}/CSS/Enums.json"
        "CSS/Enums.h"
        "CSS/Enums.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Enums.json"
    )

    invoke_cpp_generator(
        "EnvironmentVariable.cpp"
        Lagom::GenerateCSSEnvironmentVariable
        "${LIBWEB_INPUT_FOLDER}/CSS/EnvironmentVariables.json"
        "CSS/EnvironmentVariable.h"
        "CSS/EnvironmentVariable.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/EnvironmentVariables.json"
    )

    invoke_cpp_generator(
        "MathFunctions.cpp"
        Lagom::GenerateCSSMathFunctions
        "${LIBWEB_INPUT_FOLDER}/CSS/MathFunctions.json"
        "CSS/MathFunctions.h"
        "CSS/MathFunctions.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/MathFunctions.json"
    )

    invoke_cpp_generator(
        "MediaFeatureID.cpp"
        Lagom::GenerateCSSMediaFeatureID
        "${LIBWEB_INPUT_FOLDER}/CSS/MediaFeatures.json"
        "CSS/MediaFeatureID.h"
        "CSS/MediaFeatureID.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/MediaFeatures.json"
    )

    invoke_cpp_generator(
        "PropertyID.cpp"
        Lagom::GenerateCSSPropertyID
        "${LIBWEB_INPUT_FOLDER}/CSS/Properties.json"
        "CSS/PropertyID.h"
        "CSS/PropertyID.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Properties.json"
                  -e "${LIBWEB_INPUT_FOLDER}/CSS/Enums.json"
                  -g "${LIBWEB_INPUT_FOLDER}/CSS/LogicalPropertyGroups.json"
        dependencies "${LIBWEB_INPUT_FOLDER}/CSS/Enums.json" "${LIBWEB_INPUT_FOLDER}/CSS/LogicalPropertyGroups.json"
    )

    invoke_cpp_generator(
        "PseudoClass.cpp"
        Lagom::GenerateCSSPseudoClass
        "${LIBWEB_INPUT_FOLDER}/CSS/PseudoClasses.json"
        "CSS/PseudoClass.h"
        "CSS/PseudoClass.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/PseudoClasses.json"
    )

    invoke_cpp_generator(
        "PseudoElement.cpp"
        Lagom::GenerateCSSPseudoElement
        "${LIBWEB_INPUT_FOLDER}/CSS/PseudoElements.json"
        "CSS/PseudoElement.h"
        "CSS/PseudoElement.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/PseudoElements.json"
    )

    invoke_cpp_generator(
        "TransformFunctions.cpp"
        Lagom::GenerateCSSTransformFunctions
        "${LIBWEB_INPUT_FOLDER}/CSS/TransformFunctions.json"
        "CSS/TransformFunctions.h"
        "CSS/TransformFunctions.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/TransformFunctions.json"
    )

    invoke_cpp_generator(
        "Units.cpp"
        Lagom::GenerateCSSUnits
        "${LIBWEB_INPUT_FOLDER}/CSS/Units.json"
        "CSS/Units.h"
        "CSS/Units.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Units.json"
    )

    invoke_cpp_generator(
        "Keyword.cpp"
        Lagom::GenerateCSSKeyword
        "${LIBWEB_INPUT_FOLDER}/CSS/Keywords.json"
        "CSS/Keyword.h"
        "CSS/Keyword.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Keywords.json"
    )

    invoke_idl_generator(
        "GeneratedCSSNumericFactoryMethods.cpp"
        "GeneratedCSSNumericFactoryMethods.idl"
        Lagom::GenerateCSSNumericFactoryMethods
        "${LIBWEB_INPUT_FOLDER}/CSS/Units.json"
        "CSS/GeneratedCSSNumericFactoryMethods.h"
        "CSS/GeneratedCSSNumericFactoryMethods.cpp"
        "CSS/GeneratedCSSNumericFactoryMethods.idl"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Units.json"
    )

    invoke_idl_generator(
        "GeneratedCSSStyleProperties.cpp"
        "GeneratedCSSStyleProperties.idl"
        Lagom::GenerateCSSStyleProperties
        "${LIBWEB_INPUT_FOLDER}/CSS/Properties.json"
        "CSS/GeneratedCSSStyleProperties.h"
        "CSS/GeneratedCSSStyleProperties.cpp"
        "CSS/GeneratedCSSStyleProperties.idl"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Properties.json"
    )

    embed_as_string(
        "DefaultStyleSheetSource.cpp"
        "${LIBWEB_INPUT_FOLDER}/CSS/Default.css"
        "CSS/DefaultStyleSheetSource.cpp"
        "default_stylesheet_source"
        NAMESPACE "Web::CSS"
    )

    embed_as_string(
        "QuirksModeStyleSheetSource.cpp"
        "${LIBWEB_INPUT_FOLDER}/CSS/QuirksMode.css"
        "CSS/QuirksModeStyleSheetSource.cpp"
        "quirks_mode_stylesheet_source"
        NAMESPACE "Web::CSS"
    )

    embed_as_string(
        "MathMLStyleSheetSource.cpp"
        "${LIBWEB_INPUT_FOLDER}/MathML/Default.css"
        "MathML/MathMLStyleSheetSource.cpp"
        "mathml_stylesheet_source"
        NAMESPACE "Web::CSS"
    )

    embed_as_string(
        "SVGStyleSheetSource.cpp"
        "${LIBWEB_INPUT_FOLDER}/SVG/Default.css"
        "SVG/SVGStyleSheetSource.cpp"
        "svg_stylesheet_source"
        NAMESPACE "Web::CSS"
    )

    set(CSS_GENERATED_HEADERS
       "CSS/Enums.h"
       "CSS/EnvironmentVariable.h"
       "CSS/GeneratedCSSStyleProperties.h"
       "CSS/GeneratedCSSNumericFactoryMethods.h"
       "CSS/Keyword.h"
       "CSS/MathFunctions.h"
       "CSS/MediaFeatureID.h"
       "CSS/PropertyID.h"
       "CSS/PseudoClass.h"
       "CSS/PseudoElement.h"
       "CSS/TransformFunctions.h"
    )
    list(TRANSFORM CSS_GENERATED_HEADERS PREPEND "${CMAKE_CURRENT_BINARY_DIR}/")
    if (ENABLE_INSTALL_HEADERS)
        install(FILES ${CSS_GENERATED_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibWeb/CSS")
    endif()
    list(APPEND LIBWEB_ALL_GENERATED_HEADERS ${CSS_GENERATED_HEADERS})
    set(LIBWEB_ALL_GENERATED_HEADERS ${LIBWEB_ALL_GENERATED_HEADERS} PARENT_SCOPE)

    set(CSS_GENERATED_IDL
        "GeneratedCSSStyleProperties.idl"
        "GeneratedCSSNumericFactoryMethods.idl"
    )
    list(APPEND LIBWEB_ALL_GENERATED_IDL ${CSS_GENERATED_IDL})
    set(LIBWEB_ALL_GENERATED_IDL ${LIBWEB_ALL_GENERATED_IDL} PARENT_SCOPE)
endfunction()

function (generate_html_implementation)
    set(LIBWEB_INPUT_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}")

    invoke_cpp_generator(
        "NamedCharacterReferences.cpp"
        Lagom::GenerateNamedCharacterReferences
        "${LIBWEB_INPUT_FOLDER}/HTML/Parser/Entities.json"
        "HTML/Parser/NamedCharacterReferences.h"
        "HTML/Parser/NamedCharacterReferences.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/HTML/Parser/Entities.json"
    )

    invoke_py_generator(
        "MediaControlsDOM.cpp"
        "generate_dom_tree.py"
        "${LIBWEB_INPUT_FOLDER}/HTML/MediaControls.html"
        "HTML/MediaControlsDOM.h"
        "HTML/MediaControlsDOM.cpp"
        arguments -i "${LIBWEB_INPUT_FOLDER}/HTML/MediaControls.html"
                  -s MediaControlsDOM
                  -n "Web::HTML"
                  --html-tags "${LIBWEB_INPUT_FOLDER}/HTML/TagNames.h"
                  --html-attributes "${LIBWEB_INPUT_FOLDER}/HTML/AttributeNames.h"
                  --svg-tags "${LIBWEB_INPUT_FOLDER}/SVG/TagNames.h"
                  --svg-attributes "${LIBWEB_INPUT_FOLDER}/SVG/AttributeNames.h"
        dependencies "${LIBWEB_INPUT_FOLDER}/HTML/TagNames.h"
                     "${LIBWEB_INPUT_FOLDER}/HTML/AttributeNames.h"
                     "${LIBWEB_INPUT_FOLDER}/SVG/TagNames.h"
                     "${LIBWEB_INPUT_FOLDER}/SVG/AttributeNames.h"
                     "${LIBWEB_INPUT_FOLDER}/HTML/MediaControls.css"
    )

    set(HTML_GENERATED_HEADERS
       "HTML/Parser/NamedCharacterReferences.h"
       "HTML/MediaControlsDOM.h"
    )
    list(TRANSFORM HTML_GENERATED_HEADERS PREPEND "${CMAKE_CURRENT_BINARY_DIR}/")
    if (ENABLE_INSTALL_HEADERS)
        install(FILES ${HTML_GENERATED_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibWeb/HTML")
    endif()
    list(APPEND LIBWEB_ALL_GENERATED_HEADERS ${HTML_GENERATED_HEADERS})
    set(LIBWEB_ALL_GENERATED_HEADERS ${LIBWEB_ALL_GENERATED_HEADERS} PARENT_SCOPE)
endfunction()

function (generate_js_bindings target)
    set(LIBWEB_INPUT_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}")
    set(generated_idl_targets ${LIBWEB_ALL_GENERATED_IDL})
    list(TRANSFORM generated_idl_targets PREPEND "generate_")
    set(LIBWEB_ALL_BINDINGS_SOURCES)
    set(LIBWEB_ALL_IDL_FILES)
    set(LIBWEB_ALL_PARSED_IDL_FILES)
    function(libweb_js_bindings class)
        get_filename_component(basename "${class}" NAME)

        set(BINDINGS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/Bindings/${basename}.h")
        set(BINDINGS_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/Bindings/${basename}.cpp")
        set(BINDINGS_SOURCES ${BINDINGS_HEADER} ${BINDINGS_SOURCE})
        target_sources(${target} PRIVATE ${BINDINGS_SOURCES})

        if (ENABLE_INSTALL_HEADERS)
            install(FILES ${BINDINGS_HEADER} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibWeb/Bindings")
        endif()

        list(APPEND LIBWEB_ALL_GENERATED_HEADERS ${BINDINGS_HEADER})
        set(LIBWEB_ALL_GENERATED_HEADERS ${LIBWEB_ALL_GENERATED_HEADERS} PARENT_SCOPE)

        list(APPEND LIBWEB_ALL_BINDINGS_SOURCES ${BINDINGS_SOURCES})
        set(LIBWEB_ALL_BINDINGS_SOURCES ${LIBWEB_ALL_BINDINGS_SOURCES} PARENT_SCOPE)

        list(APPEND LIBWEB_ALL_IDL_FILES "${LIBWEB_INPUT_FOLDER}/${class}.idl")
        set(LIBWEB_ALL_IDL_FILES ${LIBWEB_ALL_IDL_FILES} PARENT_SCOPE)

        list(APPEND LIBWEB_ALL_PARSED_IDL_FILES "${LIBWEB_INPUT_FOLDER}/${class}.idl")
        set(LIBWEB_ALL_PARSED_IDL_FILES ${LIBWEB_ALL_PARSED_IDL_FILES} PARENT_SCOPE)
    endfunction()

    function(libweb_support_idl class)
        list(APPEND LIBWEB_ALL_PARSED_IDL_FILES "${LIBWEB_INPUT_FOLDER}/${class}.idl")
        set(LIBWEB_ALL_PARSED_IDL_FILES ${LIBWEB_ALL_PARSED_IDL_FILES} PARENT_SCOPE)
    endfunction()

    function(libweb_generated_support_idl class)
        list(APPEND LIBWEB_ALL_PARSED_IDL_FILES "${CMAKE_CURRENT_BINARY_DIR}/${class}.idl")
        set(LIBWEB_ALL_PARSED_IDL_FILES ${LIBWEB_ALL_PARSED_IDL_FILES} PARENT_SCOPE)
    endfunction()

    function(generate_exposed_interface_files)
        set(exposed_interface_sources
            IntrinsicDefinitions.cpp IntrinsicDefinitions.h
            DedicatedWorkerExposedInterfaces.cpp DedicatedWorkerExposedInterfaces.h
            SharedWorkerExposedInterfaces.cpp SharedWorkerExposedInterfaces.h
            WindowExposedInterfaces.cpp WindowExposedInterfaces.h)
        list(TRANSFORM exposed_interface_sources PREPEND "Bindings/")
        add_custom_command(
            OUTPUT  ${exposed_interface_sources}
            COMMAND "${CMAKE_COMMAND}" -E make_directory "tmp"
            COMMAND $<TARGET_FILE:Lagom::GenerateWindowOrWorkerInterfaces> -o "${CMAKE_CURRENT_BINARY_DIR}/tmp" ${LIBWEB_ALL_IDL_FILES_ARGUMENT}
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/IntrinsicDefinitions.h "Bindings/IntrinsicDefinitions.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/IntrinsicDefinitions.cpp "Bindings/IntrinsicDefinitions.cpp"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/DedicatedWorkerExposedInterfaces.h "Bindings/DedicatedWorkerExposedInterfaces.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/DedicatedWorkerExposedInterfaces.cpp "Bindings/DedicatedWorkerExposedInterfaces.cpp"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/SharedWorkerExposedInterfaces.h "Bindings/SharedWorkerExposedInterfaces.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/SharedWorkerExposedInterfaces.cpp "Bindings/SharedWorkerExposedInterfaces.cpp"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/WindowExposedInterfaces.h "Bindings/WindowExposedInterfaces.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/WindowExposedInterfaces.cpp "Bindings/WindowExposedInterfaces.cpp"
            COMMAND "${CMAKE_COMMAND}" -E remove_directory "${CMAKE_CURRENT_BINARY_DIR}/tmp"
            VERBATIM
            DEPENDS Lagom::GenerateWindowOrWorkerInterfaces ${LIBWEB_ALL_IDL_FILES}
        )
        target_sources(${target} PRIVATE ${exposed_interface_sources})
        add_custom_target(generate_exposed_interfaces DEPENDS ${exposed_interface_sources})
        add_dependencies(ladybird_codegen_accumulator generate_exposed_interfaces)
        add_dependencies(${target} generate_exposed_interfaces)
        add_dependencies(generate_exposed_interfaces ${generated_idl_targets})

        list(TRANSFORM exposed_interface_sources PREPEND "${CMAKE_CURRENT_BINARY_DIR}/")
        set(exposed_interface_headers ${exposed_interface_sources})
        list(FILTER exposed_interface_headers INCLUDE REGEX "\.h$")

        if (ENABLE_INSTALL_HEADERS)
            install(FILES ${exposed_interface_headers} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibWeb/Bindings")
        endif()

        list(APPEND LIBWEB_ALL_GENERATED_HEADERS ${exposed_interface_headers})
        set(LIBWEB_ALL_GENERATED_HEADERS ${LIBWEB_ALL_GENERATED_HEADERS} PARENT_SCOPE)
    endfunction()

    include("idl_files.cmake")
    list(REMOVE_DUPLICATES LIBWEB_ALL_PARSED_IDL_FILES)

    set(LIBWEB_ALL_IDL_FILES_ARGUMENT ${LIBWEB_ALL_IDL_FILES})
    set(LIBWEB_ALL_PARSED_IDL_FILES_ARGUMENT ${LIBWEB_ALL_PARSED_IDL_FILES})
    if (WIN32)
        list(JOIN LIBWEB_ALL_IDL_FILES "\n" idl_file_list)
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/all_idl_files.txt" CONTENT "${idl_file_list}" NEWLINE_STYLE UNIX)
        set(LIBWEB_ALL_IDL_FILES_ARGUMENT "@${CMAKE_CURRENT_BINARY_DIR}/all_idl_files.txt")

        list(JOIN LIBWEB_ALL_PARSED_IDL_FILES "\n" parsed_idl_file_list)
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/all_parsed_idl_files.txt" CONTENT "${parsed_idl_file_list}" NEWLINE_STYLE UNIX)
        set(LIBWEB_ALL_PARSED_IDL_FILES_ARGUMENT "@${CMAKE_CURRENT_BINARY_DIR}/all_parsed_idl_files.txt")
    endif()

    add_custom_command(
        OUTPUT ${LIBWEB_ALL_BINDINGS_SOURCES}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "Bindings"
        COMMAND "$<TARGET_FILE:Lagom::BindingsGenerator>" -o "Bindings" --depfile "Bindings/all_bindings.d"
                ${LIBWEB_ALL_PARSED_IDL_FILES_ARGUMENT}
        VERBATIM
        COMMENT "Generating LibWeb bindings"
        DEPENDS Lagom::BindingsGenerator ${LIBWEB_ALL_IDL_FILES} ${LIBWEB_ALL_PARSED_IDL_FILES}
        DEPFILE ${CMAKE_CURRENT_BINARY_DIR}/Bindings/all_bindings.d
    )

    add_custom_target(generate_bindings DEPENDS ${LIBWEB_ALL_BINDINGS_SOURCES})
    add_dependencies(ladybird_codegen_accumulator generate_bindings)
    add_dependencies(${target} generate_bindings)
    add_dependencies(generate_bindings ${generated_idl_targets})

    generate_exposed_interface_files()

    set(LIBWEB_ALL_GENERATED_HEADERS ${LIBWEB_ALL_GENERATED_HEADERS} PARENT_SCOPE)
endfunction()
