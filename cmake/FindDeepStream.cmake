# Locates the NVIDIA DeepStream SDK metadata libraries and headers.
#
# Used to attach a DeepStream NvDsMeta trigger record to outgoing buffers. nvstreammux transforms
# that NvDsMeta into an NvDsUserMeta on the matching NvDsFrameMeta, letting a DeepStream consumer
# read the trigger data straight off the frame without a bridging pad probe (see EXAMPLES.md).
#
#   gst_buffer_add_nvds_meta()  -> libnvdsgst_meta  (declared in gstnvdsmeta.h)
#   nvds_get_user_meta_type()   -> libnvds_meta     (declared in nvdsmeta.h)
#
# Defines the imported target DeepStream::Meta (linking both libraries) when found.

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    # /opt/nvidia/deepstream/deepstream is a version-independent symlink maintained by the SDK
    find_path(DEEPSTREAM_INCLUDE_DIR gstnvdsmeta.h
        HINTS /opt/nvidia/deepstream/deepstream/sources/includes)
    find_library(DEEPSTREAM_GST_META_LIBRARY nvdsgst_meta
        HINTS /opt/nvidia/deepstream/deepstream/lib)
    find_library(DEEPSTREAM_META_LIBRARY nvds_meta
        HINTS /opt/nvidia/deepstream/deepstream/lib)
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DeepStream DEFAULT_MSG
                                  DEEPSTREAM_GST_META_LIBRARY DEEPSTREAM_META_LIBRARY
                                  DEEPSTREAM_INCLUDE_DIR)

if (DEEPSTREAM_FOUND)
    add_library(DeepStream::Meta SHARED IMPORTED)
    set_target_properties(DeepStream::Meta PROPERTIES
        IMPORTED_LOCATION ${DEEPSTREAM_GST_META_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${DEEPSTREAM_INCLUDE_DIR}
        INTERFACE_LINK_LIBRARIES ${DEEPSTREAM_META_LIBRARY}
    )
endif ()
