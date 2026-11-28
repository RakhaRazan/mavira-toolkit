cmake_minimum_required(VERSION 3.25)

project(MAVIRA
    VERSION 1.1.0
    DESCRIPTION "MAVIRA - Native Reading Experience"
    LANGUAGES CXX
)

# ------------------------------------------------------------------------------
# Global C++20 Standard Enforcement
# ------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep Qt meta-object tooling disabled globally so pure C++20 layers remain Qt-free.
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

# ------------------------------------------------------------------------------
# Shared Compiler & Linker Options
# ------------------------------------------------------------------------------
add_library(mavira_build_options INTERFACE)
target_compile_features(mavira_build_options INTERFACE cxx_std_20)

target_compile_definitions(mavira_build_options INTERFACE
    UNICODE
    _UNICODE
    $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
    $<$<BOOL:${WIN32}>:NOMINMAX>
)

target_compile_options(mavira_build_options INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/O2 /Ob2 /Oi /Ot /GL /DNDEBUG /Gw /Gy>
)

target_link_options(mavira_build_options INTERFACE
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/LTCG /OPT:REF /OPT:ICF>
)

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
find_package(spdlog CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
find_package(pdfium CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

# ------------------------------------------------------------------------------
# Layer 4: Data & Win32 OS Layer (src/data/) - Pure C++20, Zero Qt Dependencies
# ------------------------------------------------------------------------------
add_library(mavira_data STATIC
    src/data/fs/FileManager.hpp
    src/data/fs/FileManager.cpp
    src/data/config/SettingsManager.hpp
    src/data/config/SettingsManager.cpp
    src/data/db/DatabaseManager.hpp
    src/data/db/DatabaseManager.cpp
)
target_include_directories(mavira_data PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(mavira_data PUBLIC
    mavira_build_options
    unofficial::sqlite3::sqlite3
)

# ------------------------------------------------------------------------------
# Layer 3: Core Engine (src/core/) - Pure C++20, Zero Qt Dependencies
# ------------------------------------------------------------------------------
add_library(mavira_core STATIC
    src/core/archive/IArchiveReader.hpp
    src/core/archive/ArchiveEngine.hpp
    src/core/archive/ArchiveEngine.cpp
    src/core/archive/ComicMerger.hpp
    src/core/archive/ComicMerger.cpp
    src/core/archive/ZipArchiveReader.hpp
    src/core/archive/ZipArchiveReader.cpp
    src/core/archive/FolderArchiveReader.hpp
    src/core/archive/FolderArchiveReader.cpp
    src/core/archive/LibArchiveReader.hpp
    src/core/archive/LibArchiveReader.cpp
    src/core/archive/PdfDocumentReader.hpp
    src/core/archive/PdfDocumentReader.cpp
    src/core/metadata/MetadataManager.hpp
    src/core/metadata/MetadataManager.cpp
    src/core/threading/WorkerPool.hpp
    src/core/threading/WorkerPool.cpp
    src/core/imaging/ImageDecoder.hpp
    src/core/imaging/ImageDecoder.cpp
    src/core/imaging/NativeOcrEngine.hpp
    src/core/imaging/NativeOcrEngine.cpp
    src/core/cache/ICoverStorage.hpp
    src/core/cache/CacheManager.hpp
    src/core/cache/CacheManager.cpp
    src/core/rendering/RenderingEngine.hpp
    src/core/rendering/RenderingEngine.cpp
)
target_include_directories(mavira_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(mavira_core PUBLIC
    mavira_build_options
    mavira_data
)
target_link_libraries(mavira_core PRIVATE
    libzip::zip
    LibArchive::LibArchive
    pdfium::pdfium
    libjpeg-turbo::turbojpeg
    PNG::PNG
    ZLIB::ZLIB
    WebP::webp
    $<$<BOOL:${WIN32}>:windowsapp>
    $<$<BOOL:${WIN32}>:shell32>
)

# ------------------------------------------------------------------------------
# Layer 2: Application Layer (src/app/) - Qt6::Core Coordination & State
# ------------------------------------------------------------------------------
add_library(mavira_app STATIC
    src/app/ApplicationCore.hpp
    src/app/ApplicationCore.cpp
    src/app/LibraryManager.hpp
    src/app/LibraryManager.cpp
    src/app/ReaderController.hpp
    src/app/ReaderController.cpp
)
set_target_properties(mavira_app PROPERTIES
    AUTOMOC ON
)
target_include_directories(mavira_app PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(mavira_app PUBLIC
    mavira_build_options
    mavira_core
    mavira_data
    Qt6::Core
    spdlog::spdlog
    $<$<BOOL:${WIN32}>:shell32>
    $<$<BOOL:${WIN32}>:ole32>
    $<$<BOOL:${WIN32}>:propsys>
)

# ------------------------------------------------------------------------------
# Layer 1: UI Layer (src/ui/) - Qt 6 Widgets High-Contrast Desktop Shell
# ------------------------------------------------------------------------------
add_library(mavira_ui STATIC
    src/ui/ComicItemDelegate.hpp
    src/ui/ComicItemDelegate.cpp
    src/ui/ExplorerSidebar.hpp
    src/ui/ExplorerSidebar.cpp
        src/ui/LibraryView.hpp
    src/ui/LibraryView.cpp
    src/ui/LocalizationManager.hpp
    src/ui/LocalizationManager.cpp
    src/ui/MainWindow.hpp
    src/ui/MainWindow.cpp
    src/ui/MergeComicsDialog.hpp
    src/ui/MergeComicsDialog.cpp
    src/ui/PreferencesWorkspace.hpp
    src/ui/PreferencesWorkspace.cpp
    src/ui/ReaderViewport.hpp
    src/ui/ReaderViewport.cpp
    src/ui/SectionCardWidget.hpp
    src/ui/SectionCardWidget.cpp
    src/ui/SettingsDialog.hpp
    src/ui/SettingsDialog.cpp
    src/ui/ToolWorkspaceWidget.hpp
    src/ui/ToolWorkspaceWidget.cpp
    src/ui/ToolsDialogs.hpp
    src/ui/ToolsDialogs.cpp
)
set_target_properties(mavira_ui PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
)
target_include_directories(mavira_ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(mavira_ui PUBLIC
    mavira_build_options
    mavira_app
    mavira_core
    mavira_data
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    spdlog::spdlog
)

# ------------------------------------------------------------------------------
# Application Executables: MAVIRA_Core.exe + MAVIRA.exe (WIN32 GUI Subsystem)
# ------------------------------------------------------------------------------
add_executable(MAVIRA WIN32
    src/main.cpp
    assets/app.rc
)

set_target_properties(MAVIRA PROPERTIES
    OUTPUT_NAME "MAVIRA_Core"
    WIN32_EXECUTABLE TRUE
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
)

add_executable(MAVIRA_Launcher WIN32
    src/launcher.cpp
    assets/app.rc
)

set_target_properties(MAVIRA_Launcher PROPERTIES
    OUTPUT_NAME "MAVIRA"
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(MAVIRA_Launcher PRIVATE
    mavira_build_options
)

target_include_directories(MAVIRA PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(MAVIRA PRIVATE
    mavira_build_options
    mavira_ui
    mavira_app
    mavira_core
    mavira_data
    spdlog::spdlog
    Qt6::Widgets
)

# Deploy the Qt Windows platform plugin and GLSL shaders alongside MAVIRA.exe for portable launch.
if(WIN32 AND TARGET Qt6::QWindowsIntegrationPlugin)
    add_custom_command(TARGET MAVIRA POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MAVIRA>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
            "$<TARGET_FILE_DIR:MAVIRA>/platforms/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MAVIRA>/assets/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders"
            "$<TARGET_FILE_DIR:MAVIRA>/assets/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.svg"
            "$<TARGET_FILE_DIR:MAVIRA>/assets/logo.svg"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico"
            "$<TARGET_FILE_DIR:MAVIRA>/assets/app.ico"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:MAVIRA>/LICENSE"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
            "$<TARGET_FILE_DIR:MAVIRA>/THIRD_PARTY_NOTICES.txt"
        COMMENT "Deploying Qt Windows platform plugin, GLSL shaders, branding assets, and license notices"
    )
    if(TARGET Qt6::QICOPlugin)
        add_custom_command(TARGET MAVIRA POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MAVIRA>/imageformats"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::QICOPlugin>"
                "$<TARGET_FILE_DIR:MAVIRA>/imageformats/"
            COMMENT "Deploying Qt Windows ICO imageformat plugin"
        )
    endif()
endif()

# ------------------------------------------------------------------------------
# Unit Tests (GoogleTest - Pure C++20, Zero Qt Dependencies)
# ------------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_executable(test_natural_sort
        tests/test_natural_sort.cpp
    )
    target_link_libraries(test_natural_sort PRIVATE
        mavira_build_options
        mavira_data
        GTest::gtest_main
    )

    add_executable(test_archive_and_merger
        tests/test_archive_and_merger.cpp
    )
    target_link_libraries(test_archive_and_merger PRIVATE
        mavira_build_options
        mavira_core
        mavira_data
        libzip::zip
        LibArchive::LibArchive
        GTest::gtest_main
    )

    add_executable(test_database_fts
        tests/test_database_fts.cpp
    )
    target_link_libraries(test_database_fts PRIVATE
        mavira_build_options
        mavira_data
        GTest::gtest_main
    )

    add_executable(test_archive_engine
        tests/test_archive_engine.cpp
    )
    target_link_libraries(test_archive_engine PRIVATE
        mavira_build_options
        mavira_core
        libzip::zip
        LibArchive::LibArchive
        GTest::gtest_main
    )

    add_executable(test_image_decoder
        tests/test_image_decoder.cpp
    )
    target_link_libraries(test_image_decoder PRIVATE
        mavira_build_options
        mavira_core
        PNG::PNG
        GTest::gtest_main
    )

    add_executable(test_lru_and_pack_cache
        tests/test_lru_and_pack_cache.cpp
    )
    target_link_libraries(test_lru_and_pack_cache PRIVATE
        mavira_build_options
        mavira_core
        libzip::zip
        GTest::gtest_main
    )

    add_executable(test_library_manager
        tests/test_library_manager.cpp
    )
    set_target_properties(test_library_manager PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(test_library_manager PRIVATE
        mavira_build_options
        mavira_app
        mavira_core
        mavira_data
        libzip::zip
        Qt6::Core
        GTest::gtest
    )

    add_executable(test_ui_shell
        tests/test_ui_shell.cpp
    )
    set_target_properties(test_ui_shell PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(test_ui_shell PRIVATE
        mavira_build_options
        mavira_ui
        mavira_app
        mavira_core
        mavira_data
        Qt6::Widgets
        GTest::gtest
    )

    add_executable(test_library_grid_and_settings
        tests/test_library_grid_and_settings.cpp
    )
    set_target_properties(test_library_grid_and_settings PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(test_library_grid_and_settings PRIVATE
        mavira_build_options
        mavira_ui
        mavira_app
        mavira_core
        mavira_data
        libzip::zip
        Qt6::Widgets
        GTest::gtest
    )

    add_executable(test_rendering_and_reader
        tests/test_rendering_and_reader.cpp
    )
    set_target_properties(test_rendering_and_reader PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(test_rendering_and_reader PRIVATE
        mavira_build_options
        mavira_app
        mavira_core
        mavira_data
        Qt6::Core
        GTest::gtest
    )

    add_executable(test_reader_viewport_and_shaders
        tests/test_reader_viewport_and_shaders.cpp
    )
    set_target_properties(test_reader_viewport_and_shaders PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(test_reader_viewport_and_shaders PRIVATE
        mavira_build_options
        mavira_ui
        mavira_app
        mavira_core
        mavira_data
        libzip::zip
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        GTest::gtest
    )

    add_executable(test_comic_merger
        tests/test_comic_merger.cpp
    )
    set_target_properties(test_comic_merger PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(test_comic_merger PRIVATE
        mavira_build_options
        mavira_ui
        mavira_app
        mavira_core
        mavira_data
        libzip::zip
        pdfium::pdfium
        Qt6::Widgets
        GTest::gtest
        $<$<BOOL:${WIN32}>:psapi>
    )

    add_executable(test_application_core
        tests/test_application_core.cpp
    )
    set_target_properties(test_application_core PROPERTIES
        AUTOMOC ON
    )
    target_link_libraries(test_application_core PRIVATE
        mavira_build_options
        mavira_ui
        mavira_app
        mavira_core
        mavira_data
        libzip::zip
        Qt6::Widgets
        GTest::gtest
    )

    include(GoogleTest)
    gtest_discover_tests(test_natural_sort)
    gtest_discover_tests(test_archive_and_merger)
    gtest_discover_tests(test_database_fts)
    gtest_discover_tests(test_archive_engine)
    gtest_discover_tests(test_image_decoder)
    gtest_discover_tests(test_lru_and_pack_cache)
    gtest_discover_tests(test_library_manager)
    gtest_discover_tests(test_ui_shell)
    gtest_discover_tests(test_library_grid_and_settings)
    gtest_discover_tests(test_rendering_and_reader)
    gtest_discover_tests(test_reader_viewport_and_shaders)
    gtest_discover_tests(test_comic_merger)
    gtest_discover_tests(test_application_core)
endif()
