if(NOT DEFINED SCB_EXECUTABLE OR NOT DEFINED SCB_EXPECT_REGEX)
    message(FATAL_ERROR "SCB_EXECUTABLE and SCB_EXPECT_REGEX are required")
endif()

if(NOT DEFINED SCB_WORKING_DIRECTORY)
    set(SCB_WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")
endif()

set(SCB_ARGS)
foreach(index RANGE 1 8)
    if(DEFINED SCB_ARG${index})
        list(APPEND SCB_ARGS "${SCB_ARG${index}}")
    endif()
endforeach()

execute_process(
    COMMAND "${SCB_EXECUTABLE}" ${SCB_ARGS}
    WORKING_DIRECTORY "${SCB_WORKING_DIRECTORY}"
    RESULT_VARIABLE scb_result
    OUTPUT_VARIABLE scb_stdout
    ERROR_VARIABLE scb_stderr
)

set(scb_output "${scb_stdout}${scb_stderr}")

if(scb_result EQUAL 0)
    message(FATAL_ERROR "command unexpectedly succeeded\n${scb_output}")
endif()

if(NOT scb_output MATCHES "${SCB_EXPECT_REGEX}")
    message(FATAL_ERROR "expected regex not found\n${scb_output}")
endif()

message("${scb_output}")
