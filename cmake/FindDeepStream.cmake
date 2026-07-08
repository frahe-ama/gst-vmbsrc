# Locates the NVIDIA DeepStream SDK metadata library and headers.
#
# The latency-measurement helper nvds_set_input_system_timestamp() lives in
# libnvdsgst_meta and is declared in nvds_latency_meta.h. It lets a custom source
# act as the origin for DeepStream's latency measurement (NVDS_ENABLE_LATENCY_MEASUREMENT).
#
# Defines the imported target DeepStream::Meta when found.

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    # /opt/nvidia/deepstream/deepstream is a version-independent symlink maintained by the SDK
    find_path(DEEPSTREAM_INCLUDE_DIR nvds_latency_meta.h
        HINTS /opt/nvidia/deepstream/deepstream/sources/includes)
    find_library(DEEPSTREAM_META_LIBRARY nvdsgst_meta
        HINTS /opt/nvidia/deepstream/deepstream/lib)
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DeepStream DEFAULT_MSG
                                  DEEPSTREAM_META_LIBRARY DEEPSTREAM_INCLUDE_DIR)

if (DEEPSTREAM_FOUND)
    add_library(DeepStream::Meta SHARED IMPORTED)
    set_target_properties(DeepStream::Meta PROPERTIES
        IMPORTED_LOCATION ${DEEPSTREAM_META_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${DEEPSTREAM_INCLUDE_DIR}
    )
endif ()
