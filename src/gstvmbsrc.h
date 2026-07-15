/* GStreamer
 * Copyright (C) 2021 Allied Vision Technologies GmbH
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License version 2.0 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_vmbsrc_H_
#define _GST_vmbsrc_H_

#include "pixelformats.h"

#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>
#include <glib.h>

#include <VmbC/VmbC.h>
#include <VmbC/VmbCommonTypes.h>

#include <stdbool.h>

G_BEGIN_DECLS

#define GST_TYPE_vmbsrc (gst_vmbsrc_get_type())
#define GST_vmbsrc(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_vmbsrc, GstVmbSrc))
#define GST_vmbsrc_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_vmbsrc, GstVmbSrcClass))
#define GST_IS_vmbsrc(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_vmbsrc))
#define GST_IS_vmbsrc_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_vmbsrc))

/* Allowed values for "Auto" camera Features */
typedef enum
{
    GST_VMBSRC_AUTOFEATURE_UNCHANGED,
    GST_VMBSRC_AUTOFEATURE_OFF,
    GST_VMBSRC_AUTOFEATURE_ONCE,
    GST_VMBSRC_AUTOFEATURE_CONTINUOUS
} GstVmbSrcAutoFeatureValue;

// Possible values for TriggerSelector feature
typedef enum
{
    GST_VMBSRC_TRIGGERSELECTOR_UNCHANGED,
    GST_VMBSRC_TRIGGERSELECTOR_ACQUISITION_START,
    GST_VMBSRC_TRIGGERSELECTOR_ACQUISITION_END,
    GST_VMBSRC_TRIGGERSELECTOR_ACQUISITION_ACTIVE,
    GST_VMBSRC_TRIGGERSELECTOR_FRAME_START,
    GST_VMBSRC_TRIGGERSELECTOR_FRAME_END,
    GST_VMBSRC_TRIGGERSELECTOR_FRAME_ACTIVE,
    GST_VMBSRC_TRIGGERSELECTOR_FRAME_BURST_START,
    GST_VMBSRC_TRIGGERSELECTOR_FRAME_BURST_END,
    GST_VMBSRC_TRIGGERSELECTOR_FRAME_BURST_ACTIVE,
    GST_VMBSRC_TRIGGERSELECTOR_LINE_START,
    GST_VMBSRC_TRIGGERSELECTOR_EXPOSURE_START,
    GST_VMBSRC_TRIGGERSELECTOR_EXPOSURE_END,
    GST_VMBSRC_TRIGGERSELECTOR_EXPOSURE_ACTIVE
} GstVmbSrcTriggerSelectorValue;

// Possible values for TriggerMode feature
typedef enum
{
    GST_VMBSRC_TRIGGERMODE_UNCHANGED,
    GST_VMBSRC_TRIGGERMODE_OFF,
    GST_VMBSRC_TRIGGERMODE_ON
} GstVmbSrcTriggerModeValue;

// Possible values for the TriggerSource feature
typedef enum
{
    GST_VMBSRC_TRIGGERSOURCE_UNCHANGED,
    GST_VMBSRC_TRIGGERSOURCE_SOFTWARE,
    GST_VMBSRC_TRIGGERSOURCE_LINE0,
    GST_VMBSRC_TRIGGERSOURCE_LINE1,
    GST_VMBSRC_TRIGGERSOURCE_LINE2,
    GST_VMBSRC_TRIGGERSOURCE_LINE3,
    GST_VMBSRC_TRIGGERSOURCE_USER_OUTPUT0,
    GST_VMBSRC_TRIGGERSOURCE_USER_OUTPUT1,
    GST_VMBSRC_TRIGGERSOURCE_USER_OUTPUT2,
    GST_VMBSRC_TRIGGERSOURCE_USER_OUTPUT3,
    GST_VMBSRC_TRIGGERSOURCE_COUNTER0_START,
    GST_VMBSRC_TRIGGERSOURCE_COUNTER1_START,
    GST_VMBSRC_TRIGGERSOURCE_COUNTER2_START,
    GST_VMBSRC_TRIGGERSOURCE_COUNTER3_START,
    GST_VMBSRC_TRIGGERSOURCE_COUNTER0_END,
    GST_VMBSRC_TRIGGERSOURCE_COUNTER1_END,
    GST_VMBSRC_TRIGGERSOURCE_COUNTER2_END,
    GST_VMBSRC_TRIGGERSOURCE_COUNTER3_END,
    GST_VMBSRC_TRIGGERSOURCE_TIMER0_START,
    GST_VMBSRC_TRIGGERSOURCE_TIMER1_START,
    GST_VMBSRC_TRIGGERSOURCE_TIMER2_START,
    GST_VMBSRC_TRIGGERSOURCE_TIMER3_START,
    GST_VMBSRC_TRIGGERSOURCE_TIMER0_END,
    GST_VMBSRC_TRIGGERSOURCE_TIMER1_END,
    GST_VMBSRC_TRIGGERSOURCE_TIMER2_END,
    GST_VMBSRC_TRIGGERSOURCE_TIMER3_END,
    GST_VMBSRC_TRIGGERSOURCE_ENCODER0,
    GST_VMBSRC_TRIGGERSOURCE_ENCODER1,
    GST_VMBSRC_TRIGGERSOURCE_ENCODER2,
    GST_VMBSRC_TRIGGERSOURCE_ENCODER3,
    GST_VMBSRC_TRIGGERSOURCE_LOGIC_BLOCK0,
    GST_VMBSRC_TRIGGERSOURCE_LOGIC_BLOCK1,
    GST_VMBSRC_TRIGGERSOURCE_LOGIC_BLOCK2,
    GST_VMBSRC_TRIGGERSOURCE_LOGIC_BLOCK3,
    GST_VMBSRC_TRIGGERSOURCE_ACTION0,
    GST_VMBSRC_TRIGGERSOURCE_ACTION1,
    GST_VMBSRC_TRIGGERSOURCE_ACTION2,
    GST_VMBSRC_TRIGGERSOURCE_ACTION3,
    GST_VMBSRC_TRIGGERSOURCE_LINK_TRIGGER0,
    GST_VMBSRC_TRIGGERSOURCE_LINK_TRIGGER1,
    GST_VMBSRC_TRIGGERSOURCE_LINK_TRIGGER2,
    GST_VMBSRC_TRIGGERSOURCE_LINK_TRIGGER3
} GstVmbSrcTriggerSourceValue;

// Possible values for TriggerActivation feature
typedef enum
{
    GST_VMBSRC_TRIGGERACTIVATION_UNCHANGED,
    GST_VMBSRC_TRIGGERACTIVATION_RISING_EDGE,
    GST_VMBSRC_TRIGGERACTIVATION_FALLING_EDGE,
    GST_VMBSRC_TRIGGERACTIVATION_ANY_EDGE,
    GST_VMBSRC_TRIGGERACTIVATION_LEVEL_HIGH,
    GST_VMBSRC_TRIGGERACTIVATION_LEVEL_LOW
} GstVmbSrcTriggerActivationValue;

// Implemented handling approaches for incomplete frames
typedef enum
{
    GST_VMBSRC_INCOMPLETE_FRAME_HANDLING_DROP,
    GST_VMBSRC_INCOMPLETE_FRAME_HANDLING_SUBMIT
} GstVmbSrcIncompleteFrameHandlingValue;

// Frame buffer allocation modes
typedef enum
{
    GST_VMBSRC_ALLOCATION_MODE_ANNOUNCE_FRAME,
    GST_VMBSRC_ALLOCATION_MODE_ALLOC_AND_ANNOUNCE_FRAME
} GstVimbasrcAllocationMode;

typedef struct _GstVmbSrc GstVmbSrc;
typedef struct _GstVmbSrcClass GstVmbSrcClass;

/* One external hardware-trigger event, as fed in through the "notify-trigger" action signal. */
typedef struct _GstVmbSrcTriggerEvent
{
    guint64 seq;                 // sequence number assigned by the external trigger source
    GstClockTime monotonic_time; // trigger instant, CLOCK_MONOTONIC nanoseconds (g_get_monotonic_time()*1000)
} GstVmbSrcTriggerEvent;

/* -- Trigger metadata ------------------------------------------------------------------------
 * The external hardware-trigger information correlated to a frame is attached to every buffer
 * produced by vmbsrc so downstream consumers can pair each image with the right external data.
 *
 * It is carried as a GstCustomMeta (GStreamer >= 1.20) named GST_VMBSRC_TRIGGER_META_NAME whose
 * GstStructure holds the fields below. Unlike a struct-based GstMeta, a GstCustomMeta is
 * introspectable, so it can be read from Python without any custom bindings:
 *
 *     cmeta = buf.get_custom_meta("GstVmbSrcTriggerMeta")
 *     if cmeta is not None:
 *         s = cmeta.get_structure()
 *         ok, seq = s.get_uint64("trigger-seq")
 *
 * From C, use gst_buffer_get_vmbsrc_trigger_meta() to read the fields into a GstVmbSrcTriggerMeta.
 *
 * IMPORTANT: the standard DeepStream muxer (nvstreammux) does NOT forward arbitrary GstMeta
 * (custom metas included). To read this inside a DeepStream pipeline, use a pad probe on the
 * nvstreammux SINK pad(s) - where the original source buffer is still visible - and copy the
 * fields onto the corresponding NvDsFrameMeta (e.g. into misc_frame_info). See the reference
 * probe in EXAMPLES.md.
 */
#define GST_VMBSRC_TRIGGER_META_NAME "GstVmbSrcTriggerMeta"

// Plain value type holding the trigger fields read from a buffer's custom meta. Field values map
// one-to-one to the GstStructure keys documented in gst_buffer_get_vmbsrc_trigger_meta().
typedef struct _GstVmbSrcTriggerMeta
{
    guint64 trigger_seq;         // "trigger-seq": sequence number supplied by the external trigger
                                 // source; the frame identity that stays aligned across drops
    GstClockTime trigger_time;   // "trigger-time": the raw CLOCK_MONOTONIC trigger instant (ns)
                                 // exactly as fed into notify-trigger, propagated unchanged for
                                 // pairing with external data; or GST_CLOCK_TIME_NONE. (The buffer
                                 // PTS, not this field, carries the running-time conversion.)
    GstClockTime approx_latency; // "approx-latency": arrival-minus-trigger latency (ns), or NONE
    guint64 camera_frame_id;     // "camera-frame-id": VmbFrame_t.frameID as reported by the camera
    guint64 camera_timestamp;    // "camera-timestamp": VmbFrame_t.timestamp (raw camera clock ticks)
    gboolean correlated;         // "correlated": TRUE if a matching trigger event was found
} GstVmbSrcTriggerMeta;

// Registers the custom meta (idempotent, thread-safe). Called during element class init so that
// gst_buffer_add_custom_meta(GST_VMBSRC_TRIGGER_META_NAME) can find it.
const GstMetaInfo *gst_vmbsrc_trigger_meta_get_info(void);

// Reads the trigger custom meta from buffer into *out. Returns FALSE (leaving *out untouched) if
// the buffer carries no trigger meta.
gboolean gst_buffer_get_vmbsrc_trigger_meta(GstBuffer *buffer, GstVmbSrcTriggerMeta *out);

struct _GstVmbSrc
{
    GstPushSrc base_vmbsrc;

    struct
    {
        char *id;
        VmbHandle_t handle;
        VmbCameraInfo_t info;
        VmbUint32_t supported_formats_count;
        // TODO: This overallocates since no camera will actually support all possible format matches. Allocate and fill
        // at runtime?
        const VimbaXGstFormatMatch_t *supported_formats[NUM_FORMAT_MATCHES];
        bool is_connected;
        bool is_acquiring;
    } camera;
    struct
    {
        char *settings_file_path;
        char *userset;
        double exposuretime;
        int exposureauto;
        int balancewhiteauto;
        double gain;
        int offsetx;
        int offsety;
        int width;
        int height;
        int triggerselector;
        int triggermode;
        int triggersource;
        int triggeractivation;
        int incomplete_frame_handling;
        int allocation_mode;
        guint64 trigger_latency;            // nominal trigger->arrival latency in microseconds (0 = adaptive)
        guint64 trigger_latency_tolerance;  // match acceptance half-window in microseconds (0 = accept nearest)
        gboolean emit_trigger_latency_meta; // attach GstReferenceTimestampMeta for DeepStream latency
    } properties;

    int num_frame_buffers;
    VmbFrame_t* frame_buffers;
    // queue in which filled VimbaX frames are placed in the vimbax_frame_callback (attached to each queued frame at
    // frame->context[0])
    GAsyncQueue *filled_frame_queue;
    guint64 num_frames_pushed;
    GstVideoInfo video_info;

    bool use_nvmm;

    // Hardware-trigger correlation state, fed via the "notify-trigger" action signal and consumed
    // in gst_vmbsrc_create. Access is serialized by lock because triggers are pushed from an
    // external thread while frames are processed on the streaming thread.
    struct
    {
        GMutex lock;
        GstVmbSrcTriggerEvent *events; // ring buffer of pending trigger events (oldest at head)
        guint capacity;
        guint head;
        guint count;
        GstClockTime latency_estimate; // adaptive trigger->arrival latency (ns, monotonic domain)
        gboolean latency_valid;
        guint64 overflow_count;        // trigger events discarded because the ring was full
        gboolean any_received;         // latches TRUE on the first trigger event received during an
                                       // acquisition run; selects which identity space fills the
                                       // GstReferenceTimestampMeta frame_num (see gst_vmbsrc_create)
    } trigger;
};

struct _GstVmbSrcClass
{
    GstPushSrcClass base_vmbsrc_class;

    // Action signal handler: feed one external hardware-trigger event into the element for frame
    // correlation. trigger_time_ns is a CLOCK_MONOTONIC timestamp in nanoseconds (same reference
    // as g_get_monotonic_time()*1000 and the default GStreamer system clock).
    void (*notify_trigger)(GstVmbSrc *vmbsrc, guint64 trigger_seq, guint64 trigger_time_ns);
};

GType gst_vmbsrc_get_type(void);

G_END_DECLS

VmbError_t open_camera_connection(GstVmbSrc *vmbsrc);
VmbError_t load_user_set(GstVmbSrc *vmbsrc);
VmbError_t apply_feature_settings(GstVmbSrc *vmbsrc);
VmbError_t set_roi(GstVmbSrc *vmbsrc);
VmbError_t apply_trigger_settings(GstVmbSrc *vmbsrc);
VmbError_t alloc_and_announce_buffers(GstVmbSrc *vmbsrc, GstVideoInfo *video_info);
void revoke_and_free_buffers(GstVmbSrc *vmbsrc);
VmbError_t start_image_acquisition(GstVmbSrc *vmbsrc);
VmbError_t stop_image_acquisition(GstVmbSrc *vmbsrc);
void glib_destroy_callback(gpointer data);
void VMB_CALL vimbax_frame_callback(const VmbHandle_t cameraHandle, const VmbHandle_t stream_handle, VmbFrame_t *pFrame);
void map_supported_pixel_formats(GstVmbSrc *vmbsrc);
void log_available_enum_entries(GstVmbSrc *vmbsrc, const char *feat_name);

#endif
