if(NOT DEFINED SCB_EXECUTABLE OR NOT DEFINED SCB_FIXTURE_SOURCE OR NOT DEFINED SCB_TEMP_DIRECTORY OR NOT DEFINED SCB_EXPECT_REGEX)
    message(FATAL_ERROR "SCB_EXECUTABLE, SCB_FIXTURE_SOURCE, SCB_TEMP_DIRECTORY, and SCB_EXPECT_REGEX are required")
endif()

file(REMOVE_RECURSE "${SCB_TEMP_DIRECTORY}")
file(MAKE_DIRECTORY "${SCB_TEMP_DIRECTORY}")
file(COPY "${SCB_FIXTURE_SOURCE}" DESTINATION "${SCB_TEMP_DIRECTORY}")
get_filename_component(scb_fixture_name "${SCB_FIXTURE_SOURCE}" NAME)
set(SCB_WORKING_DIRECTORY "${SCB_TEMP_DIRECTORY}/${scb_fixture_name}")

set(SCB_BUILD_ARGS build)
if(DEFINED SCB_BUILD_PROFILE)
    list(APPEND SCB_BUILD_ARGS "${SCB_BUILD_PROFILE}")
endif()
list(APPEND SCB_BUILD_ARGS --verbose)

execute_process(
    COMMAND "${SCB_EXECUTABLE}" ${SCB_BUILD_ARGS}
    WORKING_DIRECTORY "${SCB_WORKING_DIRECTORY}"
    RESULT_VARIABLE scb_result
    OUTPUT_VARIABLE scb_stdout
    ERROR_VARIABLE scb_stderr
)

if(NOT scb_result EQUAL 0)
    message(FATAL_ERROR "build command failed\n${scb_stdout}${scb_stderr}")
endif()

set(SCB_DEFAULT_BUILD_ARGS build)
execute_process(
    COMMAND "${SCB_EXECUTABLE}" ${SCB_DEFAULT_BUILD_ARGS}
    WORKING_DIRECTORY "${SCB_WORKING_DIRECTORY}"
    RESULT_VARIABLE scb_result
    OUTPUT_VARIABLE scb_stdout_default
    ERROR_VARIABLE scb_stderr_default
)

if(NOT scb_result EQUAL 0)
    message(FATAL_ERROR "default build command failed\n${scb_stdout_default}${scb_stderr_default}")
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

if(NOT scb_result EQUAL 0)
    message(FATAL_ERROR "clean command failed\n${scb_output}")
endif()

if(NOT scb_output MATCHES "${SCB_EXPECT_REGEX}")
    message(FATAL_ERROR "expected regex not found\n${scb_output}")
endif()

if(DEFINED SCB_EXPECT_REMOVED)
    if(EXISTS "${SCB_WORKING_DIRECTORY}/${SCB_EXPECT_REMOVED}")
        message(FATAL_ERROR "expected artifact to be removed: ${SCB_EXPECT_REMOVED}\n${scb_output}")
    endif()
endif()

if(DEFINED SCB_EXPECT_PRESERVED)
    if(NOT EXISTS "${SCB_WORKING_DIRECTORY}/${SCB_EXPECT_PRESERVED}")
        message(FATAL_ERROR "expected artifact to be preserved: ${SCB_EXPECT_PRESERVED}\n${scb_output}")
    endif()
endif()

message("${scb_output}")
