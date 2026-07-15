# Usage Examples

GStreamer provides a larges selection of plugins, which can be used to define flexible pipelines for
many uses. Some examples of common goals are provided in this file.

## `vmbsrc` element properties

The examples below set camera features by passing them as properties directly on the `vmbsrc`
element, e.g. `vmbsrc camera=DEV_1AB22D01BBB8 gain=10 ! ...`. The full, up to date list can always be
queried with `gst-inspect-1.0 vmbsrc`; the table below is a quick reference.

| Property                | Type   | Default    | Description                                                                                          |
|--------------------------|--------|------------|-------------------------------------------------------------------------------------------------------|
| `camera`                | string | `""`       | ID of the camera images should be recorded from                                                       |
| `settingsfile`          | string | `""`       | Path to an XML file with camera settings to load. Mutually exclusive with `userset` and the individual feature properties below |
| `userset`               | string | `""`       | Name of a camera user set (e.g. `UserSet1`) to load via `UserSetSelector`/`UserSetLoad`. Mutually exclusive with `settingsfile` and the individual feature properties below |
| `exposuretime`          | double | `-1`       | `ExposureTime` in microseconds. Only applied when `ExposureAuto` is `Off`. `-1` leaves the camera's currently applied value unchanged |
| `exposureauto`          | enum   | `UNCHANGED`| `ExposureAuto` mode: `UNCHANGED`, `Off`, `Once`, `Continuous`                                          |
| `balancewhiteauto`      | enum   | `UNCHANGED`| `BalanceWhiteAuto` mode: `UNCHANGED`, `Off`, `Once`, `Continuous`                                      |
| `gain`                  | double | `-1`       | `Gain`, as an absolute physical value. `-1` leaves the camera's currently applied value unchanged      |
| `offsetx`               | int    | `G_MAXINT` | `OffsetX` in pixels. `-1` centers the ROI horizontally on the sensor. `G_MAXINT` leaves the camera's currently applied value unchanged |
| `offsety`               | int    | `G_MAXINT` | `OffsetY` in pixels. `-1` centers the ROI vertically on the sensor. `G_MAXINT` leaves the camera's currently applied value unchanged |
| `width`                 | int    | `-1`       | `Width` in pixels. `-1` leaves the camera's currently applied value unchanged                          |
| `height`                | int    | `-1`       | `Height` in pixels. `-1` leaves the camera's currently applied value unchanged                         |
| `triggerselector`       | enum   | `UNCHANGED`| `TriggerSelector`: `UNCHANGED`, `AcquisitionStart`, `AcquisitionEnd`, `AcquisitionActive`, `FrameStart`, `FrameEnd`, `FrameActive`, `FrameBurstStart`, `FrameBurstEnd`, `FrameBurstActive`, `LineStart`, `ExposureStart`, `ExposureEnd`, `ExposureActive` |
| `triggermode`           | enum   | `UNCHANGED`| `TriggerMode` for the selected trigger: `UNCHANGED`, `Off`, `On`                                       |
| `triggersource`         | enum   | `UNCHANGED`| `TriggerSource` for the selected trigger: `UNCHANGED`, `Line0`, `Line1`, `Line2`, `Line3`, `Action0`, `Action1`, `Action2`, `Action3`. More complex sources (e.g. `Software`, counters, timers) require an XML settings file |
| `triggeractivation`     | enum   | `UNCHANGED`| `TriggerActivation` for the selected trigger: `UNCHANGED`, `RisingEdge`, `FallingEdge`, `AnyEdge`, `LevelHigh`, `LevelLow` |
| `incompleteframehandling`| enum  | `Drop`     | How incomplete frames are handled: `Drop`, `Submit`                                                    |
| `allocationmode`        | enum   | `AnnounceFrame` | Frame buffer allocation strategy: `AnnounceFrame`, `AllocAndAnnounceFrame`                        |
| `framebuffers`          | int    | `5`        | Number of frame buffers allocated for transmission from the device to the host                        |
| `triggerlatency`        | uint64 | `0`        | Nominal trigger→arrival latency in microseconds, used to correlate frames with external trigger events. `0` = estimate from data. See [Correlating frames with an external hardware trigger](#correlating-frames-with-an-external-hardware-trigger) |
| `triggerlatencytolerance`| uint64| `0`        | Acceptance half-window in microseconds for a trigger match. `0` = always accept the nearest trigger    |
| `triggerlatencymeta`    | bool   | `true`     | Attach a `GstReferenceTimestampMeta` anchoring DeepStream latency at the trigger instant (frame arrival time for frames without a correlated trigger) — see [DeepStream latency anchored at the trigger](#deepstream-latency-anchored-at-the-trigger) |

Since `triggerselector`, `triggermode`, `triggersource` and `triggeractivation` each default to
`UNCHANGED`, setting up a trigger requires passing all of them together, e.g.
```
gst-launch-1.0 vmbsrc camera=DEV_1AB22D01BBB8 triggerselector=FrameStart triggermode=On triggersource=Line1 triggeractivation=RisingEdge ! ...
```

For details on `settingsfile` and `userset`, including why they cannot be combined with the other
feature properties, see the README.

## Saving camera frames as images

Recording pictures from a camera and saving them to some common image format allows for quick
inspections of the field of view, brightness and sharpness of the image. GStreamer provides image
encoders for different image formats. The example below uses the `png` encoder. A step by step
explanation of the elements in the pipeline is given below.
```
gst-launch-1.0 vmbsrc camera=DEV_1AB22D01BBB8 num-buffers=1 ! pngenc ! filesink location=out.png
```

- `vmbsrc camera=DEV_1AB22D01BBB8 num-buffers=1`: uses the `vmbsrc` element to grab one single frame
  from the camera with the given ID and halt the pipeline afterwards
- `pngenc`: takes the input image and encodes it into a `png` file
- `filesink location=out.png`: writes the data it receives (the encoded `png` image) to the file
  `out.png` in the current working directory

Similarly it is possible to save a number of camera frames to separate image files. This can be
achieved by using the `multifilesink` element to save the images.
```
gst-launch-1.0 vmbsrc camera=DEV_1AB22D01BBB8 num-buffers=10 ! pngenc ! multifilesink location=out_%03d.png
```

Similarly to the previous example, this pipeline uses the `vmbsrc` element to record images from the
camera. Here however 10 images are recorded. The `multifilesink` saves these images to separate
files, named `out_000.png`, `out_001.png`, ... , `out_009.png`.

Further changes to the pipeline are possible to, for example, change the format of the recorded
images to ensure RGB images, or adjust the exposure time of the image acquisition process. For more
details see the README of the `vmbsrc` element.

## Saving camera stream to a video file

To save a stream of images recorded by a camera to a video file the images should be encoded in some
video format and stored in an appropriate container format. This saves a lot of space compared to
just saving the raw image data. This example uses `h264` encoding for the image data and saves the
resulting video to an `avi` file. An explanation for the separate elements of the pipeline can be
found below.
```
gst-launch-1.0 vmbsrc camera=DEV_000F315B91E2 ! video/x-raw,format=RGB ! videorate ! video/x-raw,framerate=30/1 ! videoconvert ! queue ! x264enc ! avimux ! filesink location=output.avi
```

- `vmbsrc camera=DEV_000F315B91E2`: uses the `vmbsrc` element to grab camera frames from the Vimba X
  compatible camera with the given ID. For more information on the functionality of `vmbsrc`, see
  the README
- `video/x-raw,format=RGB`: a gst capsfilter element that limits the available data formats to `RGB`
  to ensure color images for the resulting video stream. Without this, the pipeline may negotiate
  grayscale images
- `videorate ! video/x-raw,framerate=30/1`: `vmbsrc` provides image data in a variable framerate
  (due to effects like possible hardware triggers or frame jitter). Because `avi` files only support
  fixed framerates, it needs to be modified via the `videorate` plugin. This guarantees a fixed
  framerate output by either copying the input data if more frames are requested than received, or
  dropping unnecessary frames if more frames are received than requested.
- `videoconvert ! queue`: converts the input image to a compatible video format for the following
  element
- `x264enc`: performs the encoding to h264 video
- `avimux`: multiplex the incoming video stream to save it as an `avi` file
- `filesink location=output.avi`: saves the resulting video into a file named `output.avi` in the
  current working directory

## Using  NVMM Memory for accelerated processing with Nvidia elements

If vmbsrc was compiled on a system that had the required libraries available and is able to load the
required libraries at runtime, it can produce frames that are stored in NvMM memory. These can be
passed to accelerated pipeline elements from Nvidia to speed up processing. The easiest way to
ensure that this is done is by using a `capsfilter` element in the pipeline:
```
gst-launch-1.0 vmbsrc camera=DEV_1AB22D01BBB8 ! video/x-raw(memory:NVMM) ! ...
```

## Increasing the number of framebuffers

If pipelines take a long time to process frames passed to them the vmbsrc might run out of buffers
to use for transmissions from the device. In such situations it can be helpful to increase the
number of buffers used by the element. For this the `framebuffers` property can be used:
```
gst-launch-1.0 vmbsrc camera=DEV_1AB22D01BBB8 framebuffers=25 ! ...
```

This property should not be confused with `num-buffers`, which is a standard GStreamer element
property that specifies the number of images the element should produce before halting execution.

## Stream video via RTSP server

RTSP (Real Time Streaming Protocol) is a network protocol designed to control streaming media
servers. It allows clients to send commands such as "play" or "pause" to the server, to enable
control of the media being streamed. The following example shows a minimal RTSP server using the
`vmbsrc` element to stream image data from a camera via the network to a client machine. This
example uses Python to start a pre-implemented RTSP server that can be imported via the PyGObject
package. To do this a few external dependencies must be installed.

### Dependencies

The following instructions assume an Ubuntu system. On other distributions different packages may be
required. It is also assumed, that a working GStreamer installation exists on the system and that
`vmbsrc` is available to that installation.

To have access to the GStreamer RTSP Server from python, the following system packages need to be
installed via the `apt` package manager:
- gir1.2-gst-rtsp-server-1.0
- libgirepository1.0-dev
- libcairo2-dev

Additionally the following python package needs to be installed. it is available via the Python
packaging index and can be installed as usual via `pip`:
- PyGObject

### Example code

The following python code will start an RTSP server on your machine listing on port `8554`. Be sure
to adjust the ID of the camera you want to use in the pipeline! As before the pipeline may be
adjusted to specify certain image formats to force for example color images or to change camera
settings like the exposure time.

```python
# import required GStreamer and GLib modules
import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import Gst, GLib, GstRtspServer

# initialize GStreamer and start the GLib Mainloop
Gst.init(None)
mainloop = GLib.MainLoop()

# Create the RTSP Server
server = GstRtspServer.RTSPServer()
mounts = server.get_mount_points()

# define the pipeline to record images ad attach it to the "stream1" endpoint
vmbsrc_factory = GstRtspServer.RTSPMediaFactory()
vmbsrc_factory.set_launch('vmbsrc camera=DEV_1AB22D01BBB8 ! videoconvert ! x264enc speed-preset=ultrafast tune=zerolatency ! rtph264pay name=pay0')
mounts.add_factory("/stream1", vmbsrc_factory)
server.attach(None)

mainloop.run()
```

To start the server, simply save the code above to a file (e.g. `RTSP_Server.py`) and run it with
your python interpreter. The RTSP server can be stopped by halting the process. This is done by
pressing `CTRL + c` in the terminal that is running the python script.

### Displaying the stream

After starting the python example a client must connect to the running RTSP server to receive and
display the stream. This can for example be done with the VLC media player. To display the stream on
the same machine that is running the RTSP server, open `rtsp://127.0.0.1:8554/stream1` as a Network
Stream (in VLC open "Media" -> "Open Network Stream"). If you want to play the video on a different
computer, ensure that a network connection between the two systems exists and use the IP address of
the machine running the RTSP server for your Network Stream.

Upon starting playback in VLC the RTSP Server will start the GStreamer pipeline that was defined in
the python file and start streaming images. It may take some time for the stream to start. Stopping
playback will also stop the GStreamer pipeline and close the connection to the camera being used.

## Correlating frames with an external hardware trigger

When the camera is driven by an external hardware trigger (e.g. GPIO pulses generated by another
thread), `vmbsrc` can attach the trigger time and the external trigger sequence number to each
frame. This lets a downstream consumer pair every image with the right external data even when the
data transfer is slower than the trigger rate and frames sit queued or get dropped.

The mechanism has three parts:

1. **Feed trigger events in** through the `notify-trigger` action signal.
2. **Correlation** happens inside the element by timestamp proximity (see the `triggerlatency` /
   `triggerlatencytolerance` properties).
3. **Read the result** from the `GstVmbSrcTriggerMeta` attached to every buffer (and, optionally, a
   trigger-anchored DeepStream latency meta).

### Feeding trigger events in

Immediately after (or before) generating each hardware trigger, the trigger thread emits the
`notify-trigger` action signal with the trigger's sequence number and the time it fired. The time
must be a `CLOCK_MONOTONIC` timestamp **in nanoseconds** (the same reference as
`g_get_monotonic_time() * 1000` and the default GStreamer system clock).

> Emit the signal right when the GPIO is toggled — not after reading back any data — so the trigger
> event reliably reaches the element before the corresponding frame arrives.

C:

```c
#include <time.h>

static guint64 monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (guint64)ts.tv_sec * 1000000000ULL + (guint64)ts.tv_nsec;
}

// seq is your own per-trigger counter
g_signal_emit_by_name(vmbsrc, "notify-trigger", (guint64)seq, monotonic_ns());
```

Python (PyGObject):

```python
import time
# vmbsrc is the Gst.Element
vmbsrc.emit("notify-trigger", seq, time.clock_gettime_ns(time.CLOCK_MONOTONIC))
```

### Telling the element the nominal latency

If you know the approximate time between a trigger firing and the frame arriving, set it at startup.
It makes correlation correct from the very first frame and defines an acceptance window that lets the
element detect and skip triggers whose frame was dropped:

```
vmbsrc camera=DEV_1AB22D01BBB8 triggerlatency=8000 triggerlatencytolerance=2000 ! ...
```

`triggerlatency` and `triggerlatencytolerance` are in **microseconds**. `triggerlatency=0` (default)
makes the element estimate the latency purely from the data; a non-zero value is refined at runtime.
With `triggerlatencytolerance=0` the nearest trigger is always accepted; a non-zero value marks a
frame uncorrelated when no trigger falls within the window.

### What ends up on each buffer

- **Buffer PTS** — anchored at the instant the frame's originating event happened, not the moment
  the frame was dequeued for pushing. The anchor depends on whether a trigger is correlated to the
  frame (see [Buffer PTS: trigger vs. no-trigger cases](#buffer-pts-trigger-vs-no-trigger-cases)
  below). In a DeepStream pipeline this propagates into `NvDsFrameMeta.buf_pts` for free.
- **`GstVmbSrcTriggerMeta`** — a `GstCustomMeta` (named `GstVmbSrcTriggerMeta`, defined in
  `gstvmbsrc.h`) whose `GstStructure` carries the full record. Being a custom meta, it is readable
  from Python without bindings (see below). The C struct field / Python structure key / meaning:

  | C field            | Python key         | Meaning                                                        |
  |--------------------|--------------------|----------------------------------------------------------------|
  | `trigger_seq`      | `trigger-seq`      | the external sequence number — the frame identity for pairing as fed into `notify-trigger` |
  | `trigger_time`     | `trigger-time`     | raw CLOCK_MONOTONIC trigger instant (ns) as fed into `notify-trigger`, or `GST_CLOCK_TIME_NONE`; the running-time conversion is in the buffer PTS |
  | `approx_latency`   | `approx-latency`   | arrival − trigger (ns): the transfer latency for this frame    |
  | `camera_frame_id`  | `camera-frame-id`  | `VmbFrame_t.frameID` reported by the camera                    |
  | `camera_timestamp` | `camera-timestamp` | `VmbFrame_t.timestamp` (raw camera clock ticks)                |
  | `correlated`       | `correlated`       | `TRUE` if a matching trigger was found                         |

- **`GstReferenceTimestampMeta`** (when `triggerlatencymeta=true`, the default): anchors DeepStream
  latency measurement at the trigger instant. See the note at the end.
- **DeepStream `NvDsMeta`** (only when `vmbsrc` was built with the DeepStream SDK): carries
  `trigger_seq`, `trigger_time`, `camera_frame_id` and `correlated` in a form `nvstreammux` copies
  onto `NvDsFrameMeta` without a probe — see [Bridging the meta into DeepStream frame
  metadata](#bridging-the-meta-into-deepstream-frame-metadata).

### Buffer PTS: trigger vs. no-trigger cases

Every buffer is stamped with the pipeline running-time of the instant its *originating event*
happened, recovered from a `CLOCK_MONOTONIC` timestamp taken inside the frame-received callback (the
moment the frame arrived from the camera). This is deliberately **not** the time the frame was
dequeued from the internal queue and pushed downstream — a frame that waited in the queue (e.g.
while the pipeline drained a backlog) keeps the timestamp of when it really arrived, so downstream
latency analysis stays accurate. `do-timestamp` is disabled so `GstBaseSrc` does not overwrite this
value with its own dequeue-time stamp.

Which instant becomes the PTS depends on whether the frame could be correlated to an external
trigger:

| Case | Condition | Buffer PTS |
|------|-----------|------------|
| **Correlated trigger** | Triggers are being fed in via `notify-trigger` and one matched this frame within the acceptance window (`correlated = TRUE`) | The **trigger instant** — the most meaningful anchor for latency analysis and for pairing the image with external data |
| **No correlated trigger** | Triggering is not in use at all, *or* it is but no trigger matched this frame (`correlated = FALSE`) | The **frame arrival time** (when the frame was received from the camera) |
| **Fallback** | Neither a trigger nor an arrival timestamp is available (should not normally occur) | The **current running-time** at push time |

In all cases the past `CLOCK_MONOTONIC` instant is mapped into pipeline running-time using only the
elapsed monotonic delta, which stays correct for whatever clock the pipeline selected as long as it
advances in real time. When the pipeline runs the default monotonic system clock, the conversion is
done exactly (a single `base_time` subtraction) to avoid importing the microsecond quantisation of
the delta bridge.

Whether a given frame was trigger-anchored or arrival-anchored is always visible in the
`GstVmbSrcTriggerMeta` `correlated` field, so a downstream consumer can tell the two cases apart
per frame.

### Reading the meta in a plain GStreamer pipeline

The record is attached as a **`GstCustomMeta`** named `GstVmbSrcTriggerMeta` (GStreamer ≥ 1.20), so
it can be read from either C or Python, from a pad probe or an `appsink` sample — as long as no
element between the source and the read point drops custom metas.

From **C**, use the typed accessor `gst_buffer_get_vmbsrc_trigger_meta()`, which fills a
`GstVmbSrcTriggerMeta` value struct from the buffer (returns `FALSE` if the meta is absent):

```c
#include "gstvmbsrc.h"

static GstPadProbeReturn read_trigger_meta(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    GstVmbSrcTriggerMeta m;
    if (gst_buffer_get_vmbsrc_trigger_meta(buf, &m) && m.correlated)
        g_print("frame trigger_seq=%" G_GUINT64_FORMAT " latency=%" G_GUINT64_FORMAT " us\n",
                m.trigger_seq, m.approx_latency / 1000);
    return GST_PAD_PROBE_OK;
}
```

From **Python**, no custom bindings are needed — read the custom meta's `GstStructure` directly.
The fields use dashed keys (`trigger-seq`, `trigger-time`, `approx-latency`, `camera-frame-id`,
`camera-timestamp`, `correlated`) and every numeric field of the custom meta is a `guint64`.

Two related values live **outside** the custom meta:

- **PTS** is on the buffer itself (`buf.pts`) — the running-time anchor described in
  [Buffer PTS: trigger vs. no-trigger cases](#buffer-pts-trigger-vs-no-trigger-cases). It is
  `Gst.CLOCK_TIME_NONE` only if no clock was available.
- **`frame_num`** is carried in the `GstReferenceTimestampMeta` (present when
  `triggerlatencymeta=true`), stored as a `gint` in its caps structure. It equals `trigger-seq`
  once triggering is in use (`0` for a frame that failed to correlate), or the delivered-frame
  counter before any trigger has been seen.

```python
import time
from gi.repository import Gst


def mono_to_wall_ns(mono_ns):
    """Translate a CLOCK_MONOTONIC instant (ns) into a CLOCK_REALTIME wall-clock instant (ns).
    The offset between the two clocks drifts slowly, so sample it close to when it is used."""
    if mono_ns == Gst.CLOCK_TIME_NONE:
        return None
    offset = time.time_ns() - time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    return mono_ns + offset


def running_time_to_mono_ns(running_time_ns, base_time_ns):
    """Translate a pipeline running-time into an absolute CLOCK_MONOTONIC instant (ns).
    Valid when the pipeline runs the default monotonic system clock (the common case):
    clock_time = running_time + base_time, and that clock IS g_get_monotonic_time()."""
    if running_time_ns == Gst.CLOCK_TIME_NONE:
        return None
    return running_time_ns + base_time_ns


# base_time_ns = <pipeline>.get_base_time(), captured once after the pipeline reaches PLAYING.
def read_trigger_meta(buf, base_time_ns):
    # PTS lives on the buffer, independent of the custom meta.
    pts = buf.pts  # pipeline running-time, or Gst.CLOCK_TIME_NONE if unset

    cmeta = buf.get_custom_meta("GstVmbSrcTriggerMeta")
    if cmeta is None:
        return None
    s = cmeta.get_structure()
    ok, correlated  = s.get_boolean("correlated")
    ok, seq         = s.get_uint64("trigger-seq")
    ok, latency_ns  = s.get_uint64("approx-latency")
    ok, trigger_ns  = s.get_uint64("trigger-time")       # raw CLOCK_MONOTONIC (ns), or NONE
    ok, cam_ts      = s.get_uint64("camera-timestamp")   # raw camera clock ticks

    # frame_num is on the reference-timestamp meta (triggerlatencymeta=true), not the custom meta.
    frame_num = None
    rmeta = buf.get_reference_timestamp_meta(None)
    if rmeta is not None:
        ok, frame_num = rmeta.reference.get_structure(0).get_int("frame_num")

    # Translate the frame's anchor instant into the monotonic and wall clocks.
    #  - trigger-time is already a raw CLOCK_MONOTONIC value.
    #  - PTS (running-time) becomes monotonic via the pipeline base time, then wall-clock.
    pts_mono  = running_time_to_mono_ns(pts, base_time_ns)
    pts_wall  = mono_to_wall_ns(pts_mono) if pts_mono is not None else None
    trig_wall = mono_to_wall_ns(trigger_ns)

    print(f"pts={pts} frame_num={frame_num} correlated={correlated} "
          f"trigger_seq={seq} camera_timestamp={cam_ts}")
    print(f"  pts -> mono={pts_mono} ns  wall={pts_wall} ns")
    if correlated:
        print(f"  trigger_time mono={trigger_ns} ns  wall={trig_wall} ns  latency={latency_ns // 1000} us")
    return seq

# from an appsink sample (get_buffer() does not copy the image — see note below):
sample = appsink.emit("pull-sample")
read_trigger_meta(sample.get_buffer(), pipeline.get_base_time())
```

> `camera-timestamp` is on the **camera's own clock domain** (raw ticks at the sensor's clock
> frequency), not `CLOCK_MONOTONIC`, so it cannot be translated with the helpers above — it is only
> meaningful relative to other `camera-timestamp` values from the same camera. Use `trigger-time`
> (or the PTS) for host-clock correlation.


### Bridging the meta into DeepStream frame metadata

There are two ways to get the trigger record onto `NvDsFrameMeta`. Which one applies depends on how
`vmbsrc` was built.

#### Preferred: the built-in `NvDsMeta` (no probe)

`nvstreammux` drops plain custom `GstMeta` (the `GstVmbSrcTriggerMeta` above never survives the
muxer), **but** it does transform an `NvDsMeta` attached upstream into an `NvDsUserMeta` on the
matching `NvDsFrameMeta`. When `vmbsrc` is built with the DeepStream SDK present (the CMake build
finds it and defines `HAVE_DEEPSTREAM`), it attaches exactly such an `NvDsMeta` to every buffer, so
**no bridging probe is needed** — the muxer copies the record onto the right frame for you, correctly
paired per source. The payload carries `trigger_seq`, `trigger_time`, `camera_frame_id` and
`correlated`.

Read it downstream (e.g. on the `pgie`/`nvinfer` src pad, or `nvdsosd` sink pad) by walking
`frame_user_meta_list` and matching the user-meta type, which both sides derive from the same string
`"VMBSRC.TRIGGER.USERMETA"` via `nvds_get_user_meta_type()`:

```c
#include "gstnvdsmeta.h"

// Must match the payload struct vmbsrc attaches (see gst_vmbsrc_create in gstvmbsrc.c). These are
// the same fields as GstVmbSrcTriggerMeta.
typedef struct {
    guint64 trigger_seq;
    guint64 trigger_time;      // CLOCK_MONOTONIC ns, or GST_CLOCK_TIME_NONE if uncorrelated
    guint64 approx_latency;    // arrival - trigger (ns), or GST_CLOCK_TIME_NONE
    guint64 camera_frame_id;
    guint64 camera_timestamp;  // raw camera clock ticks
    gboolean correlated;
} GstVmbSrcNvDsTriggerMeta;

static GstPadProbeReturn read_frame_trigger(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    NvDsBatchMeta *batch = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch) return GST_PAD_PROBE_OK;
    guint vmbsrc_type = nvds_get_user_meta_type("VMBSRC.TRIGGER.USERMETA");
    for (NvDsMetaList *l = batch->frame_meta_list; l; l = l->next) {
        NvDsFrameMeta *fm = (NvDsFrameMeta *)l->data;
        for (NvDsMetaList *u = fm->frame_user_meta_list; u; u = u->next) {
            NvDsUserMeta *um = (NvDsUserMeta *)u->data;
            if (um->base_meta.meta_type == (gint)vmbsrc_type) {
                GstVmbSrcNvDsTriggerMeta *t = (GstVmbSrcNvDsTriggerMeta *)um->user_meta_data;
                if (t->correlated)
                    g_print("source=%u frame trigger_seq=%" G_GUINT64_FORMAT "\n",
                            fm->pad_index, t->trigger_seq);
            }
        }
    }
    return GST_PAD_PROBE_OK;
}
```

In **Python** the read is the same shape (the payload is an opaque blob, so cast it with `ctypes`):

```python
import ctypes
import pyds


class VmbTriggerMeta(ctypes.Structure):
    _fields_ = [("trigger_seq", ctypes.c_uint64),
                ("trigger_time", ctypes.c_uint64),
                ("approx_latency", ctypes.c_uint64),
                ("camera_frame_id", ctypes.c_uint64),
                ("camera_timestamp", ctypes.c_uint64),
                ("correlated", ctypes.c_int)]


def read_frame_trigger(pad, info, u_data):
    batch = pyds.gst_buffer_get_nvds_batch_meta(hash(info.get_buffer()))
    if batch is None:
        return Gst.PadProbeReturn.OK
    vmbsrc_type = pyds.nvds_get_user_meta_type("VMBSRC.TRIGGER.USERMETA")
    l_frame = batch.frame_meta_list
    while l_frame is not None:
        frame_meta = pyds.NvDsFrameMeta.cast(l_frame.data)
        l_user = frame_meta.frame_user_meta_list
        while l_user is not None:
            user_meta = pyds.NvDsUserMeta.cast(l_user.data)
            if user_meta.base_meta.meta_type == vmbsrc_type:
                t = ctypes.cast(pyds.get_ptr(user_meta.user_meta_data),
                                ctypes.POINTER(VmbTriggerMeta)).contents
                # trigger_time is CLOCK_MONOTONIC ns, or GST_CLOCK_TIME_NONE (2**64-1) if uncorrelated
                trigger_time = None if t.trigger_time == Gst.CLOCK_TIME_NONE else t.trigger_time
                latency = None if t.approx_latency == Gst.CLOCK_TIME_NONE else t.approx_latency
                print(f"source={frame_meta.pad_index} trigger_seq={t.trigger_seq} "
                      f"trigger_time={trigger_time} approx_latency={latency} "
                      f"camera_frame_id={t.camera_frame_id} camera_timestamp={t.camera_timestamp} "
                      f"correlated={bool(t.correlated)}")
            l_user = l_user.next
        l_frame = l_frame.next
    return Gst.PadProbeReturn.OK
```

> `pyds.nvds_get_user_meta_type()` must be exposed by your `pyds` build (it is in recent releases).
> If it is not, obtain the id once from a C helper (or read it from any of vmbsrc's user metas at
> startup) and hardcode it — `nvds_get_user_meta_type()` returns the same value for a given string
> for the lifetime of the process.

#### Fallback: copy the custom meta across with probes (no DeepStream SDK at build time)

If `vmbsrc` was built **without** the DeepStream SDK, it emits only the plain `GstVmbSrcTriggerMeta`,
which `nvstreammux` drops. Bridge it across the muxer with two probes: read the source buffer's meta
on the muxer **sink** pad into a per-source FIFO, then pop it on the muxer **src** pad and write it
onto the matching `NvDsFrameMeta` (order is preserved per source). The lightest target is
`misc_frame_info[]` — four spare `gint64` slots already present on every frame, so no custom
meta-type registration is needed.

```c
#include "gstvmbsrc.h"
#include "gstnvdsmeta.h"

// One GQueue of {trigger_seq, trigger_time, camera_frame_id, correlated} per source pad.
static GQueue *seq_fifo;          // GLib queue of heap-allocated gint64[4]
static GMutex  seq_lock;

// Probe on the nvstreammux SINK pad: stash the trigger info from the source buffer.
static GstPadProbeReturn on_mux_sink(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    GstVmbSrcTriggerMeta m;
    gboolean have = gst_buffer_get_vmbsrc_trigger_meta(buf, &m);
    gint64 *rec = g_new(gint64, 4);
    rec[0] = have ? (gint64)m.trigger_seq     : -1;
    rec[1] = have ? (gint64)m.trigger_time    : -1;   // CLOCK_MONOTONIC ns; -1 (== NONE) if uncorrelated
    rec[2] = have ? (gint64)m.camera_frame_id : -1;
    rec[3] = have ? (m.correlated ? 1 : 0)    : -1;
    g_mutex_lock(&seq_lock);
    g_queue_push_tail(seq_fifo, rec);
    g_mutex_unlock(&seq_lock);
    return GST_PAD_PROBE_OK;
}

// Probe on the nvstreammux SRC pad: pop and write onto each NvDsFrameMeta.
static GstPadProbeReturn on_mux_src(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    NvDsBatchMeta *batch = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch) return GST_PAD_PROBE_OK;
    for (NvDsMetaList *l = batch->frame_meta_list; l; l = l->next) {
        NvDsFrameMeta *fm = (NvDsFrameMeta *)l->data;
        g_mutex_lock(&seq_lock);
        gint64 *rec = g_queue_pop_head(seq_fifo);
        g_mutex_unlock(&seq_lock);
        if (rec) {
            fm->misc_frame_info[0] = rec[0];   // trigger_seq
            fm->misc_frame_info[1] = rec[1];   // trigger_time (ns, CLOCK_MONOTONIC; -1 == NONE)
            fm->misc_frame_info[2] = rec[2];   // camera_frame_id
            fm->misc_frame_info[3] = rec[3];   // correlated (0/1)
            g_free(rec);
        }
    }
    return GST_PAD_PROBE_OK;
}
```

For a single source this FIFO is sufficient; with several cameras batched together, key the FIFO by
`source_id` (one queue per pad) so sequences are not interleaved. If you prefer a richer, typed
record over the four `misc_frame_info` slots, allocate an `NvDsUserMeta` in the src-pad probe
(`nvds_acquire_user_meta_from_pool` / `nvds_add_user_meta_to_frame`) with your own meta type instead.

### DeepStream latency anchored at the trigger

With `triggerlatencymeta=true` (the default) each buffer carries a `GstReferenceTimestampMeta`
whose caps structure holds the anchor instant — the trigger for correlated frames, the frame
arrival time otherwise — as a wall-clock in/out timestamp pair under the component name
`<element-name>-trigger`. The **new** `nvstreammux`
(`gst-nvmultistream2`) converts it into a DeepStream latency meta, so with
`NVDS_ENABLE_COMPONENT_LATENCY_MEASUREMENT=1` the trigger→source interval appears as its own
component line in `nvds_measure_buffer_latency()` output.

> **Making it drive the overall "Frame latency" — element naming requirement.**
> `nvds_measure_buffer_latency()` computes `now − comp_in_timestamp` in wall-clock milliseconds, and
> it hardcodes (verified by disassembly of `libnvdsgst_meta.so`, DeepStream 7.0) that only a latency
> meta whose component name starts with `nvv4l2decode` or `audiodecoder` may provide that baseline.
> Metas under any other name are printed as per-component lines but ignored for the frame total; if
> no decoder-named meta is present the baseline stays `0` and "Frame latency" prints as the current
> epoch time (~`1.78e12` ms). To anchor the frame latency at the trigger instant, simply name the
> element so its `-trigger` meta passes the check:
> ```
> gst-launch-1.0 vmbsrc name=nvv4l2decoder_cam_0 camera=DEV_... ! ...
> ```
> Frames that could not be correlated to a trigger (including the no-trigger case, when
> `notify-trigger` is never emitted) fall back to the **frame arrival time** as the anchor, so the
> frame counter keeps counting and the overall latency stays sane; it then measures
> arrival→measurement instead of trigger→measurement. Whether a given frame was trigger-anchored
> is visible in the `GstVmbSrcTriggerMeta` `correlated` field.
>
> Calling `nvds_set_input_system_timestamp()` from the source (or any pad probe upstream of
> `nvstreammux`) is **not** a workable alternative: that function needs `NvDsBatchMeta`, which only
> exists downstream of the muxer, so it silently does nothing there; and post-mux metas are appended
> after the muxer's own entry in the batch list, too late to serve as the baseline.

Disable the meta with `triggerlatencymeta=false` if you only want the metadata carriage.
