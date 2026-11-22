vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

set(PDFIUM_BUILD_NUMBER "8165")

vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${PDFIUM_BUILD_NUMBER}/pdfium-win-x64.tgz"
    FILENAME "pdfium-win-x64-${PDFIUM_BUILD_NUMBER}.tgz"
    SHA512 20a217dcc5fa85841f7c9a1997c7e89f196e5ac2172fa600a84d80c440f8e49750e0f670001c728113a97b8fcd6881bc69b81e0bd67e8f028887f8e3e9c4d916
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    NO_REMOVE_ONE_LEVEL
)

file(INSTALL "${SOURCE_PATH}/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(REMOVE "${CURRENT_PACKAGES_DIR}/include/fpdfview.h.orig")

file(INSTALL "${SOURCE_PATH}/lib/pdfium.dll.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
file(INSTALL "${SOURCE_PATH}/bin/pdfium.dll" DESTINATION "${CURRENT_PACKAGES_DIR}/bin")

if(NOT VCPKG_BUILD_TYPE)
    file(INSTALL "${SOURCE_PATH}/lib/pdfium.dll.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
    file(INSTALL "${SOURCE_PATH}/bin/pdfium.dll" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/pdfiumConfig.cmake" [=[
get_filename_component(_pdfium_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if(NOT TARGET pdfium::pdfium)
    add_library(pdfium::pdfium SHARED IMPORTED)
    set_target_properties(pdfium::pdfium PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_pdfium_prefix}/include;${_pdfium_prefix}/include/cpp"
        IMPORTED_LOCATION_RELEASE "${_pdfium_prefix}/bin/pdfium.dll"
        IMPORTED_IMPLIB_RELEASE "${_pdfium_prefix}/lib/pdfium.dll.lib"
        IMPORTED_LOCATION_DEBUG "${_pdfium_prefix}/debug/bin/pdfium.dll"
        IMPORTED_IMPLIB_DEBUG "${_pdfium_prefix}/debug/lib/pdfium.dll.lib"
        IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
    )
endif()

unset(_pdfium_prefix)
]=])

file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" [=[
pdfium provides CMake targets:

    find_package(pdfium CONFIG REQUIRED)
    target_link_libraries(main PRIVATE pdfium::pdfium)
]=])
