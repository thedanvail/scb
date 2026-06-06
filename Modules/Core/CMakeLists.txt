file(GLOB_RECURSE core_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cpp"
)

add_library(scb_core ${core_sources})
add_library(scb::core ALIAS scb_core)

target_compile_features(scb_core PUBLIC cxx_std_20)

target_include_directories(scb_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Include"
)

if(SCB_BUILD_TESTS)
    file(GLOB_RECURSE core_test_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/*.cpp"
    )

    if(core_test_sources)
        add_executable(scb_core_tests ${core_test_sources})
        target_link_libraries(scb_core_tests PRIVATE scb::core Catch2::Catch2WithMain)

        include(Catch)
        catch_discover_tests(scb_core_tests WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()
