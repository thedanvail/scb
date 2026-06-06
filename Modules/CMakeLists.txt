file(GLOB children RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*")

set(module_dirs)
foreach(child IN LISTS children)
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${child}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${child}/CMakeLists.txt")
        list(APPEND module_dirs "${child}")
    endif()
endforeach()

list(SORT module_dirs)

foreach(module_dir IN LISTS module_dirs)
    add_subdirectory("${module_dir}")
endforeach()
