function (generate_css_implementation)
    set(LIBWEB_INPUT_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}")

    invoke_generator(
        "Enums.cpp"
        Lagom::GenerateCSSEnums
        "${LIBWEB_INPUT_FOLDER}/CSS/Enums.json"
        "CSS/Enums.h"
        "CSS/Enums.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Enums.json"
    )

    invoke_generator(
        "MathFunctions.cpp"
        Lagom::GenerateCSSMathFunctions
        "${LIBWEB_INPUT_FOLDER}/CSS/MathFunctions.json"
        "CSS/MathFunctions.h"
        "CSS/MathFunctions.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/MathFunctions.json"
    )

    invoke_generator(
        "MediaFeatureID.cpp"
        Lagom::GenerateCSSMediaFeatureID
        "${LIBWEB_INPUT_FOLDER}/CSS/MediaFeatures.json"
        "CSS/MediaFeatureID.h"
        "CSS/MediaFeatureID.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/MediaFeatures.json"
    )

    invoke_generator(
        "PropertyID.cpp"
        Lagom::GenerateCSSPropertyID
        "${LIBWEB_INPUT_FOLDER}/CSS/Properties.json"
        "CSS/PropertyID.h"
        "CSS/PropertyID.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Properties.json"
    )

    invoke_generator(
        "PseudoClass.cpp"
        Lagom::GenerateCSSPseudoClass
        "${LIBWEB_INPUT_FOLDER}/CSS/PseudoClasses.json"
        "CSS/PseudoClass.h"
        "CSS/PseudoClass.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/PseudoClasses.json"
    )

    invoke_generator(
        "PseudoElement.cpp"
        Lagom::GenerateCSSPseudoElement
        "${LIBWEB_INPUT_FOLDER}/CSS/PseudoElements.json"
        "CSS/PseudoElement.h"
        "CSS/PseudoElement.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/PseudoElements.json"
    )

    invoke_generator(
        "TransformFunctions.cpp"
        Lagom::GenerateCSSTransformFunctions
        "${LIBWEB_INPUT_FOLDER}/CSS/TransformFunctions.json"
        "CSS/TransformFunctions.h"
        "CSS/TransformFunctions.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/TransformFunctions.json"
    )

    invoke_generator(
        "Keyword.cpp"
        Lagom::GenerateCSSKeyword
        "${LIBWEB_INPUT_FOLDER}/CSS/Keywords.json"
        "CSS/Keyword.h"
        "CSS/Keyword.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/CSS/Keywords.json"
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
       "CSS/GeneratedCSSStyleProperties.h"
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
    )
    list(APPEND LIBWEB_ALL_GENERATED_IDL ${CSS_GENERATED_IDL})
    set(LIBWEB_ALL_GENERATED_IDL ${LIBWEB_ALL_GENERATED_IDL} PARENT_SCOPE)
endfunction()

function (generate_html_implementation)
    set(LIBWEB_INPUT_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}")

    invoke_generator(
        "NamedCharacterReferences.cpp"
        Lagom::GenerateNamedCharacterReferences
        "${LIBWEB_INPUT_FOLDER}/HTML/Parser/Entities.json"
        "HTML/Parser/NamedCharacterReferences.h"
        "HTML/Parser/NamedCharacterReferences.cpp"
        arguments -j "${LIBWEB_INPUT_FOLDER}/HTML/Parser/Entities.json"
    )

    set(HTML_GENERATED_HEADERS
       "HTML/Parser/NamedCharacterReferences.h"
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
    function(libweb_js_bindings class)
        cmake_parse_arguments(PARSE_ARGV 1 LIBWEB_BINDINGS "NAMESPACE;ITERABLE;GLOBAL" "" "")
        get_filename_component(basename "${class}" NAME)

        if (LIBWEB_BINDINGS_NAMESPACE)
            set(BINDINGS_SOURCES
                "Bindings/${basename}Namespace.h"
                "Bindings/${basename}Namespace.cpp"
            )
        else()
            set(BINDINGS_SOURCES
                "Bindings/${basename}Constructor.h"
                "Bindings/${basename}Constructor.cpp"
                "Bindings/${basename}Prototype.h"
                "Bindings/${basename}Prototype.cpp"
            )
        endif()

        if(LIBWEB_BINDINGS_ITERABLE)
            list(APPEND BINDINGS_SOURCES
                "Bindings/${basename}IteratorPrototype.h"
                "Bindings/${basename}IteratorPrototype.cpp"
            )
        endif()

        if(LIBWEB_BINDINGS_GLOBAL)
            list(APPEND BINDINGS_SOURCES
                "Bindings/${basename}GlobalMixin.h"
                "Bindings/${basename}GlobalMixin.cpp"
            )
        endif()

        list(TRANSFORM BINDINGS_SOURCES PREPEND "${CMAKE_CURRENT_BINARY_DIR}/")
        target_sources(${target} PRIVATE ${BINDINGS_SOURCES})

        get_property(include_paths DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)
        list(TRANSFORM include_paths PREPEND -i)

        # Ninja expects the target name in depfiles to be relative to CMAKE_BINARY_DIR, but ${bindings_src} is
        # relative to CMAKE_CURRENT_BINARY_DIR. CMake >= 3.20 can do the rewriting transparently (CMP0116).
        set(depfile_prefix_arg "")
        if(CMAKE_GENERATOR MATCHES "^Ninja" AND NOT POLICY CMP0116)
            file(RELATIVE_PATH depfile_target ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR})
            set(depfile_prefix_arg "--depfile-prefix ${depfile_target}" )
        endif()

        add_custom_command(
            OUTPUT ${BINDINGS_SOURCES}
            COMMAND "$<TARGET_FILE:Lagom::BindingsGenerator>" -o "Bindings" --depfile "Bindings/${basename}.d"
                    ${depfile_prefix_arg} "${LIBWEB_INPUT_FOLDER}/${class}.idl" "${LIBWEB_INPUT_FOLDER}" "${CMAKE_CURRENT_BINARY_DIR}"
            VERBATIM
            COMMENT "Generating Bindings for ${class}"
            DEPENDS Lagom::BindingsGenerator
            MAIN_DEPENDENCY ${class}.idl
            DEPFILE ${CMAKE_CURRENT_BINARY_DIR}/Bindings/${basename}.d
        )

        add_custom_target(generate_${basename} DEPENDS ${BINDINGS_SOURCES})
        add_dependencies(all_generated generate_${basename})
        add_dependencies(${target} generate_${basename})
        add_dependencies(generate_${basename} ${generated_idl_targets})

        set(BINDINGS_HEADERS ${BINDINGS_SOURCES})
        list(FILTER BINDINGS_HEADERS INCLUDE REGEX "\.h$")

        if (ENABLE_INSTALL_HEADERS)
            install(FILES ${BINDINGS_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibWeb/Bindings")
        endif()

        list(APPEND LIBWEB_ALL_GENERATED_HEADERS ${BINDINGS_HEADERS})
        set(LIBWEB_ALL_GENERATED_HEADERS ${LIBWEB_ALL_GENERATED_HEADERS} PARENT_SCOPE)

        list(APPEND LIBWEB_ALL_IDL_FILES "${LIBWEB_INPUT_FOLDER}/${class}.idl")
        set(LIBWEB_ALL_IDL_FILES ${LIBWEB_ALL_IDL_FILES} PARENT_SCOPE)
    endfunction()

    function(generate_exposed_interface_files)
        set(exposed_interface_sources
            IntrinsicDefinitions.cpp
            DedicatedWorkerExposedInterfaces.cpp DedicatedWorkerExposedInterfaces.h
            SharedWorkerExposedInterfaces.cpp SharedWorkerExposedInterfaces.h
            ShadowRealmExposedInterfaces.cpp ShadowRealmExposedInterfaces.h
            WindowExposedInterfaces.cpp WindowExposedInterfaces.h)
        list(TRANSFORM exposed_interface_sources PREPEND "Bindings/")
        set(LIBWEB_ALL_IDL_FILES_ARGUMENT ${LIBWEB_ALL_IDL_FILES})
        if (WIN32)
            list(JOIN LIBWEB_ALL_IDL_FILES "\n" idl_file_list)
            file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/all_idl_files.txt" CONTENT "${idl_file_list}" NEWLINE_STYLE UNIX)
            set(LIBWEB_ALL_IDL_FILES "${CMAKE_CURRENT_BINARY_DIR}/all_idl_files.txt")
            set(LIBWEB_ALL_IDL_FILES_ARGUMENT "@${LIBWEB_ALL_IDL_FILES}")
        endif()
        add_custom_command(
            OUTPUT  ${exposed_interface_sources}
            COMMAND "${CMAKE_COMMAND}" -E make_directory "tmp"
            COMMAND $<TARGET_FILE:Lagom::GenerateWindowOrWorkerInterfaces> -o "${CMAKE_CURRENT_BINARY_DIR}/tmp" -b "${LIBWEB_INPUT_FOLDER}" -b "${CMAKE_CURRENT_BINARY_DIR}" ${LIBWEB_ALL_IDL_FILES_ARGUMENT}
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/IntrinsicDefinitions.cpp "Bindings/IntrinsicDefinitions.cpp"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/DedicatedWorkerExposedInterfaces.h "Bindings/DedicatedWorkerExposedInterfaces.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/DedicatedWorkerExposedInterfaces.cpp "Bindings/DedicatedWorkerExposedInterfaces.cpp"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/SharedWorkerExposedInterfaces.h "Bindings/SharedWorkerExposedInterfaces.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/SharedWorkerExposedInterfaces.cpp "Bindings/SharedWorkerExposedInterfaces.cpp"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/ShadowRealmExposedInterfaces.h "Bindings/ShadowRealmExposedInterfaces.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/ShadowRealmExposedInterfaces.cpp "Bindings/ShadowRealmExposedInterfaces.cpp"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/WindowExposedInterfaces.h "Bindings/WindowExposedInterfaces.h"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different tmp/WindowExposedInterfaces.cpp "Bindings/WindowExposedInterfaces.cpp"
            COMMAND "${CMAKE_COMMAND}" -E remove_directory "${CMAKE_CURRENT_BINARY_DIR}/tmp"
            VERBATIM
            DEPENDS Lagom::GenerateWindowOrWorkerInterfaces ${LIBWEB_ALL_IDL_FILES}
        )
        target_sources(${target} PRIVATE ${exposed_interface_sources})
        add_custom_target(generate_exposed_interfaces DEPENDS ${exposed_interface_sources})
        add_dependencies(all_generated generate_exposed_interfaces)
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
    generate_exposed_interface_files()

    set(LIBWEB_ALL_GENERATED_HEADERS ${LIBWEB_ALL_GENERATED_HEADERS} PARENT_SCOPE)
endfunction()
