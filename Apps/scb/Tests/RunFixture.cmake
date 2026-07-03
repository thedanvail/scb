if(NOT DEFINED SCB_EXECUTABLE OR NOT DEFINED SCB_FIXTURE_SOURCE OR NOT DEFINED SCB_EXPECT_REGEX OR NOT DEFINED SCB_TEMP_DIRECTORY)
    message(FATAL_ERROR "SCB_EXECUTABLE, SCB_FIXTURE_SOURCE, SCB_EXPECT_REGEX, and SCB_TEMP_DIRECTORY are required")
endif()

file(REMOVE_RECURSE "${SCB_TEMP_DIRECTORY}")
file(MAKE_DIRECTORY "${SCB_TEMP_DIRECTORY}")
file(COPY "${SCB_FIXTURE_SOURCE}" DESTINATION "${SCB_TEMP_DIRECTORY}")
get_filename_component(scb_fixture_name "${SCB_FIXTURE_SOURCE}" NAME)
set(SCB_WORKING_DIRECTORY "${SCB_TEMP_DIRECTORY}/${scb_fixture_name}")

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
    message(FATAL_ERROR "command failed\n${scb_output}")
endif()

if(NOT scb_output MATCHES "${SCB_EXPECT_REGEX}")
    message(FATAL_ERROR "expected regex not found\n${scb_output}")
endif()

if(DEFINED SCB_EXPECT_ARTIFACT)
    if(NOT EXISTS "${SCB_WORKING_DIRECTORY}/${SCB_EXPECT_ARTIFACT}")
        message(FATAL_ERROR "expected artifact not found: ${SCB_EXPECT_ARTIFACT}\n${scb_output}")
    endif()
endif()

if(DEFINED SCB_EXPECT_NO_ARTIFACT)
    if(EXISTS "${SCB_WORKING_DIRECTORY}/${SCB_EXPECT_NO_ARTIFACT}")
        message(FATAL_ERROR "unexpected artifact exists: ${SCB_EXPECT_NO_ARTIFACT}\n${scb_output}")
    endif()
endif()

message("${scb_output}")
