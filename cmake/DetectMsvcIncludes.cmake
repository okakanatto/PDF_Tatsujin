# Ninja matches /showIncludes output byte for byte. On Windows without an
# English compiler language pack, console decoding can corrupt CMake's cached
# prefix and silently drop header dependencies. Probe without decoding it.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    set(probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/TatsuShowIncludes")
    file(MAKE_DIRECTORY "${probe_dir}")
    file(WRITE "${probe_dir}/tatsu-includes.h" "\n")
    file(WRITE "${probe_dir}/probe.cpp" "#include \"tatsu-includes.h\"\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c probe.cpp
        WORKING_DIRECTORY "${probe_dir}"
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_error
        RESULT_VARIABLE probe_result
        ENCODING NONE
    )
    if(NOT probe_result EQUAL 0 OR
       NOT probe_output MATCHES "(^|\n)([^\r\n]+: +)[A-Za-z]:[^\r\n]*tatsu-includes.h")
        message(FATAL_ERROR "Cannot detect MSVC header dependency prefix: ${probe_error}")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    unset(probe_dir)
    unset(probe_output)
    unset(probe_error)
    unset(probe_result)
endif()
