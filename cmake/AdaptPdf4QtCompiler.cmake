# Derive one translation unit from the pinned MIT source. Do not edit the
# submodule. Refuse a changed source rather than silently applying a loose patch.
option(TATSU_FIX_COMPILER_STARTUP "Check queued pages before the compiler waits" ON)
set(TATSU_COMPILER_TEST_DELAY_MS 0 CACHE STRING "Test-only worker startup delay")
if(NOT TATSU_COMPILER_TEST_DELAY_MS MATCHES "^[0-9]+$" OR TATSU_COMPILER_TEST_DELAY_MS GREATER 1000)
    message(FATAL_ERROR "Invalid compiler test delay")
endif()
if(NOT TATSU_FIX_COMPILER_STARTUP AND TATSU_COMPILER_TEST_DELAY_MS EQUAL 0)
    message(FATAL_ERROR "Disabling the queue fix is only allowed for an instrumented test")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vendor/PDF4QT/Pdf4QtLibWidgets/sources/pdfcompiler.cpp" compiler_source)
string(REPLACE "\r\n" "\n" compiler_source "${compiler_source}")
string(SHA256 compiler_hash "${compiler_source}")
if(NOT compiler_hash STREQUAL "a6a53b6b776c8802f60ad539fa03255a56d54612ec648194a727d6cea485b36b")
    message(FATAL_ERROR "Review the compiler adaptation for the new PDF4QT source")
endif()
string(REPLACE "#include <execution>" "#include <execution>\n#include <algorithm>" compiler_source "${compiler_source}")
if(TATSU_FIX_COMPILER_STARTUP)
    string(REPLACE "if (m_waitCondition->wait(locker.mutex(), QDeadlineTimer(QDeadlineTimer::Forever)))"
        "if (std::any_of(m_compiler->m_tasks.begin(), m_compiler->m_tasks.end(), [](const auto& task) { return !task.second.finished; }) || m_waitCondition->wait(locker.mutex(), QDeadlineTimer(QDeadlineTimer::Forever)))"
        compiler_source "${compiler_source}")
endif()
# Stop must synchronize with entering wait, or its wake can also be lost.
string(REPLACE "m_thread->requestInterruption();\n            m_waitCondition.wakeAll();"
    "{\n                QMutexLocker locker(&m_mutex);\n                m_thread->requestInterruption();\n                m_waitCondition.wakeAll();\n            }"
    compiler_source "${compiler_source}")
string(REPLACE "void PDFAsynchronousPageCompilerWorkerThread::run()\n{"
    "void PDFAsynchronousPageCompilerWorkerThread::run()\n{\n#if defined(TATSU_COMPILER_TEST_DELAY_MS)\n    QThread::msleep(TATSU_COMPILER_TEST_DELAY_MS);\n#endif"
    compiler_source "${compiler_source}")
set(adapted_compiler "${CMAKE_BINARY_DIR}/adapted/pdfcompiler.cpp")
if(EXISTS "${adapted_compiler}")
    file(READ "${adapted_compiler}" previous_compiler)
endif()
if(NOT compiler_source STREQUAL previous_compiler)
    file(WRITE "${adapted_compiler}" "${compiler_source}")
endif()
get_target_property(widget_sources Pdf4QtLibWidgets SOURCES)
list(FIND widget_sources "sources/pdfcompiler.cpp" original_compiler_index)
if(original_compiler_index LESS 0)
    message(FATAL_ERROR "The original compiler source is missing from the target")
endif()
list(REMOVE_ITEM widget_sources "sources/pdfcompiler.cpp")
set_property(TARGET Pdf4QtLibWidgets PROPERTY SOURCES "${widget_sources};${adapted_compiler}")
if(TATSU_COMPILER_TEST_DELAY_MS GREATER 0)
    set_source_files_properties("${adapted_compiler}" TARGET_DIRECTORY Pdf4QtLibWidgets
        PROPERTIES COMPILE_DEFINITIONS "TATSU_COMPILER_TEST_DELAY_MS=${TATSU_COMPILER_TEST_DELAY_MS}")
endif()
