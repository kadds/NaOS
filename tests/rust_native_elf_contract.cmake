find_program(NAOS_READELF readelf REQUIRED)

execute_process(COMMAND ${NAOS_READELF} -h ${IMAGE}
                OUTPUT_VARIABLE ELF_HEADER
                RESULT_VARIABLE ELF_HEADER_STATUS)
if(NOT ELF_HEADER_STATUS EQUAL 0 OR NOT ELF_HEADER MATCHES "Type:[ \\t]+EXEC")
    message(FATAL_ERROR "Rust native binary is not an ET_EXEC: ${IMAGE}")
endif()

execute_process(COMMAND ${NAOS_READELF} -l ${IMAGE}
                OUTPUT_VARIABLE ELF_PROGRAM_HEADERS
                RESULT_VARIABLE ELF_PROGRAM_HEADERS_STATUS)
if(NOT ELF_PROGRAM_HEADERS_STATUS EQUAL 0 OR ELF_PROGRAM_HEADERS MATCHES "INTERP")
    message(FATAL_ERROR "Rust native binary has an ELF interpreter")
endif()

execute_process(COMMAND ${NAOS_READELF} -d ${IMAGE}
                OUTPUT_VARIABLE ELF_DYNAMIC
                RESULT_VARIABLE ELF_DYNAMIC_STATUS)
if(NOT ELF_DYNAMIC_STATUS EQUAL 0 OR NOT ELF_DYNAMIC MATCHES "no dynamic section")
    message(FATAL_ERROR "Rust native binary has dynamic dependencies")
endif()

execute_process(COMMAND ${NAOS_READELF} -Ws ${IMAGE}
                OUTPUT_VARIABLE ELF_SYMBOLS
                RESULT_VARIABLE ELF_SYMBOLS_STATUS)
if(NOT ELF_SYMBOLS_STATUS EQUAL 0 OR ELF_SYMBOLS MATCHES " UND ")
    message(FATAL_ERROR "Rust native binary has undefined symbols")
endif()

file(READ ${MAP} LINK_MAP)
if(LINK_MAP MATCHES "mlibc|crt1|libstdc")
    message(FATAL_ERROR "Rust native link map contains a compatibility runtime")
endif()
