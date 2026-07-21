if(NOT DEFINED CHECKER OR NOT DEFINED CONFIG OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "CHECKER, CONFIG, and EXPECTED are required")
endif()

set(command "${CHECKER}")
if(DEFINED LIBRARY)
    list(APPEND command "${LIBRARY}")
endif()
list(APPEND command "${CONFIG}")

execute_process(
    COMMAND ${command}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL EXPECTED)
    message(FATAL_ERROR
        "expected exit ${EXPECTED}, got ${result}\nstdout:\n${output}\nstderr:\n${error}")
endif()
