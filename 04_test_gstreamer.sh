#!/bin/bash

# DEV_00012C0B5D0C = OFYMK
# DEV_00012C0B5D0D = OFYML
# echo "Testing GStreamer pipeline with vmbsrc to display 640x480 image..."
# read -p "Press Enter to continue..."
# echo "Startup may take a while..."
# gst-launch-1.0 vmbsrc camera=DEV_00012C0B5D0D width=640 height=480 ! videoscale ! videoconvert ! queue ! autovideosink -v

# echo "Testing GStreamer pipeline with vmbsrc to write to NVMM."
# read -p "Press Enter to continue..."
# echo "Startup may take a while..."
# export GST_TRACERS="framerate"
# export GST_DEBUG="GST_TRACER:7"
# export
gst-launch-1.0 -v vmbsrc camera=DEV_00012C0B5D0D userset=Default ! 'video/x-raw(memory:NVMM)' ! fpsdisplaysink video-sink=fakesink silent=false sync=false

# echo "Testing GStreamer pipeline with vmbsrc to display 640x480 image..."
# read -p "Press Enter to continue..."
# echo "Startup may take a while..."
# gst-launch-1.0 vmbsrc camera=DEV_00012C0B5D0D width=640 height=480 ! videoscale ! videoconvert ! queue ! autovideosink -v

# # with the following we can proove that actual frames are delivered from the cameras and flow through the pipeline, although the capsfilter shows framerate=(fraction)0/1
# # which just means the camera can not guarantee to deliver with a fixed framerate. Therefore framerate=(fraction)0/1 means "unspecified, variable, or unknown".
# gst-launch-1.0 -v vmbsrc camera=DEV_00012C0B5D0C userset=Default ! videoscale ! videoconvert ! queue ! fakesink

# Starting with preset
# gst-launch-1.0 vmbsrc camera=DEV_00012C0B5D0C userset=UserSet2 ! videoscale ! videoconvert ! queue ! autovideosink -v
