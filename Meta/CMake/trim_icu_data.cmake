# Copies the ICU data file INPUT to OUTPUT without the items that nothing in Ladybird uses: the character set converters,
# as text decoding goes through LibTextCodec, and the StringPrep profiles, as IDNA uses UTS #46 instead.
#
# ICUPKG is ICU's icupkg tool, and REMOVED_ITEMS_LIST a .txt file to write the list of removed items to.

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(COPY_FILE "${INPUT}" "${OUTPUT}")

execute_process(COMMAND "${ICUPKG}" -l "${OUTPUT}" OUTPUT_VARIABLE items COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\r" "" items "${items}")
string(REPLACE "\n" ";" items "${items}")
list(FILTER items INCLUDE REGEX "(\\.cnv|^cnvalias\\.icu|\\.spp)$")
if (NOT items)
    message(FATAL_ERROR "${INPUT} has none of the items to remove")
endif()
list(JOIN items "\n" removed_items)
file(WRITE "${REMOVED_ITEMS_LIST}" "${removed_items}\n")

execute_process(COMMAND "${ICUPKG}" -r "${REMOVED_ITEMS_LIST}" "${OUTPUT}" COMMAND_ERROR_IS_FATAL ANY)
