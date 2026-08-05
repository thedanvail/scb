file(GLOB_RECURSE scb_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cpp"
)

add_executable(scb ${scb_sources})
target_compile_features(scb PRIVATE cxx_std_20)
target_link_libraries(scb PRIVATE scb::core)

if(SCB_BUILD_TESTS)
    add_test(
        NAME scb_cli_zero_config
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/zero_config_project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/zero_config
            -DSCB_EXPECT_REGEX=Finished.debug:.*executed
            -DSCB_EXPECT_ARTIFACT=target/debug/exe/zero_config_project/zero_config_project
            -DSCB_ARG1=build
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunFixture.cmake"
    )

    add_test(
        NAME scb_cli_manifest_build
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/manifest_project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/manifest_build
            -DSCB_EXPECT_REGEX=Project:.fixture
            -DSCB_EXPECT_ARTIFACT=target/debug/exe/fixture/fixture
            -DSCB_ARG1=build
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunFixture.cmake"
    )

    add_test(
        NAME scb_cli_manifest_release
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/manifest_project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/release
            -DSCB_EXPECT_REGEX=Profile:.release
            -DSCB_EXPECT_ARTIFACT=target/release/exe/fixture/fixture
            -DSCB_ARG1=build
            -DSCB_ARG2=--release
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunFixture.cmake"
    )

    add_test(
        NAME scb_cli_manifest_path
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/nested/project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/nested
            -DSCB_EXPECT_REGEX=Project:.nested
            -DSCB_EXPECT_ARTIFACT=target/debug/exe/nested/nested
            -DSCB_ARG1=build
            -DSCB_ARG2=--manifest-path
            -DSCB_ARG3=scb.toml
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunFixture.cmake"
    )

    add_test(
        NAME scb_cli_dry_run
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/zero_config_project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/dry-run
            -DSCB_EXPECT_REGEX=Would.execute
            -DSCB_EXPECT_NO_ARTIFACT=target/debug/exe/zero_config_project/zero_config_project
            -DSCB_ARG1=build
            -DSCB_ARG2=--dry-run
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunFixture.cmake"
    )

    add_test(
        NAME scb_cli_verbose
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/zero_config_project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/verbose
            -DSCB_EXPECT_REGEX=cmd:
            -DSCB_EXPECT_ARTIFACT=target/debug/exe/zero_config_project/zero_config_project
            -DSCB_ARG1=build
            -DSCB_ARG2=--verbose
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunFixture.cmake"
    )

    add_test(
        NAME scb_cli_plan
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/manifest_project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/plan
            -DSCB_EXPECT_REGEX=\"actions\"
            -DSCB_EXPECT_NO_ARTIFACT=target/debug/exe/fixture/fixture
            -DSCB_ARG1=build
            -DSCB_ARG2=--plan
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunFixture.cmake"
    )

    add_test(
        NAME scb_cli_invalid_compiler
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/invalid_compiler
            -DSCB_EXPECT_REGEX=configured.compiler.does.not.exist
            -DSCB_ARG1=build
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExpectFailure.cmake"
    )

    add_test(
        NAME scb_cli_missing_manifest
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
            -DSCB_EXPECT_REGEX=does.*exist
            -DSCB_ARG1=build
            -DSCB_ARG2=--manifest-path
            -DSCB_ARG3=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/does_not_exist.toml
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExpectFailure.cmake"
    )
    set_tests_properties(scb_cli_missing_manifest PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_test(
        NAME scb_cli_invalid_usage
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
            -DSCB_EXPECT_REGEX=usage:
            -DSCB_ARG1=build
            -DSCB_ARG2=--unknown
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExpectFailure.cmake"
    )
    set_tests_properties(scb_cli_invalid_usage PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_test(
        NAME scb_cli_clean_missing_manifest_path
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
            -DSCB_EXPECT_REGEX=manifest.file.does.not.exist
            -DSCB_ARG1=clean
            -DSCB_ARG2=--manifest-path
            -DSCB_ARG3=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/does_not_exist.toml
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExpectFailure.cmake"
    )
    set_tests_properties(scb_cli_clean_missing_manifest_path PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_test(
        NAME scb_cli_clean_invalid_manifest
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/invalid_manifest
            -DSCB_EXPECT_REGEX=project.name.must.be.a.string
            -DSCB_ARG1=clean
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExpectFailure.cmake"
    )

    add_test(
        NAME scb_cli_clean_missing_manifest
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/invalid_manifest
            -DSCB_EXPECT_REGEX=manifest.file.does.not.exist
            -DSCB_ARG1=clean
            -DSCB_ARG2=--manifest-path
            -DSCB_ARG3=missing.toml
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExpectFailure.cmake"
    )

    add_test(
        NAME scb_cli_clean_default_profile
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/zero_config_project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/clean_default
            -DSCB_EXPECT_REGEX=Cleaned.target/debug
            -DSCB_EXPECT_REMOVED=target/debug
            -DSCB_ARG1=clean
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunCleanFixture.cmake"
    )

    add_test(
        NAME scb_cli_clean_all_profiles
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/zero_config_project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/clean_all
            -DSCB_BUILD_PROFILE=--release
            -DSCB_EXPECT_REGEX=Cleaned.target/
            -DSCB_EXPECT_REMOVED=target
            -DSCB_ARG1=clean
            -DSCB_ARG2=--all
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunCleanFixture.cmake"
    )

    add_test(
        NAME scb_cli_clean_specified_profile
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures/zero_config_project
            -DSCB_TEMP_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures/clean_profile
            -DSCB_BUILD_PROFILE=--release
            -DSCB_EXPECT_REGEX=Cleaned.target/release
            -DSCB_EXPECT_REMOVED=target/release
            -DSCB_EXPECT_PRESERVED=target/debug
            -DSCB_ARG1=clean
            -DSCB_ARG2=--profile
            -DSCB_ARG3=release
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RunCleanFixture.cmake"
    )

    add_test(
        NAME scb_cli_version
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
            -DSCB_EXPECT_REGEX=scb.*[0-9]+\\.[0-9]+\\.[0-9]+
            -DSCB_ARG1=--version
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExpectFailure.cmake"
    )
    set_tests_properties(scb_cli_version PROPERTIES
        WILL_FAIL TRUE
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_test(
        NAME scb_cli_build_help
        COMMAND "${CMAKE_COMMAND}"
            -DSCB_EXECUTABLE=$<TARGET_FILE:scb>
            -DSCB_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
            -DSCB_EXPECT_REGEX=usage:.*scb build
            -DSCB_ARG1=build
            -DSCB_ARG2=--help
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExpectFailure.cmake"
    )
    set_tests_properties(scb_cli_build_help PROPERTIES
        WILL_FAIL TRUE
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
