

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    find_library(NVBUFSURFACE_LIBRARY nvbufsurface PATH_SUFFIXES nvidia)
    find_path(NVBUFSURFACE_INCLUDE_DIR nvbufsurface.h HINTS /usr/src/jetson_multimedia_api/include/)
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NvBufSurface DEFAULT_MSG
                                  NVBUFSURFACE_LIBRARY NVBUFSURFACE_INCLUDE_DIR)

if (NVBUFSURFACE_FOUND)
    add_library(NvBufSurface::NvBufSurface SHARED IMPORTED)
    set_target_properties(NvBufSurface::NvBufSurface PROPERTIES
        IMPORTED_LOCATION ${NVBUFSURFACE_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${NVBUFSURFACE_INCLUDE_DIR}
    )
endif ()