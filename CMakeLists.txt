cmake_minimum_required(VERSION 3.24)
project(PDFTatsujin VERSION 0.1.0 LANGUAGES CXX)
include(cmake/DetectMsvcIncludes.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(TATSU_BUILD_APP "Build the M1 application (requires Tesseract)" ON)
option(TATSU_ENABLE_SELFTEST "Include the M1 acceptance test entry point" ON)

# Keep upstream pristine. Its generated config.h lives in this binary directory.
set(PDF4QT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PDF4QT_INSTALL_PREPARE_WIX_INSTALLER OFF CACHE BOOL "" FORCE)
set(PDF4QT_INSTALL_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(PDF4QT_INSTALL_QT_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(PDF4QT_INSTALL_TO_USR OFF CACHE BOOL "" FORCE)
include_directories("${CMAKE_BINARY_DIR}/vendor/PDF4QT")
add_subdirectory(vendor/PDF4QT EXCLUDE_FROM_ALL)

if(NOT TATSU_BUILD_APP)
    return()
endif()

find_package(Qt6 6.9 REQUIRED COMPONENTS Widgets PrintSupport)
find_package(Tesseract CONFIG REQUIRED)

# Document state and PDF operations have no dependency on the application window.
add_library(tatsu_document STATIC
    src/document.cpp
    src/pdf_io.cpp
    src/pdf_font.cpp
    src/pdf_render.cpp
)
target_include_directories(tatsu_document PUBLIC src vendor/PDF4QT/Pdf4QtLibCore/sources)
target_link_libraries(tatsu_document PUBLIC Pdf4QtLibCore Qt6::Gui Qt6::PrintSupport)

add_library(tatsu_ocr STATIC src/ocr.cpp)
target_link_libraries(tatsu_ocr PUBLIC tatsu_document PRIVATE Tesseract::libtesseract)

add_library(tatsu_ui STATIC src/canvas.cpp src/window.cpp src/page_control.cpp src/search_session.cpp src/search_panel.cpp src/selection_text.cpp src/navigation.cpp src/bookmarks_panel.cpp src/page_previews.cpp)
target_link_libraries(tatsu_ui PUBLIC tatsu_document Pdf4QtLibWidgets Qt6::Widgets)

add_executable(PDFTatsujin WIN32 src/main.cpp)
target_link_libraries(PDFTatsujin PRIVATE tatsu_ui tatsu_ocr)
set_target_properties(PDFTatsujin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(TATSU_ENABLE_SELFTEST)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    target_sources(PDFTatsujin PRIVATE tests/selftest.cpp tests/viewer_tests.cpp tests/search_tests.cpp tests/selection_tests.cpp tests/pan_tests.cpp tests/navigation_tests.cpp tests/reading_tests.cpp)
    target_include_directories(PDFTatsujin PRIVATE tests)
    target_compile_definitions(PDFTatsujin PRIVATE TATSU_ENABLE_SELFTEST)
    target_link_libraries(PDFTatsujin PRIVATE Qt6::Test)
endif()

foreach(target tatsu_document tatsu_ocr tatsu_ui PDFTatsujin)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /bigobj)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endforeach()
