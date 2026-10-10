cmake_minimum_required(VERSION 3.24)
project(PDFTatsujin VERSION 0.2.0 LANGUAGES CXX)
include(cmake/DetectMsvcIncludes.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(TATSU_BUILD_APP "Build the application (requires Tesseract)" ON)
option(TATSU_ENABLE_SELFTEST "Include the M1/M2 acceptance test entry point" ON)

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

target_sources(Pdf4QtLibCore PRIVATE src/pdf_font_metrics.cpp)

# The upstream compiler is private to this DLL. Keep our small adapter in the
# same target instead of patching upstream or exporting all of its internals.
target_sources(Pdf4QtLibWidgets PRIVATE src/pdf_viewer_adapter.cpp)
target_include_directories(Pdf4QtLibWidgets PRIVATE vendor/PDF4QT/Pdf4QtLibWidgets/sources)
include(cmake/AdaptPdf4QtCompiler.cmake)
include(cmake/AdaptPdf4QtManipulator.cmake)
include(cmake/AdaptPdf4QtColorSpace.cmake)
include(cmake/AdaptPdf4QtWriter.cmake)
include(cmake/AdaptPdf4QtSecurity.cmake)
include(cmake/AdaptPdf4QtVerticalFont.cmake)
target_sources(Pdf4QtLibCore PRIVATE src/pdf_security_adapter.cpp)

find_package(Qt6 6.9 REQUIRED COMPONENTS Widgets PrintSupport Xml)
find_package(Tesseract CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

# Document state and PDF operations have no dependency on the application window.
add_library(tatsu_document STATIC
    src/document.cpp
    src/ocr_jobs.cpp
    src/ocr_job.cpp
    src/private_temp.cpp
    src/save_candidate.cpp
    src/windows_path.cpp
    src/worker_channels.cpp
    src/image_overlay.cpp
    src/image_embedding.cpp
    src/existing_image_edit.cpp
    src/existing_form_image_edit.cpp
    src/content_invocation_edit.cpp
    src/form_content_invocations.cpp
    src/existing_text_edit.cpp
    src/body_text_font.cpp
    src/body_text_wrap.cpp
    src/image_pdf.cpp
    src/image_export.cpp
    src/signature_library.cpp
    src/page_operations.cpp
    src/page_crop.cpp
    src/page_decoration.cpp
    src/bookmark_edit.cpp
    src/link_edit.cpp
    src/pdf_optimization.cpp
    src/redaction_content.cpp
    src/redaction_document.cpp
    src/redaction_resource_audit.cpp
    src/redaction_copy.cpp
    src/certificate_verification.cpp
    src/certificate_crypto.cpp
    src/certificate_signing.cpp
    src/encrypted_pdf.cpp
    src/unprotected_pdf.cpp
    src/document_comparison.cpp
    src/batch_processing.cpp
    src/navigation.cpp
    src/pdf_appearance.cpp
    src/form_fields.cpp
    src/form_font.cpp
    src/form_design.cpp
    src/form_data.cpp
    src/annotation_operations.cpp
    src/pdf_io.cpp
    src/pdf_font.cpp
    src/text_font.cpp
    src/pdf_render.cpp
)
target_include_directories(tatsu_document PUBLIC src vendor/PDF4QT/Pdf4QtLibCore/sources)
target_link_libraries(tatsu_document PUBLIC Pdf4QtLibCore Qt6::Gui Qt6::PrintSupport)
target_link_libraries(tatsu_document PRIVATE advapi32 crypt32 ZLIB::ZLIB OpenSSL::Crypto)
target_sources(tatsu_document PRIVATE src/owned_process.cpp src/office_import.cpp)
target_sources(tatsu_document PRIVATE src/office_pdf_compat.cpp)
target_sources(tatsu_document PRIVATE src/table_extraction.cpp src/table_xlsx.cpp)
target_link_libraries(tatsu_document PRIVATE Qt6::CorePrivate Qt6::Xml)

add_library(tatsu_ocr STATIC src/ocr.cpp src/ocr_preprocess.cpp src/vertical_ocr_layer.cpp)
target_link_libraries(tatsu_ocr PUBLIC tatsu_document PRIVATE Tesseract::libtesseract)

add_library(tatsu_ui STATIC src/writing_panel.cpp src/text_font_picker.cpp src/window_writing.cpp src/canvas.cpp src/window.cpp src/page_control.cpp src/search_session.cpp src/search_panel.cpp src/selection_text.cpp src/bookmarks_panel.cpp src/page_previews.cpp src/preview_raster.cpp)
target_sources(tatsu_ui PRIVATE src/page_organizer.cpp src/window_pages.cpp)
target_sources(tatsu_ui PRIVATE src/page_crop_dialog.cpp)
target_sources(tatsu_ui PRIVATE src/page_decoration_dialog.cpp src/window_formatting.cpp)
target_sources(tatsu_ui PRIVATE src/bookmark_edit_dialog.cpp src/window_bookmarks.cpp)
target_sources(tatsu_ui PRIVATE src/link_edit_dialog.cpp src/page_region_preview.cpp src/window_links.cpp)
target_sources(tatsu_ui PRIVATE src/existing_image_dialog.cpp src/window_existing_images.cpp)
target_sources(tatsu_ui PRIVATE src/candidate_preview.cpp src/existing_text_dialog.cpp src/window_existing_text.cpp)
target_sources(tatsu_ui PRIVATE src/pdf_optimization_dialog.cpp src/window_optimization.cpp)
target_sources(tatsu_ui PRIVATE src/encryption_dialog.cpp src/window_encryption.cpp)
target_sources(tatsu_ui PRIVATE src/decryption_dialog.cpp)
target_sources(tatsu_ui PRIVATE src/certificate_dialog.cpp src/window_certificates.cpp)
target_sources(tatsu_ui PRIVATE src/certificate_signing_dialog.cpp)
target_sources(tatsu_ui PRIVATE src/redaction_dialog.cpp src/window_redaction.cpp)
target_sources(tatsu_ui PRIVATE src/comparison_dialog.cpp src/window_comparison.cpp)
target_sources(tatsu_ui PRIVATE src/batch_dialog.cpp src/window_batch.cpp)
target_sources(tatsu_ui PRIVATE src/image_pdf_dialog.cpp src/window_creation.cpp)
target_sources(tatsu_ui PRIVATE src/office_import_dialog.cpp)
target_sources(tatsu_ui PRIVATE src/table_grid_preview.cpp src/table_extraction_dialog.cpp)
target_sources(tatsu_ui PRIVATE src/image_export_dialog.cpp)
target_sources(tatsu_ui PRIVATE src/form_editor.cpp)
target_sources(tatsu_ui PRIVATE src/form_design_dialog.cpp src/window_form_design.cpp)
target_sources(tatsu_ui PRIVATE src/form_data_dialog.cpp src/window_form_data.cpp)
target_sources(tatsu_ui PRIVATE src/view_history.cpp)
target_sources(tatsu_ui PRIVATE src/ui_icons.cpp)
target_sources(tatsu_ui PRIVATE src/ui_widgets.cpp)
target_sources(tatsu_ui PRIVATE src/annotation_panel.cpp src/window_annotations.cpp)
target_link_libraries(tatsu_ui PUBLIC tatsu_document Pdf4QtLibWidgets Qt6::Widgets)

add_executable(PDFTatsujin WIN32 src/main.cpp)
target_link_libraries(PDFTatsujin PRIVATE tatsu_ui tatsu_ocr)
set_target_properties(PDFTatsujin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Optional measurement harness: use the real UI without modifying the product binary.
add_executable(PDFTatsujinRedactionProbe WIN32 EXCLUDE_FROM_ALL tests/redaction_probe.cpp tests/redaction_candidate_checks.cpp)
target_link_libraries(PDFTatsujinRedactionProbe PRIVATE tatsu_document)
set_target_properties(PDFTatsujinRedactionProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(MSVC)
    target_compile_options(PDFTatsujinRedactionProbe PRIVATE /utf-8 /bigobj)
    target_compile_definitions(PDFTatsujinRedactionProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
add_executable(PDFTatsujinFormFontProbe WIN32 EXCLUDE_FROM_ALL tests/form_font_probe.cpp)
target_link_libraries(PDFTatsujinFormFontProbe PRIVATE tatsu_document)
set_target_properties(PDFTatsujinFormFontProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(MSVC)
    target_compile_options(PDFTatsujinFormFontProbe PRIVATE /utf-8 /bigobj)
    target_compile_definitions(PDFTatsujinFormFontProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
add_executable(PDFTatsujinReadingBenchmark WIN32 EXCLUDE_FROM_ALL tests/reading_benchmark.cpp)
target_link_libraries(PDFTatsujinReadingBenchmark PRIVATE tatsu_ui tatsu_ocr)
set_target_properties(PDFTatsujinReadingBenchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
add_executable(PDFTatsujinInteractionBenchmark WIN32 EXCLUDE_FROM_ALL tests/interaction_benchmark.cpp)
target_link_libraries(PDFTatsujinInteractionBenchmark PRIVATE tatsu_ui tatsu_ocr)
set_target_properties(PDFTatsujinInteractionBenchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
add_executable(PDFTatsujinSoakTest WIN32 EXCLUDE_FROM_ALL tests/soak_test.cpp tests/memory_diagnostics.cpp)
target_link_libraries(PDFTatsujinSoakTest PRIVATE tatsu_ui)
set_target_properties(PDFTatsujinSoakTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(WIN32)
    add_executable(PDFTatsujinFilesystemProbe WIN32 EXCLUDE_FROM_ALL tests/filesystem_probe.cpp tests/disk_full_tests.cpp tests/save_interruption_worker.cpp)
    target_link_libraries(PDFTatsujinFilesystemProbe PRIVATE tatsu_document)
    target_compile_definitions(PDFTatsujinFilesystemProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(PDFTatsujinFilesystemProbe PRIVATE /utf-8 /bigobj)
    endif()
    set_target_properties(PDFTatsujinFilesystemProbe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_executable(PDFTatsujinNetworkProbe EXCLUDE_FROM_ALL tests/network_probe.cpp)
    target_link_libraries(PDFTatsujinNetworkProbe PRIVATE ws2_32 advapi32)
    target_compile_definitions(PDFTatsujinNetworkProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(PDFTatsujinNetworkProbe PROPERTIES
        AUTOMOC OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

if(TATSU_ENABLE_SELFTEST)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    target_sources(PDFTatsujin PRIVATE tests/writing_tests.cpp tests/selftest.cpp tests/viewer_tests.cpp tests/search_tests.cpp tests/selection_tests.cpp tests/pan_tests.cpp tests/navigation_tests.cpp tests/reading_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/page_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/form_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/annotation_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/image_decode_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/ocr_job_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/font_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/compact_viewer_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/save_candidate_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/layout_transition_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/reference_workflow_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/image_pdf_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/office_import_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/office_sheets_slides_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/table_extraction_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/vertical_ocr_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/image_export_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/page_geometry_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/page_crop_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/page_decoration_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/bookmark_edit_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/link_edit_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/pdf_optimization_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/encryption_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/form_data_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/unprotected_pdf_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/comparison_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/batch_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/form_design_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/certificate_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/certificate_signing_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/redaction_copy_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/redaction_scan_tests.cpp tests/redaction_ui_test_support.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_image_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_form_image_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_text_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_text_ui_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_text_font_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_text_multiline_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_text_line_count_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_form_text_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_text_wrap_tests.cpp)
    target_sources(PDFTatsujin PRIVATE tests/existing_image_ui_tests.cpp)
    target_include_directories(PDFTatsujin PRIVATE tests)
    target_compile_definitions(PDFTatsujin PRIVATE TATSU_ENABLE_SELFTEST)
    target_link_libraries(PDFTatsujin PRIVATE Qt6::Test OpenSSL::Crypto)
endif()

foreach(target tatsu_document tatsu_ocr tatsu_ui PDFTatsujin PDFTatsujinReadingBenchmark PDFTatsujinInteractionBenchmark PDFTatsujinSoakTest)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /bigobj)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endforeach()
