file(GLOB children RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*")

set(app_dirs)
foreach(child IN LISTS children)
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${child}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${child}/CMakeLists.txt")
        list(APPEND app_dirs "${child}")
    endif()
endforeach()

list(SORT app_dirs)

foreach(app_dir IN LISTS app_dirs)
    add_subdirectory("${app_dir}")
endforeach()
