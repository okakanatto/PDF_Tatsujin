# Optional dev43 boundary probe. Include only after the primary build is complete.
# The Core PUBLIC forced header is inherited through tatsu_document, as in the app.
if(NOT TARGET PDFTatsujinWordSpacingBoundaries)
    add_executable(PDFTatsujinWordSpacingBoundaries WIN32 EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_LIST_DIR}/../tests/dev43_boundary_runner.cpp)
    target_link_libraries(PDFTatsujinWordSpacingBoundaries PRIVATE tatsu_document)
    set_target_properties(PDFTatsujinWordSpacingBoundaries PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    target_compile_definitions(PDFTatsujinWordSpacingBoundaries PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(PDFTatsujinWordSpacingBoundaries PRIVATE /utf-8 /bigobj)
    endif()
endif()
