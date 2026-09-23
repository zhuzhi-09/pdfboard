cmake_minimum_required(VERSION 3.21)
project(PDFBoard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Pdf Network)
qt_standard_project_setup()

qt_add_executable(pdfboard
    assets/app.rc
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/DocumentTabs.h
    src/DocumentTabs.cpp
    src/ZoomBar.h
    src/ZoomBar.cpp
    src/PdfCanvas.h
    src/PdfCanvas.cpp
    src/AnnotationBundle.h
    src/AnnotationBundle.cpp
    src/PdfExport.h
    src/PdfExport.cpp
    src/ImageImport.h
    src/ImageImport.cpp
    src/InkToolbar.h
    src/InkToolbar.cpp
    src/PageGrid.h
    src/PageGrid.cpp
    src/AppSettings.h
    src/AppSettings.cpp
  src/AppLog.h
  src/AppLog.cpp
  src/CrashLog.h
  src/CrashLog.cpp
    src/SettingsPage.h
    src/SettingsPage.cpp
    src/HomePage.h
    src/HomePage.cpp
    src/WordConvert.h
    src/WordConvert.cpp
    src/UpdateChecker.h
    src/UpdateChecker.cpp
    src/IconPainter.h
    src/IconPainter.cpp
    src/Theme.h
  src/InputProbe.h
  src/InkDirty.h
  src/PaperBase.h
)

target_link_libraries(pdfboard PRIVATE
    Qt6::Widgets
    Qt6::Pdf
    Qt6::Network
psapi
dwmapi
version
dbghelp
)

# GUI subsystem: double-clicking the installed app must not flash a console.
# The --bench / --selftest-* modes re-attach to the launching terminal instead.
set_target_properties(pdfboard PROPERTIES WIN32_EXECUTABLE TRUE)

if (MSVC)
    # Chinese UI strings in sources need /utf-8
    target_compile_options(pdfboard PRIVATE /utf-8 /W4)
    # PDFium's rasteriser recurses deeply while rendering a 4K page at 250 % DPI, and a
    # stack overflow (0xC00000FD) is what a classroom VM was crashing with when the
    # zoom was dragged. The default 1 MB main-thread stack can be exhausted inside one
    # such render; 8 MB is only reserved address space, committed on demand.
    target_link_options(pdfboard PRIVATE "/STACK:8388608")
endif()

