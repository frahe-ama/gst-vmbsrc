#!/bin/bash
# Install GStreamer and its plugins

sudo apt-get install cmake

git clone --branch fix/latency-buffer-timestamps https://github.com/frahe-ama/gst-vmbsrc.git
cd gst-vmbsrc
# possibly adjust the CMakeUserPresets.json file to point to the correct Vimba SDK path
# Vmb_DIR for arm64
cp ../CMakeUserPresets.json.POLARIS CMakeUserPresets.json

# Build and install the plugin
cmake --preset arm64
cmake --build build-arm64

mkdir -p $HOME/.local/share/gstreamer-1.0/plugins
cp build-arm64/libgstvmbsrc.so $HOME/.local/share/gstreamer-1.0/plugins/

sudo apt install libssl-dev libgstrtspserver-1.0-0 libgirepository1.0-dev -y
source $HOME/.venv/bin/activate
# newer versions of pygobject require a newer version of glib, which is not available on Ubuntu 22.04, so we need to install an older version of pygobject
python -m pip install pygobject==3.50.2