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
/**
 * SECTION:element-gstvmbsrc
 *
 * The vmbsrc element provides a way to stream image data into GStreamer pipelines from cameras
 * using the VimbaX API
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v vmbsrc camera=<CAMERAID> ! videoconvert ! autovideosink
 * ]|
 * Display a stream from the given camera
 * </refsect2>
 */

#include "gstvmbsrc.h"
#include "helpers.h"
#include "vimbax_helpers.h"
#include "pixelformats.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/video/video.h>
#include <gst/video/video-info.h>
#include <glib.h>

#ifdef _WIN32
#include <stdlib.h>
#endif

#if HAVE_NVMM
#include <nvbufsurface.h>
#endif

#if HAVE_DEEPSTREAM
#include <gstnvdsmeta.h>
#endif

#include <VmbC/VmbC.h>

// Counter variable to keep track of calls to VmbStartup() and VmbShutdown()
static unsigned int vmb_open_count = 0;
G_LOCK_DEFINE(vmb_open_count);

GST_DEBUG_CATEGORY_STATIC(gst_vmbsrc_debug_category);
#define GST_CAT_DEFAULT gst_vmbsrc_debug_category

#define GST_CAPS_FEATURE_MEMORY_NVMM      "memory:NVMM"

#if HAVE_NVMM

#define GST_VMBSRC_CAPS_NVMM \
    ";" GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ GRAY8, RGB, BGR, UYVY, BGRx, RGBA }")
#else
#define GST_VMBSRC_CAPS_NVMM
#endif

#define GST_VMBSRC_CAPS_DEFAULT \
    GST_VIDEO_CAPS_MAKE(GST_VIDEO_FORMATS_ALL) ";" GST_BAYER_CAPS_MAKE(GST_BAYER_FORMATS_ALL)

/* prototypes */

static void gst_vmbsrc_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec);
static void gst_vmbsrc_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec);
static void gst_vmbsrc_finalize(GObject *object);

static GstCaps *gst_vmbsrc_get_caps(GstBaseSrc *src, GstCaps *filter);
static gboolean gst_vmbsrc_set_caps(GstBaseSrc *src, GstCaps *caps);
static gboolean gst_vmbsrc_start(GstBaseSrc *src);
static gboolean gst_vmbsrc_stop(GstBaseSrc *src);

static GstFlowReturn gst_vmbsrc_create(GstPushSrc *src, GstBuffer **buf);

static void gst_vmbsrc_notify_trigger(GstVmbSrc *vmbsrc, guint64 trigger_seq, guint64 trigger_time_ns);
static gboolean gst_vmbsrc_correlate_trigger(GstVmbSrc *vmbsrc,
                                             GstClockTime arrival_monotonic_ns,
                                             guint64 *out_seq,
                                             GstClockTime *out_trigger_monotonic_ns,
                                             GstClockTime *out_latency_ns);

// Number of pending trigger events the element buffers while waiting for the matching frames to
// be delivered. With correct correlation only about one in-flight trigger is expected (two at
// most); the ring is kept just large enough to absorb that, and logs a warning if it overflows.
#define GST_VMBSRC_TRIGGER_RING_CAPACITY 4

enum
{
    SIGNAL_NOTIFY_TRIGGER,
    LAST_SIGNAL
};
static guint gst_vmbsrc_signals[LAST_SIGNAL] = {0};

enum
{
    PROP_0,
    PROP_CAMERA_ID,
    PROP_SETTINGS_FILENAME,
    PROP_USERSET,
    PROP_EXPOSURETIME,
    PROP_EXPOSUREAUTO,
    PROP_BALANCEWHITEAUTO,
    PROP_GAIN,
    PROP_OFFSETX,
    PROP_OFFSETY,
    PROP_WIDTH,
    PROP_HEIGHT,
    PROP_TRIGGERSELECTOR,
    PROP_TRIGGERMODE,
    PROP_TRIGGERSOURCE,
    PROP_TRIGGERACTIVATION,
    PROP_INCOMPLETE_FRAME_HANDLING,
    PROP_ALLOCATION_MODE,
    PROP_NUM_FRAME_BUFFERS,
    PROP_TRIGGERLATENCY,
    PROP_TRIGGERLATENCYTOLERANCE,
    PROP_TRIGGERLATENCYMETA
};

/* pad templates */
static GstStaticPadTemplate gst_vmbsrc_src_template =
    GST_STATIC_PAD_TEMPLATE("src",
                            GST_PAD_SRC,
                            GST_PAD_ALWAYS,
                            GST_STATIC_CAPS(GST_VMBSRC_CAPS_DEFAULT GST_VMBSRC_CAPS_NVMM));

/* Auto exposure modes */
#define GST_ENUM_EXPOSUREAUTO_MODES (gst_vmbsrc_exposureauto_get_type())
static GType gst_vmbsrc_exposureauto_get_type(void)
{
    static GType vmbsrc_exposureauto_type = 0;
    static const GEnumValue exposureauto_modes[] = {
        /* The "nick" (last entry) will be used to pass the setting value on to the VimbaX FeatureEnum */
        {GST_VMBSRC_AUTOFEATURE_UNCHANGED, "Does not change the currently applied exposureauto value on the device", "UNCHANGED"},
        {GST_VMBSRC_AUTOFEATURE_OFF, "Exposure duration is usercontrolled using ExposureTime", "Off"},
        {GST_VMBSRC_AUTOFEATURE_ONCE, "Exposure duration is adapted once by the device. Once it has converged, it returns to the Offstate", "Once"},
        {GST_VMBSRC_AUTOFEATURE_CONTINUOUS, "Exposure duration is constantly adapted by the device to maximize the dynamic range", "Continuous"},
        {0, NULL, NULL}};
    if (!vmbsrc_exposureauto_type)
    {
        vmbsrc_exposureauto_type =
            g_enum_register_static("GstVmbSrcExposureAutoModes", exposureauto_modes);
    }
    return vmbsrc_exposureauto_type;
}

/* Auto white balance modes */
#define GST_ENUM_BALANCEWHITEAUTO_MODES (gst_vmbsrc_balancewhiteauto_get_type())
static GType gst_vmbsrc_balancewhiteauto_get_type(void)
{
    static GType vmbsrc_balancewhiteauto_type = 0;
    static const GEnumValue balancewhiteauto_modes[] = {
        /* The "nick" (last entry) will be used to pass the setting value on to the VimbaX FeatureEnum */
        {GST_VMBSRC_AUTOFEATURE_UNCHANGED, "Does not change the currently applied balancewhiteauto value on the device", "UNCHANGED"},
        {GST_VMBSRC_AUTOFEATURE_OFF, "White balancing is user controlled using BalanceRatioSelector and BalanceRatio", "Off"},
        {GST_VMBSRC_AUTOFEATURE_ONCE, "White balancing is automatically adjusted once by the device. Once it has converged, it automatically returns to the Off state", "Once"},
        {GST_VMBSRC_AUTOFEATURE_CONTINUOUS, "White balancing is constantly adjusted by the device", "Continuous"},
        {0, NULL, NULL}};
    if (!vmbsrc_balancewhiteauto_type)
    {
        vmbsrc_balancewhiteauto_type =
            g_enum_register_static("GstVmbSrcBalanceWhiteAutoModes", balancewhiteauto_modes);
    }
    return vmbsrc_balancewhiteauto_type;
}

/* TriggerSelector values */
#define GST_ENUM_TRIGGERSELECTOR_VALUES (gst_vmbsrc_triggerselector_get_type())
static GType gst_vmbsrc_triggerselector_get_type(void)
{
    static GType vmbsrc_triggerselector_type = 0;
    static const GEnumValue triggerselector_values[] = {
        /* The "nick" (last entry) will be used to pass the setting value on to the VimbaX FeatureEnum */
        {GST_VMBSRC_TRIGGERSELECTOR_UNCHANGED, "Does not change the currently applied triggerselector value on the device", "UNCHANGED"},
        {GST_VMBSRC_TRIGGERSELECTOR_ACQUISITION_START, "Selects a trigger that starts the Acquisition of one or many frames according to AcquisitionMode", "AcquisitionStart"},
        {GST_VMBSRC_TRIGGERSELECTOR_ACQUISITION_END, "Selects a trigger that ends the Acquisition of one or many frames according to AcquisitionMode", "AcquisitionEnd"},
        {GST_VMBSRC_TRIGGERSELECTOR_ACQUISITION_ACTIVE, "Selects a trigger that controls the duration of the Acquisition of one or many frames. The Acquisition is activated when the trigger signal becomes active and terminated when it goes back to the inactive state", "AcquisitionActive"},
        {GST_VMBSRC_TRIGGERSELECTOR_FRAME_START, "Selects a trigger starting the capture of one frame", "FrameStart"},
        {GST_VMBSRC_TRIGGERSELECTOR_FRAME_END, "Selects a trigger ending the capture of one frame (mainly used in linescanmode)", "FrameEnd"},
        {GST_VMBSRC_TRIGGERSELECTOR_FRAME_ACTIVE, "Selects a trigger controlling the duration of one frame (mainly used in linescanmode)", "FrameActive"},
        {GST_VMBSRC_TRIGGERSELECTOR_FRAME_BURST_START, "Selects a trigger starting the capture of the bursts of frames in an acquisition. AcquisitionBurstFrameCount controls the length of each burst unless a FrameBurstEnd trigger is active. The total number of frames captured is also conditioned by AcquisitionFrameCount if AcquisitionMode is MultiFrame", "FrameBurstStart"},
        {GST_VMBSRC_TRIGGERSELECTOR_FRAME_BURST_END, "Selects a trigger ending the capture of the bursts of frames in an acquisition", "FrameBurstEnd"},
        {GST_VMBSRC_TRIGGERSELECTOR_FRAME_BURST_ACTIVE, "Selects a trigger controlling the duration of the capture of the bursts of frames in an acquisition", "FrameBurstActive"},
        {GST_VMBSRC_TRIGGERSELECTOR_LINE_START, "Selects a trigger starting the capture of one Line of a Frame (mainly used in linescanmode)", "LineStart"},
        {GST_VMBSRC_TRIGGERSELECTOR_EXPOSURE_START, "Selects a trigger controlling the start of the exposure of one Frame (or Line)", "ExposureStart"},
        {GST_VMBSRC_TRIGGERSELECTOR_EXPOSURE_END, "Selects a trigger controlling the end of the exposure of one Frame (or Line)", "ExposureEnd"},
        {GST_VMBSRC_TRIGGERSELECTOR_EXPOSURE_ACTIVE, "Selects a trigger controlling the duration of the exposure of one frame (or Line)", "ExposureActive"},
        {0, NULL, NULL}};
    if (!vmbsrc_triggerselector_type)
    {
        vmbsrc_triggerselector_type =
            g_enum_register_static("GstVmbSrcTriggerSelectorValues", triggerselector_values);
    }
    return vmbsrc_triggerselector_type;
}

/* TriggerMode values */
#define GST_ENUM_TRIGGERMODE_VALUES (gst_vmbsrc_triggermode_get_type())
static GType gst_vmbsrc_triggermode_get_type(void)
{
    static GType vmbsrc_triggermode_type = 0;
    static const GEnumValue triggermode_values[] = {
        /* The "nick" (last entry) will be used to pass the setting value on to the VimbaX FeatureEnum */
        {GST_VMBSRC_TRIGGERMODE_UNCHANGED, "Does not change the currently applied triggermode value on the device", "UNCHANGED"},
        {GST_VMBSRC_TRIGGERMODE_OFF, "Disables the selected trigger", "Off"},
        {GST_VMBSRC_TRIGGERMODE_ON, "Enable the selected trigger", "On"},
        {0, NULL, NULL}};
    if (!vmbsrc_triggermode_type)
    {
        vmbsrc_triggermode_type =
            g_enum_register_static("GstVmbSrcTriggerModeValues", triggermode_values);
    }
    return vmbsrc_triggermode_type;
}

/* TriggerSource values */
#define GST_ENUM_TRIGGERSOURCE_VALUES (gst_vmbsrc_triggersource_get_type())
static GType gst_vmbsrc_triggersource_get_type(void)
{
    static GType vmbsrc_triggersource_type = 0;
    static const GEnumValue triggersource_values[] = {
        /* The "nick" (last entry) will be used to pass the setting value on to the VimbaX FeatureEnum */
        // Commented out trigger sources require more complex setups and should be performed via XML configuration file
        {GST_VMBSRC_TRIGGERSOURCE_UNCHANGED, "Does not change the currently applied triggersource value on the device", "UNCHANGED"},
        // {GST_VMBSRC_TRIGGERSOURCE_SOFTWARE, "Specifies that the trigger source will be generated by software using the TriggerSoftware command", "Software"},
        {GST_VMBSRC_TRIGGERSOURCE_LINE0, "Specifies which physical line (or pin) and associated I/O control block to use as external source for the trigger signal", "Line0"},
        {GST_VMBSRC_TRIGGERSOURCE_LINE1, "Specifies which physical line (or pin) and associated I/O control block to use as external source for the trigger signal", "Line1"},
        {GST_VMBSRC_TRIGGERSOURCE_LINE2, "Specifies which physical line (or pin) and associated I/O control block to use as external source for the trigger signal", "Line2"},
        {GST_VMBSRC_TRIGGERSOURCE_LINE3, "Specifies which physical line (or pin) and associated I/O control block to use as external source for the trigger signal", "Line3"},
        // {GST_VMBSRC_TRIGGERSOURCE_USER_OUTPUT0, "Specifies which User Output bit signal to use as internal source for the trigger", "UserOutput0"},
        // {GST_VMBSRC_TRIGGERSOURCE_USER_OUTPUT1, "Specifies which User Output bit signal to use as internal source for the trigger", "UserOutput1"},
        // {GST_VMBSRC_TRIGGERSOURCE_USER_OUTPUT2, "Specifies which User Output bit signal to use as internal source for the trigger", "UserOutput2"},
        // {GST_VMBSRC_TRIGGERSOURCE_USER_OUTPUT3, "Specifies which User Output bit signal to use as internal source for the trigger", "UserOutput3"},
        // {GST_VMBSRC_TRIGGERSOURCE_COUNTER0_START, "Specifies which of the Counter signal to use as internal source for the trigger", "Counter0Start"},
        // {GST_VMBSRC_TRIGGERSOURCE_COUNTER1_START, "Specifies which of the Counter signal to use as internal source for the trigger", "Counter1Start"},
        // {GST_VMBSRC_TRIGGERSOURCE_COUNTER2_START, "Specifies which of the Counter signal to use as internal source for the trigger", "Counter2Start"},
        // {GST_VMBSRC_TRIGGERSOURCE_COUNTER3_START, "Specifies which of the Counter signal to use as internal source for the trigger", "Counter3Start"},
        // {GST_VMBSRC_TRIGGERSOURCE_COUNTER0_END, "Specifies which of the Counter signal to use as internal source for the trigger", "Counter0End"},
        // {GST_VMBSRC_TRIGGERSOURCE_COUNTER1_END, "Specifies which of the Counter signal to use as internal source for the trigger", "Counter1End"},
        // {GST_VMBSRC_TRIGGERSOURCE_COUNTER2_END, "Specifies which of the Counter signal to use as internal source for the trigger", "Counter2End"},
        // {GST_VMBSRC_TRIGGERSOURCE_COUNTER3_END, "Specifies which of the Counter signal to use as internal source for the trigger", "Counter3End"},
        // {GST_VMBSRC_TRIGGERSOURCE_TIMER0_START, "Specifies which Timer signal to use as internal source for the trigger", "Timer0Start"},
        // {GST_VMBSRC_TRIGGERSOURCE_TIMER1_START, "Specifies which Timer signal to use as internal source for the trigger", "Timer1Start"},
        // {GST_VMBSRC_TRIGGERSOURCE_TIMER2_START, "Specifies which Timer signal to use as internal source for the trigger", "Timer2Start"},
        // {GST_VMBSRC_TRIGGERSOURCE_TIMER3_START, "Specifies which Timer signal to use as internal source for the trigger", "Timer3Start"},
        // {GST_VMBSRC_TRIGGERSOURCE_TIMER0_END, "Specifies which Timer signal to use as internal source for the trigger", "Timer0End"},
        // {GST_VMBSRC_TRIGGERSOURCE_TIMER1_END, "Specifies which Timer signal to use as internal source for the trigger", "Timer1End"},
        // {GST_VMBSRC_TRIGGERSOURCE_TIMER2_END, "Specifies which Timer signal to use as internal source for the trigger", "Timer2End"},
        // {GST_VMBSRC_TRIGGERSOURCE_TIMER3_END, "Specifies which Timer signal to use as internal source for the trigger", "Timer3End"},
        // {GST_VMBSRC_TRIGGERSOURCE_ENCODER0, "Specifies which Encoder signal to use as internal source for the trigger", "Encoder0"},
        // {GST_VMBSRC_TRIGGERSOURCE_ENCODER1, "Specifies which Encoder signal to use as internal source for the trigger", "Encoder1"},
        // {GST_VMBSRC_TRIGGERSOURCE_ENCODER2, "Specifies which Encoder signal to use as internal source for the trigger", "Encoder2"},
        // {GST_VMBSRC_TRIGGERSOURCE_ENCODER3, "Specifies which Encoder signal to use as internal source for the trigger", "Encoder3"},
        // {GST_VMBSRC_TRIGGERSOURCE_LOGIC_BLOCK0, "Specifies which Logic Block signal to use as internal source for the trigger", "LogicBlock0"},
        // {GST_VMBSRC_TRIGGERSOURCE_LOGIC_BLOCK1, "Specifies which Logic Block signal to use as internal source for the trigger", "LogicBlock1"},
        // {GST_VMBSRC_TRIGGERSOURCE_LOGIC_BLOCK2, "Specifies which Logic Block signal to use as internal source for the trigger", "LogicBlock2"},
        // {GST_VMBSRC_TRIGGERSOURCE_LOGIC_BLOCK3, "Specifies which Logic Block signal to use as internal source for the trigger", "LogicBlock3"},
        {GST_VMBSRC_TRIGGERSOURCE_ACTION0, "Specifies which Action command to use as internal source for the trigger", "Action0"},
        {GST_VMBSRC_TRIGGERSOURCE_ACTION1, "Specifies which Action command to use as internal source for the trigger", "Action1"},
        {GST_VMBSRC_TRIGGERSOURCE_ACTION2, "Specifies which Action command to use as internal source for the trigger", "Action2"},
        {GST_VMBSRC_TRIGGERSOURCE_ACTION3, "Specifies which Action command to use as internal source for the trigger", "Action3"},
        // {GST_VMBSRC_TRIGGERSOURCE_LINK_TRIGGER0, "Specifies which Link Trigger to use as source for the trigger (received from the transport layer)", "LinkTrigger0"},
        // {GST_VMBSRC_TRIGGERSOURCE_LINK_TRIGGER1, "Specifies which Link Trigger to use as source for the trigger (received from the transport layer)", "LinkTrigger1"},
        // {GST_VMBSRC_TRIGGERSOURCE_LINK_TRIGGER2, "Specifies which Link Trigger to use as source for the trigger (received from the transport layer)", "LinkTrigger2"},
        // {GST_VMBSRC_TRIGGERSOURCE_LINK_TRIGGER3, "Specifies which Link Trigger to use as source for the trigger (received from the transport layer)", "LinkTrigger3"},
        {0, NULL, NULL}};
    if (!vmbsrc_triggersource_type)
    {
        vmbsrc_triggersource_type =
            g_enum_register_static("GstVmbSrcTriggerSourceValues", triggersource_values);
    }
    return vmbsrc_triggersource_type;
}

/* TriggerActivation values */
#define GST_ENUM_TRIGGERACTIVATION_VALUES (gst_vmbsrc_triggeractivation_get_type())
static GType gst_vmbsrc_triggeractivation_get_type(void)
{
    static GType vmbsrc_triggeractivation_type = 0;
    static const GEnumValue triggeractivation_values[] = {
        /* The "nick" (last entry) will be used to pass the setting value on to the VimbaX FeatureEnum */
        {GST_VMBSRC_TRIGGERACTIVATION_UNCHANGED, "Does not change the currently applied triggeractivation value on the device", "UNCHANGED"},
        {GST_VMBSRC_TRIGGERACTIVATION_RISING_EDGE, "Specifies that the trigger is considered valid on the rising edge of the source signal", "RisingEdge"},
        {GST_VMBSRC_TRIGGERACTIVATION_FALLING_EDGE, "Specifies that the trigger is considered valid on the falling edge of the source signal", "FallingEdge"},
        {GST_VMBSRC_TRIGGERACTIVATION_ANY_EDGE, "Specifies that the trigger is considered valid on the falling or rising edge of the source signal", "AnyEdge"},
        {GST_VMBSRC_TRIGGERACTIVATION_LEVEL_HIGH, "Specifies that the trigger is considered valid as long as the level of the source signal is high", "LevelHigh"},
        {GST_VMBSRC_TRIGGERACTIVATION_LEVEL_LOW, "Specifies that the trigger is considered valid as long as the level of the source signal is low", "LevelLow"},
        {0, NULL, NULL}};
    if (!vmbsrc_triggeractivation_type)
    {
        vmbsrc_triggeractivation_type =
            g_enum_register_static("GstVmbSrcTriggerActivationValues", triggeractivation_values);
    }
    return vmbsrc_triggeractivation_type;
}

/* IncompleteFrameHandling values */
#define GST_ENUM_INCOMPLETEFRAMEHANDLING_VALUES (gst_vmbsrc_incompleteframehandling_get_type())
static GType gst_vmbsrc_incompleteframehandling_get_type(void)
{
    static GType vmbsrc_incompleteframehandling_type = 0;
    static const GEnumValue incompleteframehandling_values[] = {
        {GST_VMBSRC_INCOMPLETE_FRAME_HANDLING_DROP, "Drop incomplete frames", "Drop"},
        {GST_VMBSRC_INCOMPLETE_FRAME_HANDLING_SUBMIT, "Use incomplete frames and submit them to the next element for processing", "Submit"},
        {0, NULL, NULL}};
    if (!vmbsrc_incompleteframehandling_type)
    {
        vmbsrc_incompleteframehandling_type =
            g_enum_register_static("GstVmbSrcIncompleteFrameHandlingValues", incompleteframehandling_values);
    }
    return vmbsrc_incompleteframehandling_type;
}

/* Frame buffer allocation modes */
#define GST_ENUM_ALLOCATIONMODE_VALUES (gst_vmbsrc_allocationmode_get_type())
static GType gst_vmbsrc_allocationmode_get_type(void)
{
    static GType vmbsrc_allocationmode_type = 0;
    static const GEnumValue allocationmode_values[] = {
        {GST_VMBSRC_ALLOCATION_MODE_ANNOUNCE_FRAME, "Allocate buffers in the plugin", "AnnounceFrame"},
        {GST_VMBSRC_ALLOCATION_MODE_ALLOC_AND_ANNOUNCE_FRAME, "Let the transport layer allocate buffers", "AllocAndAnnounceFrame"},
        {0, NULL, NULL}};
    if (!vmbsrc_allocationmode_type)
    {
        vmbsrc_allocationmode_type =
            g_enum_register_static("GstVimbasrcAllocationModeValues", allocationmode_values);
    }
    return vmbsrc_allocationmode_type;
}

/* class initialization */

G_DEFINE_TYPE_WITH_CODE(GstVmbSrc,
                        gst_vmbsrc,
                        GST_TYPE_PUSH_SRC,
                        GST_DEBUG_CATEGORY_INIT(gst_vmbsrc_debug_category,
                                                "vmbsrc",
                                                0,
                                                "debug category for vmbsrc element"))

static void gst_vmbsrc_class_init(GstVmbSrcClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS(klass);
    GstPushSrcClass *push_src_class = GST_PUSH_SRC_CLASS(klass);

    /* Setting up pads and setting metadata should be moved to base_class_init if you intend to subclass this class. */
    gst_element_class_add_static_pad_template(GST_ELEMENT_CLASS(klass),
                                              &gst_vmbsrc_src_template);

    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
                                          "VimbaX GStreamer source",
                                          "Generic",
                                          DESCRIPTION,
                                          "Allied Vision Technologies GmbH");

    gobject_class->set_property = gst_vmbsrc_set_property;
    gobject_class->get_property = gst_vmbsrc_get_property;
    gobject_class->finalize = gst_vmbsrc_finalize;
    base_src_class->get_caps = GST_DEBUG_FUNCPTR(gst_vmbsrc_get_caps);
    base_src_class->set_caps = GST_DEBUG_FUNCPTR(gst_vmbsrc_set_caps);
    base_src_class->start = GST_DEBUG_FUNCPTR(gst_vmbsrc_start);
    base_src_class->stop = GST_DEBUG_FUNCPTR(gst_vmbsrc_stop);
    push_src_class->create = GST_DEBUG_FUNCPTR(gst_vmbsrc_create);

    // Install properties
    g_object_class_install_property(
        gobject_class,
        PROP_CAMERA_ID,
        g_param_spec_string(
            "camera",
            "Camera ID",
            "ID of the camera images should be recorded from",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_SETTINGS_FILENAME,
        g_param_spec_string(
            "settingsfile",
            "Camera settings filepath",
            "Path to XML file containing camera settings that should be applied, loaded via VmbSettingsLoad. "
            "Other feature settings passed as element properties are ignored while this is set. Leave empty "
            "to not load a settings file.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_USERSET,
        g_param_spec_string(
            "userset",
            "Camera user set to load",
            "Name of a camera user set (e.g. \"UserSet1\") to load via UserSetSelector/UserSetLoad. Other "
            "feature settings passed as element properties are ignored while this is set. Unlike "
            "\"settingsfile\" this loads a set stored on the camera itself instead of an XML file. Leave "
            "empty to not load a user set.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_EXPOSURETIME,
        g_param_spec_double(
            "exposuretime",
            "ExposureTime feature setting",
            "Sets the Exposure time (in microseconds) when ExposureMode is Timed and ExposureAuto is Off. This controls the duration where the photosensitive cells are exposed to light. If -1 is passed the currently applied value on the device is left unchanged.",
            -1.,
            G_MAXDOUBLE,
            -1.,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_EXPOSUREAUTO,
        g_param_spec_enum(
            "exposureauto",
            "ExposureAuto feature setting",
            "Sets the auto exposure mode. The output of the auto exposure function affects the whole image",
            GST_ENUM_EXPOSUREAUTO_MODES,
            GST_VMBSRC_AUTOFEATURE_UNCHANGED,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_BALANCEWHITEAUTO,
        g_param_spec_enum(
            "balancewhiteauto",
            "BalanceWhiteAuto feature setting",
            "Controls the mode for automatic white balancing between the color channels. The white balancing ratios are automatically adjusted",
            GST_ENUM_BALANCEWHITEAUTO_MODES,
            GST_VMBSRC_AUTOFEATURE_UNCHANGED,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_GAIN,
        g_param_spec_double(
            "gain",
            "Gain feature setting",
            "Controls the selected gain as an absolute physical value. This is an amplification factor applied to the video signal. If -1 is passed the currently applied value on the device is left unchanged.",
            -1.,
            G_MAXDOUBLE,
            -1.,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_OFFSETX,
        g_param_spec_int(
            "offsetx",
            "OffsetX feature setting",
            "Horizontal offset from the origin to the region of interest (in pixels). If -1 is passed the ROI will be centered in the sensor along the horizontal axis. If G_MAXINT (the default) is passed the currently applied value on the device is left unchanged.",
            -1,
            G_MAXINT,
            G_MAXINT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_OFFSETY,
        g_param_spec_int(
            "offsety",
            "OffsetY feature setting",
            "Vertical offset from the origin to the region of interest (in pixels). If -1 is passed the ROI will be centered in the sensor along the vertical axis. If G_MAXINT (the default) is passed the currently applied value on the device is left unchanged.",
            -1,
            G_MAXINT,
            G_MAXINT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_WIDTH,
        g_param_spec_int(
            "width",
            "Width feature setting",
            "Width of the image provided by the device (in pixels). If -1 (the default) is passed the currently applied value on the device is left unchanged.",
            -1,
            G_MAXINT,
            -1,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_HEIGHT,
        g_param_spec_int(
            "height",
            "Height feature setting",
            "Height of the image provided by the device (in pixels). If -1 (the default) is passed the currently applied value on the device is left unchanged.",
            -1,
            G_MAXINT,
            -1,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_TRIGGERSELECTOR,
        g_param_spec_enum(
            "triggerselector",
            "TriggerSelector feature setting",
            "Selects the type of trigger to configure. Not all cameras support every trigger selector listed below. Check which selectors are supported by the used camera model",
            GST_ENUM_TRIGGERSELECTOR_VALUES,
            GST_VMBSRC_TRIGGERSELECTOR_UNCHANGED,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_TRIGGERMODE,
        g_param_spec_enum(
            "triggermode",
            "TriggerMode feature setting",
            "Controls if the selected trigger is active",
            GST_ENUM_TRIGGERMODE_VALUES,
            GST_VMBSRC_TRIGGERMODE_UNCHANGED,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_TRIGGERSOURCE,
        g_param_spec_enum(
            "triggersource",
            "TriggerSource feature setting",
            "Specifies the internal signal or physical input Line to use as the trigger source. The selected trigger must have its TriggerMode set to On. Not all cameras support every trigger source listed below. Check which sources are supported by the used camera model",
            GST_ENUM_TRIGGERSOURCE_VALUES,
            GST_VMBSRC_TRIGGERSOURCE_UNCHANGED,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_TRIGGERACTIVATION,
        g_param_spec_enum(
            "triggeractivation",
            "TriggerActivation feature setting",
            "Specifies the activation mode of the trigger. Not all cameras support every trigger activation listed below. Check which activations are supported by the used camera model",
            GST_ENUM_TRIGGERACTIVATION_VALUES,
            GST_VMBSRC_TRIGGERACTIVATION_UNCHANGED,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_INCOMPLETE_FRAME_HANDLING,
        g_param_spec_enum(
            "incompleteframehandling",
            "Incomplete frame handling",
            "Determines how the element should handle received frames where data transmission was incomplete. Incomplete frames may contain pixel intensities from old acquisitions or random data",
            GST_ENUM_INCOMPLETEFRAMEHANDLING_VALUES,
            GST_VMBSRC_INCOMPLETE_FRAME_HANDLING_DROP,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_ALLOCATION_MODE,
        g_param_spec_enum(
            "allocationmode",
            "Buffer allocation strategy",
            "Decides if frame buffers should be allocated by the gstreamer element itself or by the transport layer",
            GST_ENUM_ALLOCATIONMODE_VALUES,
            GST_VMBSRC_ALLOCATION_MODE_ANNOUNCE_FRAME,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_NUM_FRAME_BUFFERS,
        g_param_spec_int(
            "framebuffers",
            "Number of frame buffers to allocate",
            "Configures the number of frame buffers that are allocated for transmission from the device to the host. This number should be chosen large enough to not starve the system of free buffers for transmission",
            1,
            G_MAXINT,
            5,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_TRIGGERLATENCY,
        g_param_spec_uint64(
            "triggerlatency",
            "Nominal trigger-to-arrival latency",
            "Approximate time (in microseconds) between an external hardware trigger firing and the "
            "corresponding frame arriving in this element. Used to correlate delivered frames with "
            "the trigger events fed in via the \"notify-trigger\" signal. Providing a good estimate "
            "makes correlation correct from the first frame; the value is refined at runtime. Set to "
            "0 to let the element estimate it purely from the data.",
            0,
            G_MAXUINT64,
            0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_TRIGGERLATENCYTOLERANCE,
        g_param_spec_uint64(
            "triggerlatencytolerance",
            "Trigger match tolerance",
            "Half-width (in microseconds) of the acceptance window around the expected trigger time. "
            "A frame is correlated to a trigger only if that trigger lies within this window; "
            "otherwise the frame is marked uncorrelated. Set to 0 to always accept the nearest "
            "trigger regardless of distance.",
            0,
            G_MAXUINT64,
            0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class,
        PROP_TRIGGERLATENCYMETA,
        g_param_spec_boolean(
            "triggerlatencymeta",
            "Emit DeepStream trigger latency meta",
            "When TRUE, attach a GstReferenceTimestampMeta describing the trigger->source interval "
            "so DeepStream's latency measurement (NVDS_ENABLE_LATENCY_MEASUREMENT) is anchored at the "
            "trigger instant. Frames without a correlated trigger (e.g. when notify-trigger is not "
            "used at all) fall back to the frame arrival time as the anchor. Only takes effect with "
            "the new nvstreammux; harmless otherwise.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /* Action signal used by an external thread (e.g. the GPIO trigger generator) to hand a trigger
     * event - a sequence number and a CLOCK_MONOTONIC timestamp in nanoseconds - to the element:
     *   g_signal_emit_by_name(vmbsrc, "notify-trigger", (guint64)seq, (guint64)time_ns); */
    klass->notify_trigger = gst_vmbsrc_notify_trigger;
    gst_vmbsrc_signals[SIGNAL_NOTIFY_TRIGGER] = g_signal_new(
        "notify-trigger",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
        G_STRUCT_OFFSET(GstVmbSrcClass, notify_trigger),
        NULL,
        NULL,
        NULL, /* use the generic (libffi) marshaller */
        G_TYPE_NONE,
        2,
        G_TYPE_UINT64,
        G_TYPE_UINT64);

    // Force registration of the trigger custom meta now so gst_buffer_add_custom_meta() can find
    // it by name in create(). Idempotent and thread-safe.
    gst_vmbsrc_trigger_meta_get_info();
}

static void gst_vmbsrc_init(GstVmbSrc *vmbsrc)
{
    GST_TRACE_OBJECT(vmbsrc, "init");
    GST_INFO_OBJECT(vmbsrc, "gst-vmbsrc version %s", VERSION);
    VmbError_t result = VmbErrorSuccess;
    // Start the VimbaX API
    G_LOCK(vmb_open_count);
    if (0 == vmb_open_count++)
    {
        result = VmbStartup(NULL);
        GST_DEBUG_OBJECT(vmbsrc, "VmbStartup returned: %s", ErrorCodeToMessage(result));
        if (result != VmbErrorSuccess)
        {
            GST_ERROR_OBJECT(vmbsrc, "VimbaX initialization failed");
        }
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "VmbStartup was already called. Current open count: %u", vmb_open_count);
    }
    G_UNLOCK(vmb_open_count);

    // Log the used VmbC version
    VmbVersionInfo_t version_info;
    result = VmbVersionQuery(&version_info, sizeof(version_info));
    if (result == VmbErrorSuccess)
    {
        GST_INFO_OBJECT(vmbsrc,
                        "Running with VmbC Version %u.%u.%u",
                        version_info.major,
                        version_info.minor,
                        version_info.patch);
    }
    else
    {
        GST_WARNING_OBJECT(vmbsrc, "VmbVersionQuery failed with Reason: %s", ErrorCodeToMessage(result));
    }

    // Mark this element as a live source (disable preroll)
    gst_base_src_set_live(GST_BASE_SRC(vmbsrc), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(vmbsrc), GST_FORMAT_TIME);
    // Do NOT let GstBaseSrc timestamp the buffers automatically. do-timestamp stamps the buffer
    // with the running-time at the moment create() returns (i.e. when the frame is dequeued),
    // which hides the time a frame spent waiting in filled_frame_queue and adds jitter. We set an
    // accurate frame-arrival timestamp ourselves in gst_vmbsrc_create instead.
    gst_base_src_set_do_timestamp(GST_BASE_SRC(vmbsrc), FALSE);

    // Set property helper variables to default values
    GObjectClass *gobject_class = G_OBJECT_GET_CLASS(vmbsrc);

    vmbsrc->camera.id = g_value_dup_string(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "camera")));
    vmbsrc->properties.settings_file_path = g_value_dup_string(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "settingsfile")));
    vmbsrc->properties.userset = g_value_dup_string(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "userset")));
    vmbsrc->properties.exposuretime = g_value_get_double(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "exposuretime")));
    vmbsrc->properties.exposureauto = g_value_get_enum(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "exposureauto")));
    vmbsrc->properties.balancewhiteauto = g_value_get_enum(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "balancewhiteauto")));
    vmbsrc->properties.gain = g_value_get_double(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "gain")));
    vmbsrc->properties.offsetx = g_value_get_int(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "offsetx")));
    vmbsrc->properties.offsety = g_value_get_int(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "offsety")));
    vmbsrc->properties.width = g_value_get_int(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "width")));
    vmbsrc->properties.height = g_value_get_int(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "height")));
    vmbsrc->properties.triggerselector = g_value_get_enum(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "triggerselector")));
    vmbsrc->properties.triggermode = g_value_get_enum(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "triggermode")));
    vmbsrc->properties.triggersource = g_value_get_enum(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "triggersource")));
    vmbsrc->properties.triggeractivation = g_value_get_enum(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "triggeractivation")));
    vmbsrc->properties.incomplete_frame_handling = g_value_get_enum(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "incompleteframehandling")));
    vmbsrc->properties.allocation_mode = g_value_get_enum(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "allocationmode")));
    vmbsrc->num_frame_buffers = g_value_get_int(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "framebuffers")));
    vmbsrc->properties.trigger_latency = g_value_get_uint64(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "triggerlatency")));
    vmbsrc->properties.trigger_latency_tolerance = g_value_get_uint64(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "triggerlatencytolerance")));
    vmbsrc->properties.emit_trigger_latency_meta = g_value_get_boolean(
        g_param_spec_get_default_value(
            g_object_class_find_property(
                gobject_class,
                "triggerlatencymeta")));

    // Set up the hardware-trigger correlation state
    g_mutex_init(&vmbsrc->trigger.lock);
    vmbsrc->trigger.capacity = GST_VMBSRC_TRIGGER_RING_CAPACITY;
    vmbsrc->trigger.events = g_new0(GstVmbSrcTriggerEvent, vmbsrc->trigger.capacity);
    vmbsrc->trigger.head = 0;
    vmbsrc->trigger.count = 0;
    vmbsrc->trigger.latency_estimate = 0;
    vmbsrc->trigger.latency_valid = FALSE;
    vmbsrc->trigger.overflow_count = 0;
    vmbsrc->trigger.any_received = FALSE;

    gst_video_info_init(&vmbsrc->video_info);
}

void gst_vmbsrc_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
    GstVmbSrc *vmbsrc = GST_vmbsrc(object);

    GST_DEBUG_OBJECT(vmbsrc, "set_property");

    switch (property_id)
    {
    case PROP_CAMERA_ID:
        if (strcmp(vmbsrc->camera.id, "") != 0)
        {
            free((void *)vmbsrc->camera.id); // Free memory of old entry
        }
        vmbsrc->camera.id = g_value_dup_string(value);
        break;
    case PROP_SETTINGS_FILENAME:
        if (strcmp(vmbsrc->properties.settings_file_path, "") != 0)
        {
            free((void *)vmbsrc->properties.settings_file_path); // Free memory of old entry
        }
        vmbsrc->properties.settings_file_path = g_value_dup_string(value);
        break;
    case PROP_USERSET:
        if (strcmp(vmbsrc->properties.userset, "") != 0)
        {
            free((void *)vmbsrc->properties.userset); // Free memory of old entry
        }
        vmbsrc->properties.userset = g_value_dup_string(value);
        break;
    case PROP_EXPOSURETIME:
        vmbsrc->properties.exposuretime = g_value_get_double(value);
        break;
    case PROP_EXPOSUREAUTO:
        vmbsrc->properties.exposureauto = g_value_get_enum(value);
        break;
    case PROP_BALANCEWHITEAUTO:
        vmbsrc->properties.balancewhiteauto = g_value_get_enum(value);
        break;
    case PROP_GAIN:
        vmbsrc->properties.gain = g_value_get_double(value);
        break;
    case PROP_OFFSETX:
        vmbsrc->properties.offsetx = g_value_get_int(value);
        break;
    case PROP_OFFSETY:
        vmbsrc->properties.offsety = g_value_get_int(value);
        break;
    case PROP_WIDTH:
        vmbsrc->properties.width = g_value_get_int(value);
        break;
    case PROP_HEIGHT:
        vmbsrc->properties.height = g_value_get_int(value);
        break;
    case PROP_TRIGGERSELECTOR:
        vmbsrc->properties.triggerselector = g_value_get_enum(value);
        break;
    case PROP_TRIGGERMODE:
        vmbsrc->properties.triggermode = g_value_get_enum(value);
        break;
    case PROP_TRIGGERSOURCE:
        vmbsrc->properties.triggersource = g_value_get_enum(value);
        break;
    case PROP_TRIGGERACTIVATION:
        vmbsrc->properties.triggeractivation = g_value_get_enum(value);
        break;
    case PROP_INCOMPLETE_FRAME_HANDLING:
        vmbsrc->properties.incomplete_frame_handling = g_value_get_enum(value);
        break;
    case PROP_ALLOCATION_MODE:
        vmbsrc->properties.allocation_mode = g_value_get_enum(value);
        break;
    case PROP_NUM_FRAME_BUFFERS:
        vmbsrc->num_frame_buffers = g_value_get_int(value);
        break;
    case PROP_TRIGGERLATENCY:
        vmbsrc->properties.trigger_latency = g_value_get_uint64(value);
        break;
    case PROP_TRIGGERLATENCYTOLERANCE:
        vmbsrc->properties.trigger_latency_tolerance = g_value_get_uint64(value);
        break;
    case PROP_TRIGGERLATENCYMETA:
        vmbsrc->properties.emit_trigger_latency_meta = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

void gst_vmbsrc_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
    GstVmbSrc *vmbsrc = GST_vmbsrc(object);

    VmbError_t result;

    const char *vmbfeature_value_char;
    double vmbfeature_value_double;
    VmbInt64_t vmbfeature_value_int64;

    GST_TRACE_OBJECT(vmbsrc, "get_property");

    switch (property_id)
    {
    case PROP_CAMERA_ID:
        g_value_set_string(value, vmbsrc->camera.id);
        break;
    case PROP_SETTINGS_FILENAME:
        g_value_set_string(value, vmbsrc->properties.settings_file_path);
        break;
    case PROP_USERSET:
        g_value_set_string(value, vmbsrc->properties.userset);
        break;
    case PROP_EXPOSURETIME:
        // TODO: Workaround for cameras with legacy "ExposureTimeAbs" feature should be replaced with a general legacy
        // feature name handling approach: See similar TODO above

        result = VmbFeatureFloatGet(vmbsrc->camera.handle, "ExposureTime", &vmbfeature_value_double);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"ExposureTime\": %f",
                             vmbfeature_value_double);
            vmbsrc->properties.exposuretime = vmbfeature_value_double;
        }
        else if (result == VmbErrorNotFound)
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to get \"ExposureTime\". Return code was: %s Attempting \"ExposureTimeAbs\"",
                               ErrorCodeToMessage(result));
            result = VmbFeatureFloatGet(vmbsrc->camera.handle, "ExposureTimeAbs", &vmbfeature_value_double);
            if (result == VmbErrorSuccess)
            {
                GST_DEBUG_OBJECT(vmbsrc,
                                 "Camera returned the following value for \"ExposureTimeAbs\": %f",
                                 vmbfeature_value_double);
                vmbsrc->properties.exposuretime = vmbfeature_value_double;
            }
            else
            {
                GST_WARNING_OBJECT(vmbsrc,
                                   "Failed to read value of \"ExposureTimeAbs\" from camera. Return code was: %s",
                                   ErrorCodeToMessage(result));
            }
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to read value of \"ExposureTime\" from camera. Return code was: %s",
                               ErrorCodeToMessage(result));
        }

        g_value_set_double(value, vmbsrc->properties.exposuretime);
        break;
    case PROP_EXPOSUREAUTO:
        result = VmbFeatureEnumGet(vmbsrc->camera.handle, "ExposureAuto", &vmbfeature_value_char);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"ExposureAuto\": %s",
                             vmbfeature_value_char);
            vmbsrc->properties.exposureauto = g_enum_get_value_by_nick(
                                                     g_type_class_ref(GST_ENUM_EXPOSUREAUTO_MODES),
                                                     vmbfeature_value_char)
                                                     ->value;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to read value of \"ExposureAuto\" from camera. Return code was: %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_enum(value, vmbsrc->properties.exposureauto);
        break;
    case PROP_BALANCEWHITEAUTO:
        result = VmbFeatureEnumGet(vmbsrc->camera.handle, "BalanceWhiteAuto", &vmbfeature_value_char);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"BalanceWhiteAuto\": %s",
                             vmbfeature_value_char);
            vmbsrc->properties.balancewhiteauto = g_enum_get_value_by_nick(
                                                         g_type_class_ref(GST_ENUM_BALANCEWHITEAUTO_MODES),
                                                         vmbfeature_value_char)
                                                         ->value;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to read value of \"BalanceWhiteAuto\" from camera. Return code was: %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_enum(value, vmbsrc->properties.balancewhiteauto);
        break;
    case PROP_GAIN:
        result = VmbFeatureFloatGet(vmbsrc->camera.handle, "Gain", &vmbfeature_value_double);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"Gain\": %f",
                             vmbfeature_value_double);
            vmbsrc->properties.gain = vmbfeature_value_double;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to read value of \"Gain\" from camera. Return code was: %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_double(value, vmbsrc->properties.gain);
        break;
    case PROP_OFFSETX:
        result = VmbFeatureIntGet(vmbsrc->camera.handle, "OffsetX", &vmbfeature_value_int64);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"OffsetX\": %lld",
                             vmbfeature_value_int64);
            vmbsrc->properties.offsetx = (int)vmbfeature_value_int64;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Could not read value for \"OffsetX\". Got return code %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_int(value, vmbsrc->properties.offsetx);
        break;
    case PROP_OFFSETY:
        result = VmbFeatureIntGet(vmbsrc->camera.handle, "OffsetY", &vmbfeature_value_int64);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"OffsetY\": %lld",
                             vmbfeature_value_int64);
            vmbsrc->properties.offsety = (int)vmbfeature_value_int64;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Could not read value for \"OffsetY\". Got return code %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_int(value, vmbsrc->properties.offsety);
        break;
    case PROP_WIDTH:
        result = VmbFeatureIntGet(vmbsrc->camera.handle, "Width", &vmbfeature_value_int64);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"Width\": %lld",
                             vmbfeature_value_int64);
            vmbsrc->properties.width = (int)vmbfeature_value_int64;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Could not read value for \"Width\". Got return code %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_int(value, vmbsrc->properties.width);
        break;
    case PROP_HEIGHT:
        result = VmbFeatureIntGet(vmbsrc->camera.handle, "Height", &vmbfeature_value_int64);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"Height\": %lld",
                             vmbfeature_value_int64);
            vmbsrc->properties.height = (int)vmbfeature_value_int64;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Could not read value for \"Height\". Got return code %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_int(value, vmbsrc->properties.height);
        break;
    case PROP_TRIGGERSELECTOR:
        result = VmbFeatureEnumGet(vmbsrc->camera.handle, "TriggerSelector", &vmbfeature_value_char);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"TriggerSelector\": %s",
                             vmbfeature_value_char);
            vmbsrc->properties.triggerselector = g_enum_get_value_by_nick(
                                                     g_type_class_ref(GST_ENUM_TRIGGERSELECTOR_VALUES),
                                                     vmbfeature_value_char)
                                                     ->value;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to read value of \"TriggerSelector\" from camera. Return code was: %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_enum(value, vmbsrc->properties.triggerselector);
        break;
    case PROP_TRIGGERMODE:
        result = VmbFeatureEnumGet(vmbsrc->camera.handle, "TriggerMode", &vmbfeature_value_char);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"TriggerMode\": %s",
                             vmbfeature_value_char);
            vmbsrc->properties.triggermode = g_enum_get_value_by_nick(
                                                     g_type_class_ref(GST_ENUM_TRIGGERMODE_VALUES),
                                                     vmbfeature_value_char)
                                                     ->value;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to read value of \"TriggerMode\" from camera. Return code was: %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_enum(value, vmbsrc->properties.triggermode);
        break;
    case PROP_TRIGGERSOURCE:
        result = VmbFeatureEnumGet(vmbsrc->camera.handle, "TriggerSource", &vmbfeature_value_char);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"TriggerSource\": %s",
                             vmbfeature_value_char);
            vmbsrc->properties.triggersource = g_enum_get_value_by_nick(
                                                     g_type_class_ref(GST_ENUM_TRIGGERSOURCE_VALUES),
                                                     vmbfeature_value_char)
                                                     ->value;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to read value of \"TriggerSource\" from camera. Return code was: %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_enum(value, vmbsrc->properties.triggersource);
        break;
    case PROP_TRIGGERACTIVATION:
        result = VmbFeatureEnumGet(vmbsrc->camera.handle, "TriggerActivation", &vmbfeature_value_char);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "Camera returned the following value for \"TriggerActivation\": %s",
                             vmbfeature_value_char);
            vmbsrc->properties.triggeractivation = g_enum_get_value_by_nick(
                                                     g_type_class_ref(GST_ENUM_TRIGGERACTIVATION_VALUES),
                                                     vmbfeature_value_char)
                                                     ->value;
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to read value of \"TriggerActivation\" from camera. Return code was: %s",
                               ErrorCodeToMessage(result));
        }
        g_value_set_enum(value, vmbsrc->properties.triggeractivation);
        break;
    case PROP_INCOMPLETE_FRAME_HANDLING:
        g_value_set_enum(value, vmbsrc->properties.incomplete_frame_handling);
        break;
    case PROP_ALLOCATION_MODE:
        g_value_set_enum(value, vmbsrc->properties.allocation_mode);
        break;
    case PROP_NUM_FRAME_BUFFERS:
        g_value_set_int(value, vmbsrc->num_frame_buffers);
        break;
    case PROP_TRIGGERLATENCY:
        g_value_set_uint64(value, vmbsrc->properties.trigger_latency);
        break;
    case PROP_TRIGGERLATENCYTOLERANCE:
        g_value_set_uint64(value, vmbsrc->properties.trigger_latency_tolerance);
        break;
    case PROP_TRIGGERLATENCYMETA:
        g_value_set_boolean(value, vmbsrc->properties.emit_trigger_latency_meta);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

void gst_vmbsrc_finalize(GObject *object)
{
    GstVmbSrc *vmbsrc = GST_vmbsrc(object);

    GST_TRACE_OBJECT(vmbsrc, "finalize");

    if (vmbsrc->camera.is_connected)
    {
        VmbError_t result = VmbCameraClose(vmbsrc->camera.handle);
        if (result == VmbErrorSuccess)
        {
            GST_INFO_OBJECT(vmbsrc, "Closed camera %s", vmbsrc->camera.id);
        }
        else
        {
            GST_ERROR_OBJECT(vmbsrc,
                             "Closing camera %s failed. Got error code: %s",
                             vmbsrc->camera.id,
                             ErrorCodeToMessage(result));
        }
        vmbsrc->camera.is_connected = false;
    }

    G_LOCK(vmb_open_count);
    if (0 == --vmb_open_count)
    {
        VmbShutdown();
        GST_INFO_OBJECT(vmbsrc, "VimbaX API was shut down");
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "VmbShutdown not called. Current open count: %u", vmb_open_count);
    }
    G_UNLOCK(vmb_open_count);

    g_free(vmbsrc->trigger.events);
    vmbsrc->trigger.events = NULL;
    g_mutex_clear(&vmbsrc->trigger.lock);

    G_OBJECT_CLASS(gst_vmbsrc_parent_class)->finalize(object);
}

/* get caps from subclass */
static GstCaps *gst_vmbsrc_get_caps(GstBaseSrc *src, GstCaps *filter)
{
    UNUSED(filter); // enable compilation while treating warning of unused vairable as error
    GstVmbSrc *vmbsrc = GST_vmbsrc(src);

    GST_TRACE_OBJECT(vmbsrc, "get_caps");

    GstCaps *caps;
    caps = gst_pad_get_pad_template_caps(GST_BASE_SRC_PAD(src));
    caps = gst_caps_make_writable(caps);

    // Query the capabilities from the camera and return sensible values. If no camera is connected the template caps
    // are returned
    if (vmbsrc->camera.is_connected)
    {
        VmbInt64_t vmb_width, vmb_height;

        VmbFeatureIntGet(vmbsrc->camera.handle, "Width", &vmb_width);
        VmbFeatureIntGet(vmbsrc->camera.handle, "Height", &vmb_height);

        GValue width = G_VALUE_INIT;
        GValue height = G_VALUE_INIT;

        g_value_init(&width, G_TYPE_INT);
        g_value_init(&height, G_TYPE_INT);

        g_value_set_int(&width, (gint)vmb_width);

        g_value_set_int(&height, (gint)vmb_height);

        GstStructure *raw_caps = gst_caps_get_structure(caps, 0);
        GstStructure *bayer_caps = gst_caps_get_structure(caps, 1);

        gst_structure_set_value(raw_caps, "width", &width);
        gst_structure_set_value(raw_caps, "height", &height);
        gst_structure_set(raw_caps,
                          // TODO: Check if framerate should also be gotten from camera (e.g. as max-framerate here)
                          // Mark the framerate as variable because triggering might cause variable framerate
                          "framerate", GST_TYPE_FRACTION, 0, 1,
                          NULL);

        gst_structure_set_value(bayer_caps, "width", &width);
        gst_structure_set_value(bayer_caps, "height", &height);
        gst_structure_set(bayer_caps,
                          // TODO: Check if framerate should also be gotten from camera (e.g. as max-framerate here)
                          // Mark the framerate as variable because triggering might cause variable framerate
                          "framerate", GST_TYPE_FRACTION, 0, 1,
                          NULL);
#if HAVE_NVMM
        GstStructure *nvmm_raw_caps = gst_caps_get_structure(caps, 2);

        gst_structure_set_value(nvmm_raw_caps, "width", &width);
        gst_structure_set_value(nvmm_raw_caps, "height", &height);
        gst_structure_set(nvmm_raw_caps,
                          // TODO: Check if framerate should also be gotten from camera (e.g. as max-framerate here)
                          // Mark the framerate as variable because triggering might cause variable framerate
                          "framerate", GST_TYPE_FRACTION, 0, 1,
                          NULL);
#endif 

        // Query supported pixel formats from camera and map them to GStreamer formats
        GValue pixel_format_raw_list = G_VALUE_INIT;
        g_value_init(&pixel_format_raw_list, GST_TYPE_LIST);

        GValue pixel_format_bayer_list = G_VALUE_INIT;
        g_value_init(&pixel_format_bayer_list, GST_TYPE_LIST);

        GValue pixel_format = G_VALUE_INIT;
        g_value_init(&pixel_format, G_TYPE_STRING);

        // Add all supported GStreamer format string to the reported caps
        for (unsigned int i = 0; i < vmbsrc->camera.supported_formats_count; i++)
        {
            g_value_set_static_string(&pixel_format, vmbsrc->camera.supported_formats[i]->gst_format_name);
            // TODO: Should this perhaps be done via a flag in vimbax_gst_format_matches?
            if (starts_with(vmbsrc->camera.supported_formats[i]->vimbax_format_name, "Bayer"))
            {
                gst_value_list_append_value(&pixel_format_bayer_list, &pixel_format);
            }
            else
            {
                gst_value_list_append_value(&pixel_format_raw_list, &pixel_format);
            }
        }
        gst_structure_set_value(raw_caps, "format", &pixel_format_raw_list);
        gst_structure_set_value(bayer_caps, "format", &pixel_format_bayer_list);
    }

    GST_DEBUG_OBJECT(vmbsrc, "returning caps: %s", gst_caps_to_string(caps));

    return caps;
}

/* notify the subclass of new caps */
static gboolean gst_vmbsrc_set_caps(GstBaseSrc *src, GstCaps *caps)
{
    GstVmbSrc *vmbsrc = GST_vmbsrc(src);
    GstCapsFeatures *feat;

    GST_TRACE_OBJECT(vmbsrc, "set_caps");

    GST_DEBUG_OBJECT(vmbsrc, "caps requested to be set: %s", gst_caps_to_string(caps));

    // TODO: save to assume that "format" is always exactly one format and not a list? gst_caps_is_fixed might otherwise
    // be a good check and gst_caps_normalize could help make sure of it
    GstStructure *structure;
    structure = gst_caps_get_structure(caps, 0);
    const char *gst_format = gst_structure_get_string(structure, "format");
    GST_DEBUG_OBJECT(vmbsrc,
                     "Looking for matching VimbaX pixel format to GSreamer format \"%s\"",
                     gst_format);

    const char *vimbax_format = NULL;
    for (unsigned int i = 0; i < vmbsrc->camera.supported_formats_count; i++)
    {
        if (strcmp(gst_format, vmbsrc->camera.supported_formats[i]->gst_format_name) == 0)
        {
            vimbax_format = vmbsrc->camera.supported_formats[i]->vimbax_format_name;
            GST_DEBUG_OBJECT(vmbsrc, "Found matching VimbaX pixel format \"%s\"", vimbax_format);
            break;
        }
    }
    if (vimbax_format == NULL)
    {
        GST_ERROR_OBJECT(vmbsrc,
                         "Could not find a matching VimbaX pixel format for GStreamer format \"%s\"",
                         gst_format);
        return FALSE;
    }

    // Apply the requested caps to appropriate camera settings
    VmbError_t result;
    // Changing the pixel format can not be done while images are acquired
    result = stop_image_acquisition(vmbsrc);

    if(vmbsrc->filled_frame_queue != NULL)
    {
        // current possibly non-empty frame queue layout is no longer valid so we use a new one
        g_async_queue_unref(vmbsrc->filled_frame_queue); // delete current queue
        vmbsrc->filled_frame_queue = g_async_queue_new(); // create new queue for our frames
    }

    result = VmbFeatureEnumSet(vmbsrc->camera.handle,
                               "PixelFormat",
                               vimbax_format);
    if (result != VmbErrorSuccess)
    {
        GST_ERROR_OBJECT(vmbsrc,
                         "Could not set \"PixelFormat\" to \"%s\". Got return code \"%s\"",
                         vimbax_format,
                         ErrorCodeToMessage(result));
        return FALSE;
    }


    feat = gst_caps_get_features(caps, 0);
    if (gst_caps_features_contains(feat, GST_CAPS_FEATURE_MEMORY_NVMM))
    {
        vmbsrc->use_nvmm = true;
    }
    
    if (vmbsrc->use_nvmm && vmbsrc->properties.allocation_mode == GST_VMBSRC_ALLOCATION_MODE_ALLOC_AND_ANNOUNCE_FRAME)
    {
        GST_ERROR_OBJECT(vmbsrc, "NVMM incompatible with alloc and announce allocation mode");
        return FALSE;
    }

    // TODO: Check if vmbsrc->video_info can be directly initialized here
    GstVideoInfo video_info = { 0 };
    if (!gst_video_info_from_caps(&video_info, caps))
    {
        return FALSE;
    }


    // width and height are always the value that is already written on the camera because get_caps only reports that
    // value. Setting it here is not necessary as the feature values are controlled via properties of the element.

    // Buffer size needs to be increased if the new payload size is greater than the old one because that means the
    // previously allocated buffers are not large enough. We simply check the size of the first buffer because they were
    // all allocated with the same size
    VmbUint32_t new_payload_size;
    result = VmbPayloadSizeGet(vmbsrc->camera.handle, &new_payload_size);
    if (vmbsrc->frame_buffers[0].bufferSize < new_payload_size || result != VmbErrorSuccess)
    {
        // Also reallocate buffers if PayloadSize could not be read because it might have increased
        GST_DEBUG_OBJECT(vmbsrc,
                         "PayloadSize increased or has not been set yet. Reallocating frame buffers to ensure enough space");
        revoke_and_free_buffers(vmbsrc);
        result = alloc_and_announce_buffers(vmbsrc, &video_info);
    }
    if (result == VmbErrorSuccess)
    {
        result = start_image_acquisition(vmbsrc);
    }

    if (result != VmbErrorSuccess)
    {
        return FALSE;
    }

    vmbsrc->video_info = video_info;

    return TRUE;
}

/* start and stop processing, ideal for opening/closing the resource */
static gboolean gst_vmbsrc_start(GstBaseSrc *src)
{
    GstVmbSrc *vmbsrc = GST_vmbsrc(src);

    GST_TRACE_OBJECT(vmbsrc, "start");

    // Prepare queue for filled frames from which vmbsrc_create can take them
    vmbsrc->filled_frame_queue = g_async_queue_new();

    // Reset hardware-trigger correlation state for this acquisition run
    g_mutex_lock(&vmbsrc->trigger.lock);
    vmbsrc->trigger.head = 0;
    vmbsrc->trigger.count = 0;
    vmbsrc->trigger.latency_estimate = 0;
    vmbsrc->trigger.latency_valid = FALSE;
    vmbsrc->trigger.overflow_count = 0;
    vmbsrc->trigger.any_received = FALSE;
    g_mutex_unlock(&vmbsrc->trigger.lock);

    if (vmbsrc->frame_buffers == NULL)
    {
        vmbsrc->frame_buffers = calloc(vmbsrc->num_frame_buffers, sizeof *vmbsrc->frame_buffers);
    }

    vmbsrc->use_nvmm = false;

    VmbError_t result;

    // TODO: Error handling
    if (!vmbsrc->camera.is_connected)
    {
        result = open_camera_connection(vmbsrc);
        if (result != VmbErrorSuccess)
        {
            // Can't connect to camera. Abort execution by returning FALSE. This stops the pipeline!
            return FALSE;
        }
    }

    // Load settings from given file if a path was given (settings_file_path is not empty)
    if (strcmp(vmbsrc->properties.settings_file_path, "") != 0)
    {
        GST_WARNING_OBJECT(vmbsrc,
                           "\"%s\" was given as settingsfile. Other feature settings passed as element properties will be ignored!",
                           vmbsrc->properties.settings_file_path);

        VmbFilePathChar_t *buffer;
#ifdef _WIN32
        size_t num_char = strlen(vmbsrc->properties.settings_file_path);
        size_t num_wchar = 0;
        mbstowcs_s(&num_wchar, NULL, 0, vmbsrc->properties.settings_file_path, num_char);
        buffer = calloc(num_wchar, sizeof(VmbFilePathChar_t));
        mbstowcs_s(NULL, buffer, num_wchar, vmbsrc->properties.settings_file_path, num_char);
#else
        buffer = vmbsrc->properties.settings_file_path;
#endif
        result = VmbSettingsLoad(vmbsrc->camera.handle,
                                 buffer,
                                 NULL,
                                 sizeof(VmbFeaturePersistSettings_t));
#ifdef _WIN32
        free(buffer);
#endif
        if (result != VmbErrorSuccess)
        {
            GST_ERROR_OBJECT(vmbsrc,
                             "Could not load settings from file \"%s\". Got error code %s",
                             vmbsrc->properties.settings_file_path,
                             ErrorCodeToMessage(result));
        }
    }
    else if (strcmp(vmbsrc->properties.userset, "") != 0)
    {
        // Feature settings passed as element properties default to values (e.g. 0 for "gain") that
        // apply_feature_settings() would unconditionally write to the camera below, overwriting what
        // the requested user set just loaded. So, same as with settingsfile above, loading a user set
        // and setting individual feature properties are mutually exclusive.
        GST_WARNING_OBJECT(vmbsrc,
                           "\"%s\" was given as userset. Other feature settings passed as element properties will be ignored!",
                           vmbsrc->properties.userset);
        result = load_user_set(vmbsrc);
    }
    else
    {
        // If neither a settings file nor a user set is given, apply the passed properties as feature
        // settings instead
        GST_DEBUG_OBJECT(vmbsrc, "No settings file or userset given. Applying features from element properties instead");
        result = apply_feature_settings(vmbsrc);
    }

    // Is this necessary?
    if (result == VmbErrorSuccess)
    {
        gst_base_src_start_complete(src, GST_FLOW_OK);
    }
    else
    {
        GST_ERROR_OBJECT(vmbsrc, "Could not start acquisition. Experienced error: %s", ErrorCodeToMessage(result));
        gst_base_src_start_complete(src, GST_FLOW_ERROR);
    }

    // TODO: Is this enough error handling?
    return result == VmbErrorSuccess ? TRUE : FALSE;
}

static gboolean gst_vmbsrc_stop(GstBaseSrc *src)
{
    GstVmbSrc *vmbsrc = GST_vmbsrc(src);

    GST_TRACE_OBJECT(vmbsrc, "stop");

    stop_image_acquisition(vmbsrc);

    revoke_and_free_buffers(vmbsrc);

    free(vmbsrc->frame_buffers);
    vmbsrc->frame_buffers = NULL;

    // Unref the filled frame queue so it is deleted properly
    g_async_queue_unref(vmbsrc->filled_frame_queue);

    return TRUE;
}

static GstBuffer* gst_vmbsrc_frame_to_buffer(GstVmbSrc *vmbsrc, VmbFrame_t *frame)
{
    size_t imageSize = calculateImageBufferSize(frame->width, frame->height, frame->pixelFormat);
#if HAVE_NVMM
    if (vmbsrc->use_nvmm)
    {
        NvBufSurface *surf = frame->context[2];

        surf->surfaceList[0].planeParams.width[0] = frame->width;
        surf->surfaceList[0].planeParams.height[0] = frame->height;

        return gst_buffer_new_wrapped_full(
            0, 
            surf, 
            sizeof(*surf),
            0,
            sizeof(*surf),
            frame,
            &glib_destroy_callback );
    }
#else
    UNUSED(vmbsrc);
#endif

    return gst_buffer_new_wrapped_full(
        0, /* TODO: Any flags needed here instead of just 0? */
        frame->imageData,
        imageSize,
        0,
        imageSize,
        frame,
        &glib_destroy_callback );
}

#if HAVE_DEEPSTREAM
// Payload carried into DeepStream. nvstreammux drops plain custom GstMeta (which is why the
// GstVmbSrcTriggerMeta needs a bridging probe), but it *does* transform an NvDsMeta attached
// upstream into an NvDsUserMeta on the matching NvDsFrameMeta - so a DeepStream consumer reads
// these fields straight off the frame, correctly paired per source, without any probe. See the
// "Bridging the meta into DeepStream frame metadata" section of EXAMPLES.md.
// Mirrors the fields of GstVmbSrcTriggerMeta so a DeepStream consumer sees the same record a
// pure-GStreamer consumer does.
typedef struct
{
    guint64 trigger_seq;       // sequence number supplied by the external trigger
    guint64 trigger_time;      // raw CLOCK_MONOTONIC trigger instant (ns), or GST_CLOCK_TIME_NONE
    guint64 approx_latency;    // arrival-minus-trigger latency (ns), or GST_CLOCK_TIME_NONE
    guint64 camera_frame_id;   // VmbFrame_t.frameID
    guint64 camera_timestamp;  // VmbFrame_t.timestamp (raw camera clock ticks)
    gboolean correlated;       // TRUE if a matching trigger was found
} GstVmbSrcNvDsTriggerMeta;

// nvds_get_user_meta_type() maps a string to a process-stable user-meta type id. A DeepStream
// consumer computes the same id from the same string to identify the frame's NvDsUserMeta.
#define GST_VMBSRC_NVDS_TRIGGER_META_NAME "VMBSRC.TRIGGER.USERMETA"

static guint gst_vmbsrc_nvds_trigger_meta_type(void)
{
    static gsize once = 0;
    static guint meta_type = 0;
    if (g_once_init_enter(&once))
    {
        meta_type = nvds_get_user_meta_type((gchar *)GST_VMBSRC_NVDS_TRIGGER_META_NAME);
        g_once_init_leave(&once, 1);
    }
    return meta_type;
}

// Copy/release of the payload while it rides on the source buffer (GstMeta level).
static gpointer gst_vmbsrc_nvds_meta_copy(gpointer data, gpointer user_data)
{
    (void)user_data;
    GstVmbSrcNvDsTriggerMeta *copy = g_new(GstVmbSrcNvDsTriggerMeta, 1);
    *copy = *(GstVmbSrcNvDsTriggerMeta *)data;
    return copy;
}

static void gst_vmbsrc_nvds_meta_release(gpointer data, gpointer user_data)
{
    (void)user_data;
    g_free(data);
}

// Called by nvstreammux to transform the buffer-level payload into the frame's NvDsUserMeta:
// "data" is the NvDsUserMeta, its user_meta_data is our buffer-level payload. Return a fresh heap
// copy that the frame meta takes ownership of (freed via the release func below).
static gpointer gst_vmbsrc_nvds_meta_transform(gpointer data, gpointer user_data)
{
    (void)user_data;
    NvDsUserMeta *user_meta = (NvDsUserMeta *)data;
    GstVmbSrcNvDsTriggerMeta *copy = g_new(GstVmbSrcNvDsTriggerMeta, 1);
    *copy = *(GstVmbSrcNvDsTriggerMeta *)user_meta->user_meta_data;
    return copy;
}

static void gst_vmbsrc_nvds_meta_transform_release(gpointer data, gpointer user_data)
{
    (void)user_data;
    NvDsUserMeta *user_meta = (NvDsUserMeta *)data;
    if (user_meta != NULL && user_meta->user_meta_data != NULL)
    {
        g_free(user_meta->user_meta_data);
        user_meta->user_meta_data = NULL;
    }
}
#endif // HAVE_DEEPSTREAM

/* ask the subclass to create a buffer */
static GstFlowReturn gst_vmbsrc_create(GstPushSrc *src, GstBuffer **buf)
{
    GstVmbSrc *vmbsrc = GST_vmbsrc(src);

    GST_TRACE_OBJECT(vmbsrc, "create");

    bool submit_frame = false;
    VmbFrame_t *frame;
    do
    {
        // Wait until we can get a filled frame (added to queue in vimbax_frame_callback)
        frame = NULL;
        GstStateChangeReturn ret;
        GstState state;
        do
        {
            // Try to get a filled frame for 10 microseconds
            frame = g_async_queue_timeout_pop(vmbsrc->filled_frame_queue, 10);
            // Get the current state of the element. Should return immediately since we are not doing ASYNC state changes
            // but wait at most for 100 nanoseconds
            ret = gst_element_get_state(GST_ELEMENT(vmbsrc), &state, NULL, 100); // timeout is given in nanoseconds
            UNUSED(ret);
            if (state != GST_STATE_PLAYING)
            {
                // The src should not create any more data. Stop waiting for frame and do not fill buf
                GST_INFO_OBJECT(vmbsrc, "Element state is no longer \"GST_STATE_PLAYING\". Aborting create call.");
                return GST_FLOW_FLUSHING;
            }
        } while (frame == NULL);
        // We got a frame. Check receive status and handle incomplete frames according to
        // vmbsrc->properties.incomplete_frame_handling
        if (frame->receiveStatus == VmbFrameStatusIncomplete)
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Received frame with ID \"%llu\" was incomplete", frame->frameID);
            if (vmbsrc->properties.incomplete_frame_handling == GST_VMBSRC_INCOMPLETE_FRAME_HANDLING_SUBMIT)
            {
                GST_DEBUG_OBJECT(vmbsrc,
                                 "Submitting incomplete frame because \"incompleteframehandling\" requested it");
                submit_frame = true;
            }
            else
            {
                // frame should be dropped -> requeue VimbaX buffer here since image data will not be used
                GST_DEBUG_OBJECT(vmbsrc, "Dropping incomplete frame and requeueing buffer to capture queue");
                VmbCaptureFrameQueue(vmbsrc->camera.handle, frame, &vimbax_frame_callback);
            }
        }
        else
        {
            GST_TRACE_OBJECT(vmbsrc, "frame was complete");
            submit_frame = true;
        }
    } while (!submit_frame);

    // Create GstBuffer in such a way that the registered callback is called once the buffer is no
    // longer used by the pipeline. In the callback we can requeue the frame for further transfers
    // from the camera.
    GstBuffer* buffer = gst_vmbsrc_frame_to_buffer(vmbsrc, frame);

    // Arrival time recorded in vimbax_frame_callback (CLOCK_MONOTONIC nanoseconds). Using this,
    // NOT the time create() dequeues the frame, avoids hiding the time a frame spent waiting in
    // filled_frame_queue (which would corrupt latency analysis).
    gint64 arrival_monotonic_us = (gint64)(guintptr)frame->context[3];
    GstClockTime arrival_monotonic_ns =
        (arrival_monotonic_us > 0) ? ((GstClockTime)arrival_monotonic_us * GST_USECOND) : GST_CLOCK_TIME_NONE;

    // Try to correlate this frame with an external hardware-trigger event (fed via notify-trigger).
    guint64 trigger_seq = 0;
    GstClockTime trigger_monotonic_ns = GST_CLOCK_TIME_NONE;
    GstClockTime approx_latency_ns = GST_CLOCK_TIME_NONE;
    gboolean correlated = FALSE;
    if (arrival_monotonic_ns != GST_CLOCK_TIME_NONE)
    {
        correlated = gst_vmbsrc_correlate_trigger(vmbsrc, arrival_monotonic_ns,
                                                  &trigger_seq, &trigger_monotonic_ns, &approx_latency_ns);
    }

    // Timestamp the buffer with the instant its originating event happened: the correlated trigger
    // time when available (the most meaningful anchor for latency analysis and for pairing the
    // image with external data), otherwise the frame arrival time. In both cases we map a past
    // CLOCK_MONOTONIC instant into pipeline running-time using only the elapsed monotonic delta,
    // which stays correct for whatever clock the pipeline selected as long as it advances in real
    // time: running_time(t) = now_running_time - (now_monotonic - t).
    GstClock *clock = gst_element_get_clock(GST_ELEMENT(vmbsrc));
    GstClockTime pts = GST_CLOCK_TIME_NONE;
    GstClockTime trigger_running_time = GST_CLOCK_TIME_NONE;
    if (clock)
    {
        GstClockTime now = gst_clock_get_time(clock);
        GstClockTime now_monotonic_ns = (GstClockTime)g_get_monotonic_time()*1000;
        GstClockTime base_time = gst_element_get_base_time(GST_ELEMENT(vmbsrc));
        GstClockTime now_running_time = (now >= base_time) ? (now - base_time) : 0;

        // The trigger/arrival instants are CLOCK_MONOTONIC; the buffer PTS must be pipeline
        // running-time. GStreamer's default clock (GstSystemClock, GST_CLOCK_TYPE_MONOTONIC) IS
        // g_get_monotonic_time(), so its epoch matches those instants and the two reads of "now"
        // above measure the same physical clock - making the delta bridge below redundant and,
        // worse, saddling the result with the microsecond resolution of g_get_monotonic_time().
        // Detect that: compare the two "now" readings, and if they agree within 2.5 us (a little
        // above that microsecond quantisation plus the gap between the two reads) treat the clocks
        // as identical and convert exactly with a single subtraction of base_time. Otherwise the
        // pipeline runs a foreign clock (e.g. slaved to PTP/network); keep bridging the domains via
        // the elapsed monotonic delta measured now, which stays correct for any clock advancing in
        // real time.
        GstClockTimeDiff clock_offset = (GstClockTimeDiff)(now - now_monotonic_ns);
        gboolean clocks_aligned =
            (clock_offset < 0 ? -clock_offset : clock_offset) < (GstClockTimeDiff)(5 * GST_USECOND / 2);

        if (correlated && trigger_monotonic_ns != GST_CLOCK_TIME_NONE)
        {
            if (clocks_aligned)
            {
                trigger_running_time =
                    (trigger_monotonic_ns > base_time) ? (trigger_monotonic_ns - base_time) : 0;
            }
            else
            {
                GstClockTimeDiff since = (GstClockTimeDiff)(now_monotonic_ns - trigger_monotonic_ns);
                if (since < 0)
                {
                    since = 0;
                }
                trigger_running_time = (now_running_time > (GstClockTime)since) ? (now_running_time - since) : 0;
            }
            pts = trigger_running_time;
        }
        else if (arrival_monotonic_ns != GST_CLOCK_TIME_NONE)
        {
            if (clocks_aligned)
            {
                pts = (arrival_monotonic_ns > base_time) ? (arrival_monotonic_ns - base_time) : 0;
            }
            else
            {
                GstClockTimeDiff age = (GstClockTimeDiff)(now_monotonic_ns - arrival_monotonic_ns);
                if (age < 0)
                {
                    age = 0;
                }
                pts = (now_running_time > (GstClockTime)age) ? (now_running_time - age) : 0;
            }
        }
        else
        {
            // Neither trigger nor arrival time available; fall back to the current running-time
            pts = now_running_time;
        }

        // Anchor DeepStream latency measurement at the trigger instant, falling back to the frame
        // arrival instant for frames without a correlated trigger (in particular when notify-trigger
        // is not used at all - the meta must still be emitted per frame, otherwise DeepStream's
        // frame counter freezes and the frame-latency baseline stays 0). The new nvstreammux reads
        // component in/out timestamps (wall-clock milliseconds) from a GstReferenceTimestampMeta's
        // caps structure; convert the anchor instant into that domain via the monotonic->realtime
        // delta measured now. NOTE: units/behaviour follow the new nvstreammux (gst-nvmultistream2)
        // and should be validated with NVDS_ENABLE_LATENCY_MEASUREMENT for the mux in use.
        // NOTE: nvds_measure_buffer_latency() only uses a component as the overall frame-latency
        // baseline (comp_in_timestamp) if its name starts with "nvv4l2decode" or "audiodecoder"
        // (hardcoded strncmp in libnvdsgst_meta.so). To anchor DeepStream's "Frame latency" at the
        // trigger instant, name this element accordingly, e.g. "nvv4l2decoder_cam_<ID>"; the meta
        // below is then emitted as "<element-name>-trigger" and passes that check. This block only
        // uses core GStreamer API, so it needs no DeepStream SDK at build time and is harmless in
        // non-DeepStream pipelines.
        GstClockTime meta_anchor_monotonic_ns = GST_CLOCK_TIME_NONE;
        GstClockTime meta_anchor_running_time = GST_CLOCK_TIME_NONE;
        if (correlated && trigger_running_time != GST_CLOCK_TIME_NONE)
        {
            meta_anchor_monotonic_ns = trigger_monotonic_ns;
            meta_anchor_running_time = trigger_running_time;
        }
        else if (arrival_monotonic_ns != GST_CLOCK_TIME_NONE)
        {
            meta_anchor_monotonic_ns = arrival_monotonic_ns;
            meta_anchor_running_time = pts;
        }
        if (vmbsrc->properties.emit_trigger_latency_meta &&
            meta_anchor_monotonic_ns != GST_CLOCK_TIME_NONE)
        {
            // Choose the frame_num identity carried into DeepStream. Unlike the latency *anchor*
            // above (which always uses the best timing instant available per frame), frame_num sticks
            // to a single value space for the whole acquisition run so downstream logs stay
            // unambiguous - a given frame_num means the same kind of thing every frame:
            //   - Before any trigger has ever been seen, triggering may not be in use at all and no
            //     trigger sequence exists; use the monotonic delivered-frame counter, a collision-
            //     free per-frame index.
            //   - Once any trigger has arrived (latched in trigger.any_received), commit to the
            //     trigger-sequence space for the rest of the run and never fall back. trigger_seq is
            //     the cross-camera frame identity: cameras fired by the same external trigger report
            //     the same frame_num, which is what lets two streams be paired by trigger event. A
            //     frame that then fails to correlate is reported as 0 - a deliberate, obvious "this
            //     should not happen" break in the logs, rather than a delivered-counter value that
            //     would silently collide with the trigger-sequence space and read as a valid frame.
            gboolean triggers_active;
            g_mutex_lock(&vmbsrc->trigger.lock);
            triggers_active = vmbsrc->trigger.any_received;
            g_mutex_unlock(&vmbsrc->trigger.lock);
            guint64 meta_frame_num =
                triggers_active ? (correlated ? trigger_seq : 0) : vmbsrc->num_frames_pushed;

            gdouble now_realtime_ms = (gdouble)g_get_real_time() / 1000.0;
            gdouble anchor_realtime_ms =
                now_realtime_ms - (gdouble)(now_monotonic_ns - meta_anchor_monotonic_ns) / (gdouble)GST_MSECOND;
            gchar *component_name = g_strdup_printf("%s-trigger", GST_ELEMENT_NAME(vmbsrc));
            GstCaps *reference = gst_caps_new_simple(
                "timestamp/x-vmbsrc-trigger",
                "component_name", G_TYPE_STRING, component_name,
                "frame_num", G_TYPE_INT, (gint)meta_frame_num,
                "in_timestamp", G_TYPE_DOUBLE, anchor_realtime_ms,
                "out_timestamp", G_TYPE_DOUBLE, now_realtime_ms,
                NULL);
            gst_buffer_add_reference_timestamp_meta(buffer, reference, meta_anchor_running_time, GST_CLOCK_TIME_NONE);
            gst_caps_unref(reference);
            g_free(component_name);
        }
        g_object_unref(clock);
    }
    GST_BUFFER_PTS(buffer) = pts;
    // Framerate is advertised as variable (0/1) because triggering may produce a variable frame
    // rate, so no meaningful constant buffer duration can be provided. DTS is left unset as is
    // conventional for raw video.
    GST_BUFFER_DURATION(buffer) = GST_CLOCK_TIME_NONE;

    // Manually calculate the stride for pixel rows as it might not be identical to GStreamer
    // expectations
    gint stride[GST_VIDEO_MAX_PLANES] = {0};
    gint num_planes = vmbsrc->video_info.finfo->n_planes;

    for (gint i = 0; i < num_planes; ++i)
    {
        stride[i] = vmbsrc->video_info.width * vmbsrc->video_info.finfo->pixel_stride[i];
    }

    gst_buffer_add_video_meta_full(buffer,
                                   GST_VIDEO_FRAME_FLAG_NONE,
                                   vmbsrc->video_info.finfo->format,
                                   vmbsrc->video_info.width,
                                   vmbsrc->video_info.height,
                                   num_planes,
                                   vmbsrc->video_info.offset,
                                   stride);

    GST_BUFFER_OFFSET(buffer) = vmbsrc->num_frames_pushed;
    GST_BUFFER_OFFSET_END(buffer) = ++(vmbsrc->num_frames_pushed);

    // Attach the vmbsrc trigger metadata to every buffer so downstream (pure GStreamer, or a
    // DeepStream bridge probe - see EXAMPLES.md) can pair the image with the right external data.
    // The external trigger sequence number IS the frame identity that stays aligned across dropped
    // frames/triggers (unlike a delivered-frame counter such as GST_BUFFER_OFFSET or
    // NvDsFrameMeta.frame_num).
    GstCustomMeta *trigger_meta = gst_buffer_add_custom_meta(buffer, GST_VMBSRC_TRIGGER_META_NAME);
    if (trigger_meta != NULL)
    {
        // Store the fields in the custom meta's GstStructure. GstClockTime is guint64, so
        // GST_CLOCK_TIME_NONE round-trips as a plain uint64; keys match the accessor above.
        gst_structure_set(gst_custom_meta_get_structure(trigger_meta),
                          "trigger-seq", G_TYPE_UINT64, (guint64)trigger_seq,
                          // Propagate the raw CLOCK_MONOTONIC trigger instant exactly as fed into
                          // notify-trigger (this custom meta is our own payload, opaque to
                          // GStreamer; the running-time conversion lives in the buffer PTS instead).
                          "trigger-time", G_TYPE_UINT64,
                          (guint64)(correlated ? trigger_monotonic_ns : GST_CLOCK_TIME_NONE),
                          "approx-latency", G_TYPE_UINT64, (guint64)approx_latency_ns,
                          "camera-frame-id", G_TYPE_UINT64, (guint64)frame->frameID,
                          "camera-timestamp", G_TYPE_UINT64, (guint64)frame->timestamp,
                          "correlated", G_TYPE_BOOLEAN, correlated,
                          NULL);
    }

#if HAVE_DEEPSTREAM
    // Additionally attach the record as a DeepStream NvDsMeta. Unlike the custom GstMeta above
    // (dropped by nvstreammux), this is transformed by nvstreammux into an NvDsUserMeta on the
    // matching NvDsFrameMeta, so a DeepStream consumer reads it off the frame with no bridging
    // probe (see EXAMPLES.md). Compiled in only when the DeepStream SDK was found at build time.
    {
        GstVmbSrcNvDsTriggerMeta *payload = g_new(GstVmbSrcNvDsTriggerMeta, 1);
        payload->trigger_seq = (guint64)trigger_seq;
        payload->trigger_time = (guint64)(correlated ? trigger_monotonic_ns : GST_CLOCK_TIME_NONE);
        payload->approx_latency = (guint64)approx_latency_ns;
        payload->camera_frame_id = (guint64)frame->frameID;
        payload->camera_timestamp = (guint64)frame->timestamp;
        payload->correlated = correlated;
        NvDsMeta *nvds_meta = gst_buffer_add_nvds_meta(buffer, payload, NULL,
                                                       gst_vmbsrc_nvds_meta_copy,
                                                       gst_vmbsrc_nvds_meta_release);
        if (nvds_meta != NULL)
        {
            // nvstreammux copies meta_data to NvDsFrameMeta user meta via the transform func, and
            // tags the resulting NvDsUserMeta->base_meta.meta_type with this meta_type.
            nvds_meta->meta_type = (gint)gst_vmbsrc_nvds_trigger_meta_type();
            nvds_meta->gst_to_nvds_meta_transform_func = gst_vmbsrc_nvds_meta_transform;
            nvds_meta->gst_to_nvds_meta_release_func = gst_vmbsrc_nvds_meta_transform_release;
        }
        else
        {
            // add failed: no meta took ownership, so free our payload to avoid a leak.
            g_free(payload);
        }
    }
#endif // HAVE_DEEPSTREAM

    // Set filled GstBuffer as output to pass down the pipeline
    *buf = buffer;

    return GST_FLOW_OK;
}

/* -- GstVmbSrcTriggerMeta implementation -------------------------------------------------------- */

// Copy one GstStructure field into another; used to carry the meta across buffer copies.
static gboolean gst_vmbsrc_trigger_meta_copy_field(GQuark field_id, const GValue *value, gpointer user_data)
{
    GstStructure *dst = (GstStructure *)user_data;
    gst_structure_id_set_value(dst, field_id, value);
    return TRUE;
}

static gboolean gst_vmbsrc_trigger_meta_transform(GstBuffer *dest, GstCustomMeta *meta, GstBuffer *buffer,
                                                  GQuark type, gpointer data, gpointer user_data)
{
    UNUSED(buffer);
    UNUSED(data);
    UNUSED(user_data);
    // Carry the metadata across buffer copies unchanged. It is not tied to the pixel data, so it
    // survives any copy transform; other transforms (e.g. scaling) simply don't apply to it.
    if (GST_META_TRANSFORM_IS_COPY(type))
    {
        GstCustomMeta *dst = gst_buffer_add_custom_meta(dest, GST_VMBSRC_TRIGGER_META_NAME);
        if (dst == NULL)
        {
            return FALSE;
        }
        gst_structure_foreach(gst_custom_meta_get_structure(meta),
                              gst_vmbsrc_trigger_meta_copy_field,
                              gst_custom_meta_get_structure(dst));
    }
    return TRUE;
}

const GstMetaInfo *gst_vmbsrc_trigger_meta_get_info(void)
{
    static gsize info = 0;
    if (g_once_init_enter(&info))
    {
        // Registered as a GstCustomMeta (GStreamer >= 1.20) so the fields, stored in a
        // GstStructure, are introspectable from Python via gst_buffer_get_custom_meta().
        static const gchar *tags[] = {NULL};
        const GstMetaInfo *mi = gst_meta_register_custom(GST_VMBSRC_TRIGGER_META_NAME,
                                                         tags,
                                                         gst_vmbsrc_trigger_meta_transform,
                                                         NULL,  /* user_data */
                                                         NULL); /* destroy_data */
        g_once_init_leave(&info, (gsize)mi);
    }
    return (const GstMetaInfo *)info;
}

gboolean gst_buffer_get_vmbsrc_trigger_meta(GstBuffer *buffer, GstVmbSrcTriggerMeta *out)
{
    GstCustomMeta *cmeta = gst_buffer_get_custom_meta(buffer, GST_VMBSRC_TRIGGER_META_NAME);
    if (cmeta == NULL)
    {
        return FALSE;
    }
    const GstStructure *s = gst_custom_meta_get_structure(cmeta);
    // GstClockTime/guint64 fields are stored as G_TYPE_UINT64; correlated as G_TYPE_BOOLEAN.
    // Absent keys leave the corresponding out-field at the initialized default.
    out->trigger_seq = 0;
    out->trigger_time = GST_CLOCK_TIME_NONE;
    out->approx_latency = GST_CLOCK_TIME_NONE;
    out->camera_frame_id = 0;
    out->camera_timestamp = 0;
    out->correlated = FALSE;
    gst_structure_get_uint64(s, "trigger-seq", &out->trigger_seq);
    gst_structure_get_uint64(s, "trigger-time", &out->trigger_time);
    gst_structure_get_uint64(s, "approx-latency", &out->approx_latency);
    gst_structure_get_uint64(s, "camera-frame-id", &out->camera_frame_id);
    gst_structure_get_uint64(s, "camera-timestamp", &out->camera_timestamp);
    gst_structure_get_boolean(s, "correlated", &out->correlated);
    return TRUE;
}

/* -- Hardware-trigger correlation --------------------------------------------------------------- */

// Action signal handler. Called from the external trigger thread to hand a trigger event to the
// element. Pushes it onto the ring buffer for gst_vmbsrc_correlate_trigger to consume.
static void gst_vmbsrc_notify_trigger(GstVmbSrc *vmbsrc, guint64 trigger_seq, guint64 trigger_time_ns)
{
    g_mutex_lock(&vmbsrc->trigger.lock);
    if (vmbsrc->trigger.count == vmbsrc->trigger.capacity)
    {
        // Ring full: drop the oldest pending trigger to make room. This indicates frames are not
        // being consumed fast enough relative to the trigger rate, or correlation is failing.
        vmbsrc->trigger.head = (vmbsrc->trigger.head + 1) % vmbsrc->trigger.capacity;
        vmbsrc->trigger.count--;
        vmbsrc->trigger.overflow_count++;
        GST_WARNING_OBJECT(vmbsrc,
                           "Trigger ring buffer full; dropping oldest pending trigger event "
                           "(total dropped: %" G_GUINT64_FORMAT ")",
                           vmbsrc->trigger.overflow_count);
    }
    guint tail = (vmbsrc->trigger.head + vmbsrc->trigger.count) % vmbsrc->trigger.capacity;
    vmbsrc->trigger.events[tail].seq = trigger_seq;
    vmbsrc->trigger.events[tail].monotonic_time = (GstClockTime)trigger_time_ns;
    vmbsrc->trigger.count++;
    // Latch that triggering is in use for this run; from here on frame_num is drawn from the
    // trigger-sequence space (see gst_vmbsrc_create), never again from the delivered-frame counter.
    vmbsrc->trigger.any_received = TRUE;
    g_mutex_unlock(&vmbsrc->trigger.lock);

    GST_TRACE_OBJECT(vmbsrc, "Received trigger event seq=%" G_GUINT64_FORMAT " time=%" G_GUINT64_FORMAT " ns",
                     trigger_seq, trigger_time_ns);
}

// Match a delivered frame (identified by its CLOCK_MONOTONIC arrival time) to the trigger event
// that most likely produced it, using timestamp proximity. Robust to dropped frames/over-triggers:
// the expected trigger instant is arrival minus the (nominal or adaptively estimated) latency, and
// the nearest pending trigger to that instant is chosen; any older, unmatched triggers are then
// discarded because they produced no delivered frame. Returns TRUE and fills the out-parameters
// when a match within tolerance is found. Must be called with vmbsrc->trigger.lock NOT held.
static gboolean gst_vmbsrc_correlate_trigger(GstVmbSrc *vmbsrc,
                                             GstClockTime arrival_monotonic_ns,
                                             guint64 *out_seq,
                                             GstClockTime *out_trigger_monotonic_ns,
                                             GstClockTime *out_latency_ns)
{
    gboolean correlated = FALSE;
    g_mutex_lock(&vmbsrc->trigger.lock);

    if (vmbsrc->trigger.count > 0)
    {
        // Expected trigger latency: the adaptive estimate once we have one, otherwise the
        // configured nominal value, otherwise zero (pick the trigger nearest to arrival itself).
        GstClockTime latency = vmbsrc->trigger.latency_valid
                                   ? vmbsrc->trigger.latency_estimate
                                   : (GstClockTime)vmbsrc->properties.trigger_latency * GST_USECOND;
        GstClockTime target = (arrival_monotonic_ns > latency) ? (arrival_monotonic_ns - latency) : 0;

        // Find the pending trigger whose timestamp is closest to the expected instant
        guint best_offset = 0;
        GstClockTime best_diff = G_MAXUINT64;
        for (guint i = 0; i < vmbsrc->trigger.count; i++)
        {
            guint idx = (vmbsrc->trigger.head + i) % vmbsrc->trigger.capacity;
            GstClockTime t = vmbsrc->trigger.events[idx].monotonic_time;
            GstClockTime diff = (t > target) ? (t - target) : (target - t);
            if (diff < best_diff)
            {
                best_diff = diff;
                best_offset = i;
            }
        }

        GstClockTime tolerance = (GstClockTime)vmbsrc->properties.trigger_latency_tolerance * GST_USECOND;
        if (tolerance == 0 || best_diff <= tolerance)
        {
            guint best_idx = (vmbsrc->trigger.head + best_offset) % vmbsrc->trigger.capacity;
            GstClockTime matched_time = vmbsrc->trigger.events[best_idx].monotonic_time;
            guint64 matched_seq = vmbsrc->trigger.events[best_idx].seq;
            GstClockTime observed =
                (arrival_monotonic_ns > matched_time) ? (arrival_monotonic_ns - matched_time) : 0;

            if (best_offset > 0)
            {
                GST_DEBUG_OBJECT(vmbsrc,
                                 "Discarding %u older trigger event(s) with no matching frame "
                                 "(dropped frames / over-trigger)",
                                 best_offset);
            }

            // Consume the matched trigger and all older ones
            guint consumed = best_offset + 1;
            vmbsrc->trigger.head = (vmbsrc->trigger.head + consumed) % vmbsrc->trigger.capacity;
            vmbsrc->trigger.count -= consumed;

            // Refine the adaptive latency estimate (exponential moving average)
            if (vmbsrc->trigger.latency_valid)
            {
                vmbsrc->trigger.latency_estimate = (vmbsrc->trigger.latency_estimate * 7 + observed) / 8;
            }
            else
            {
                vmbsrc->trigger.latency_estimate = observed;
                vmbsrc->trigger.latency_valid = TRUE;
            }

            *out_seq = matched_seq;
            *out_trigger_monotonic_ns = matched_time;
            *out_latency_ns = observed;
            correlated = TRUE;
        }
        else
        {
            GST_DEBUG_OBJECT(vmbsrc,
                             "No trigger within tolerance for frame (nearest off by %" G_GUINT64_FORMAT
                             " ns); leaving frame uncorrelated",
                             best_diff);
        }
    }

    g_mutex_unlock(&vmbsrc->trigger.lock);
    return correlated;
}

static gboolean plugin_init(GstPlugin *plugin)
{

    /* FIXME Remember to set the rank if it's an element that is meant to be autoplugged by decodebin. */
    return gst_element_register(plugin, "vmbsrc", GST_RANK_NONE,
                                GST_TYPE_vmbsrc);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
                  GST_VERSION_MINOR,
                  vmbsrc,
                  DESCRIPTION,
                  plugin_init,
                  VERSION,
                  "LGPL",
                  PACKAGE,
                  HOMEPAGE_URL)

/**
 * @brief Opens the connection to the camera given by the ID passed as vmbsrc property and stores the resulting handle
 *
 * @param vmbsrc Provides access to the camera ID and holds the resulting handle
 * @return VmbError_t Return status indicating errors if they occurred
 */
VmbError_t open_camera_connection(GstVmbSrc *vmbsrc)
{
    VmbError_t result = VmbCameraOpen(vmbsrc->camera.id, VmbAccessModeFull, &vmbsrc->camera.handle);
    if (result == VmbErrorSuccess)
    {
        VmbCameraInfoQuery(vmbsrc->camera.id, &vmbsrc->camera.info, sizeof(vmbsrc->camera.info));
        GST_INFO_OBJECT(vmbsrc,
                        "Successfully opened camera %s (model \"%s\", serial \"%s\")",
                        vmbsrc->camera.id,
                        vmbsrc->camera.info.modelName,
                        vmbsrc->camera.info.serialString); // TODO: This seems to show N/A for some cameras (observed with USB)

        // Set the GeV packet size to the highest possible value if a GigE camera is used
        if (VmbErrorSuccess == VmbFeatureCommandRun(vmbsrc->camera.info.streamHandles[0], "GVSPAdjustPacketSize"))
        {
            VmbBool_t is_command_done = VmbBoolFalse;
            do
            {
                if (VmbErrorSuccess != VmbFeatureCommandIsDone(vmbsrc->camera.info.streamHandles[0],
                                                               "GVSPAdjustPacketSize",
                                                               &is_command_done))
                {
                    break;
                }
            } while (VmbBoolFalse == is_command_done);
        }
        vmbsrc->camera.is_connected = true;
        map_supported_pixel_formats(vmbsrc);
    }
    else
    {
        GST_ERROR_OBJECT(vmbsrc,
                         "Could not open camera %s. Got error code: %s",
                         vmbsrc->camera.id,
                         ErrorCodeToMessage(result));
        vmbsrc->camera.is_connected = false;
        // TODO: List available cameras in this case?
        // TODO: Can we signal an error to the pipeline to stop immediately?
    }
    vmbsrc->camera.is_acquiring = false;
    return result;
}

/**
 * @brief Loads the camera user set named by the "userset" property (e.g. "UserSet1") via
 * UserSetSelector/UserSetLoad
 *
 * @param vmbsrc Provides access to the camera handle used for the VmbC calls and holds the name of the
 * user set to load
 * @return VmbError_t Return status indicating errors if they occurred
 */
VmbError_t load_user_set(GstVmbSrc *vmbsrc)
{
    GST_INFO_OBJECT(vmbsrc, "Loading user set \"%s\"", vmbsrc->properties.userset);

    VmbError_t result = VmbFeatureEnumSet(vmbsrc->camera.handle, "UserSetSelector", vmbsrc->properties.userset);
    if (result != VmbErrorSuccess)
    {
        GST_ERROR_OBJECT(vmbsrc,
                         "Could not select user set \"%s\". Got error code: %s",
                         vmbsrc->properties.userset,
                         ErrorCodeToMessage(result));
        return result;
    }

    result = VmbFeatureCommandRun(vmbsrc->camera.handle, "UserSetLoad");
    if (result == VmbErrorSuccess)
    {
        VmbBool_t is_command_done = VmbBoolFalse;
        do
        {
            if (VmbErrorSuccess != VmbFeatureCommandIsDone(vmbsrc->camera.handle, "UserSetLoad", &is_command_done))
            {
                break;
            }
        } while (VmbBoolFalse == is_command_done);
    }
    else
    {
        GST_ERROR_OBJECT(vmbsrc,
                         "Could not load user set \"%s\". Got error code: %s",
                         vmbsrc->properties.userset,
                         ErrorCodeToMessage(result));
    }

    return result;
}

/**
 * @brief Applies the values defiend in the vmbsrc properties to their corresponding camera features
 *
 * @param vmbsrc Provides access to the camera handle used for the VmbC calls and holds the desired values for the
 * modified features
 * @return VmbError_t Return status indicating errors if they occurred
 */
VmbError_t apply_feature_settings(GstVmbSrc *vmbsrc)
{
    bool was_acquiring = vmbsrc->camera.is_acquiring;
    if (vmbsrc->camera.is_acquiring)
    {
        GST_DEBUG_OBJECT(vmbsrc, "Camera was acquiring. Stopping to change feature settings");
        stop_image_acquisition(vmbsrc);
    }
    GEnumValue *enum_entry;

    // exposure time
    // TODO: Workaround for cameras with legacy "ExposureTimeAbs" feature should be replaced with a general legacy
    // feature name handling approach: A static table maps each property, e.g. "exposuretime", to a list of (feature
    // name, set function, get function) pairs, e.g. [("ExposureTime", setExposureTime, getExposureTime),
    // ("ExposureTimeAbs", setExposureTimeAbs, getExposureTimeAbs)]. On startup, the feature list of the connected
    // camera obtained from VmbFeaturesList() is used to determine which set/get function to use.

    VmbError_t result = VmbErrorSuccess;
    if (vmbsrc->properties.exposuretime == -1.)
    {
        GST_DEBUG_OBJECT(vmbsrc, "\"exposuretime\" is set to -1. Not changing camera value");
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"ExposureTime\" to %f", vmbsrc->properties.exposuretime);
        result = VmbFeatureFloatSet(vmbsrc->camera.handle, "ExposureTime", vmbsrc->properties.exposuretime);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else if (result == VmbErrorNotFound)
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"ExposureTime\" to %f. Return code was: %s Attempting \"ExposureTimeAbs\"",
                               vmbsrc->properties.exposuretime,
                               ErrorCodeToMessage(result));
            result = VmbFeatureFloatSet(vmbsrc->camera.handle, "ExposureTimeAbs", vmbsrc->properties.exposuretime);
            if (result == VmbErrorSuccess)
            {
                GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
            }
            else
            {
                GST_WARNING_OBJECT(vmbsrc,
                                   "Failed to set \"ExposureTimeAbs\" to %f. Return code was: %s",
                                   vmbsrc->properties.exposuretime,
                                   ErrorCodeToMessage(result));
            }
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"ExposureTime\" to %f. Return code was: %s",
                               vmbsrc->properties.exposuretime,
                               ErrorCodeToMessage(result));
        }
    }

    // Exposure Auto
    enum_entry = g_enum_get_value(g_type_class_ref(GST_ENUM_EXPOSUREAUTO_MODES), vmbsrc->properties.exposureauto);
    if (enum_entry->value == GST_VMBSRC_AUTOFEATURE_UNCHANGED)
    {
        GST_DEBUG_OBJECT(vmbsrc, "\"ExposureAuto\" is set to %s. Not changing camera value", enum_entry->value_nick);
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"ExposureAuto\" to %s", enum_entry->value_nick);
        result = VmbFeatureEnumSet(vmbsrc->camera.handle, "ExposureAuto", enum_entry->value_nick);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"ExposureAuto\" to %s. Return code was: %s",
                               enum_entry->value_nick,
                               ErrorCodeToMessage(result));
        }
    }

    // Auto whitebalance
    enum_entry = g_enum_get_value(g_type_class_ref(GST_ENUM_BALANCEWHITEAUTO_MODES),
                                  vmbsrc->properties.balancewhiteauto);
    if (enum_entry->value == GST_VMBSRC_AUTOFEATURE_UNCHANGED)
    {
        GST_DEBUG_OBJECT(vmbsrc, "\"BalanceWhiteAuto\" is set to %s. Not changing camera value", enum_entry->value_nick);
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"BalanceWhiteAuto\" to %s", enum_entry->value_nick);
        result = VmbFeatureEnumSet(vmbsrc->camera.handle, "BalanceWhiteAuto", enum_entry->value_nick);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"BalanceWhiteAuto\" to %s. Return code was: %s",
                               enum_entry->value_nick,
                               ErrorCodeToMessage(result));
        }
    }

    // gain
    if (vmbsrc->properties.gain == -1.)
    {
        GST_DEBUG_OBJECT(vmbsrc, "\"gain\" is set to -1. Not changing camera value");
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"Gain\" to %f", vmbsrc->properties.gain);
        result = VmbFeatureFloatSet(vmbsrc->camera.handle, "Gain", vmbsrc->properties.gain);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"Gain\" to %f. Return code was: %s",
                               vmbsrc->properties.gain,
                               ErrorCodeToMessage(result));
        }
    }

    result = set_roi(vmbsrc);

    result = apply_trigger_settings(vmbsrc);

    if (was_acquiring)
    {
        GST_DEBUG_OBJECT(vmbsrc, "Camera was acquiring before changing feature settings. Restarting.");
        result = start_image_acquisition(vmbsrc);
    }

    return result;
}

/**
 * @brief Helper function to set Width, Height, OffsetX and OffsetY feature in correct order to define the region of
 * interest (ROI) on the sensor.
 *
 * The values for setting the ROI are defined as GStreamer properties of the vmbsrc element. Width and height default
 * to -1, and offsetx/offsety default to G_MAXINT; these defaults mean the corresponding feature is left unchanged on
 * the camera. offsetx/offsety additionally accept -1 to center the ROI on the sensor along that axis.
 *
 * @param vmbsrc Provides access to the camera handle used for the VmbC calls and holds the desired values for the
 * modified features
 * @return VmbError_t Return status indicating errors if they occurred
 */
VmbError_t set_roi(GstVmbSrc *vmbsrc)
{
    // TODO: Improve error handling (Perhaps more explicit allowed values are enough?) Early exit on errors?

    VmbError_t result = VmbErrorSuccess;
    gboolean width_requested = vmbsrc->properties.width != -1;
    gboolean height_requested = vmbsrc->properties.height != -1;

    // Reset OffsetX and OffsetY to 0 first so that the full requested width/height is usable, but only if Width or
    // Height are actually about to change. They are reapplied to their requested values (if any) further down.
    if (width_requested || height_requested)
    {
        GST_DEBUG_OBJECT(vmbsrc, "Temporarily resetting \"OffsetX\" and \"OffsetY\" to 0");
        result = VmbFeatureIntSet(vmbsrc->camera.handle, "OffsetX", 0);
        if (result != VmbErrorSuccess)
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"OffsetX\" to 0. Return code was: %s",
                               ErrorCodeToMessage(result));
        }
        result = VmbFeatureIntSet(vmbsrc->camera.handle, "OffsetY", 0);
        if (result != VmbErrorSuccess)
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"OffsetY\" to 0. Return code was: %s",
                               ErrorCodeToMessage(result));
        }
    }

    if (!width_requested)
    {
        GST_DEBUG_OBJECT(vmbsrc, "\"width\" is set to -1. Not changing camera value");
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"Width\" to %d", vmbsrc->properties.width);
        result = VmbFeatureIntSet(vmbsrc->camera.handle, "Width", vmbsrc->properties.width);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"Width\" to value \"%d\". Return code was: %s",
                               vmbsrc->properties.width,
                               ErrorCodeToMessage(result));
        }
    }

    if (!height_requested)
    {
        GST_DEBUG_OBJECT(vmbsrc, "\"height\" is set to -1. Not changing camera value");
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"Height\" to %d", vmbsrc->properties.height);
        result = VmbFeatureIntSet(vmbsrc->camera.handle, "Height", vmbsrc->properties.height);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"Height\" to value \"%d\". Return code was: %s",
                               vmbsrc->properties.height,
                               ErrorCodeToMessage(result));
        }
    }

    // offsetx
    if (vmbsrc->properties.offsetx == G_MAXINT)
    {
        GST_DEBUG_OBJECT(vmbsrc, "\"offsetx\" is set to G_MAXINT. Not changing camera value");
    }
    else
    {
        if (vmbsrc->properties.offsetx == -1)
        {
            // Center along the x-axis using the sensor's max width and the width currently applied on the camera
            // (which might have just been changed above, or is left over from a previous configuration)
            VmbInt64_t sensor_width = 0;
            VmbInt64_t current_width = 0;
            VmbFeatureIntRangeQuery(vmbsrc->camera.handle, "Width", NULL, &sensor_width);
            VmbFeatureIntGet(vmbsrc->camera.handle, "Width", &current_width);
            VmbInt64_t vmb_offsetx = (sensor_width - current_width) >> 1;
            GST_DEBUG_OBJECT(vmbsrc, "ROI centering along x-axis requested. Desired offsetx=%lld",
                             vmb_offsetx);
            // Check if the desired value is valid. If not round to nearest valid value
            VmbInt64_t offsetx_min = 0;
            VmbInt64_t offsetx_max = 0;
            VmbInt64_t offsetx_increment = 0;
            result = VmbFeatureIntRangeQuery(vmbsrc->camera.handle, "OffsetX", &offsetx_min, &offsetx_max);
            if (result == VmbErrorSuccess)
            {
                result = VmbFeatureIntIncrementQuery(vmbsrc->camera.handle, "OffsetX", &offsetx_increment);
                if (result == VmbErrorSuccess)
                {
                    VmbInt64_t valid_vmb_offsetx = RoundToNearestValidValue(vmb_offsetx, offsetx_min, offsetx_max, offsetx_increment);
                    if (valid_vmb_offsetx != vmb_offsetx)
                    {
                        GST_DEBUG_OBJECT(vmbsrc,
                                         "Desired offsetx=%lld was not valid. Using nearest valid value=%lld",
                                         vmb_offsetx,
                                         valid_vmb_offsetx);
                        vmb_offsetx = valid_vmb_offsetx;
                    }
                }
                else
                {
                    GST_DEBUG_OBJECT(vmbsrc,
                                     "Error during imcrement query for OffsetX. Using inital desired value: %s",
                                     ErrorCodeToMessage(result));
                }
            }
            else
            {
                GST_DEBUG_OBJECT(vmbsrc,
                                 "Error during range query for OffsetX. Using inital desired value: %s",
                                 ErrorCodeToMessage(result));
            }
            g_object_set(vmbsrc, "offsetx", (int)vmb_offsetx, NULL);
        }
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"OffsetX\" to %d", vmbsrc->properties.offsetx);
        result = VmbFeatureIntSet(vmbsrc->camera.handle, "OffsetX", vmbsrc->properties.offsetx);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"OffsetX\" to value \"%d\". Return code was: %s",
                               vmbsrc->properties.offsetx,
                               ErrorCodeToMessage(result));
        }
    }

    // offsety
    if (vmbsrc->properties.offsety == G_MAXINT)
    {
        GST_DEBUG_OBJECT(vmbsrc, "\"offsety\" is set to G_MAXINT. Not changing camera value");
    }
    else
    {
        if (vmbsrc->properties.offsety == -1)
        {
            // Center along the y-axis using the sensor's max height and the height currently applied on the camera
            // (which might have just been changed above, or is left over from a previous configuration)
            VmbInt64_t sensor_height = 0;
            VmbInt64_t current_height = 0;
            VmbFeatureIntRangeQuery(vmbsrc->camera.handle, "Height", NULL, &sensor_height);
            VmbFeatureIntGet(vmbsrc->camera.handle, "Height", &current_height);
            VmbInt64_t vmb_offsety = (sensor_height - current_height) >> 1;
            GST_DEBUG_OBJECT(vmbsrc, "ROI centering along y-axis requested. Desired offsety=%lld",
                             vmb_offsety);
            // Check if the desired value is valid. If not round to nearest valid value
            VmbInt64_t offsety_min = 0;
            VmbInt64_t offsety_max = 0;
            VmbInt64_t offsety_increment = 0;
            result = VmbFeatureIntRangeQuery(vmbsrc->camera.handle, "OffsetY", &offsety_min, &offsety_max);
            if (result == VmbErrorSuccess)
            {
                result = VmbFeatureIntIncrementQuery(vmbsrc->camera.handle, "OffsetY", &offsety_increment);
                if (result == VmbErrorSuccess)
                {
                    VmbInt64_t valid_vmb_offsety = RoundToNearestValidValue(vmb_offsety, offsety_min, offsety_max, offsety_increment);
                    if (valid_vmb_offsety != vmb_offsety)
                    {
                        GST_DEBUG_OBJECT(vmbsrc,
                                         "Desired offsety=%lld was not valid. Using nearest valid value=%lld",
                                         vmb_offsety,
                                         valid_vmb_offsety);
                        vmb_offsety = valid_vmb_offsety;
                    }
                }
                else
                {
                    GST_DEBUG_OBJECT(vmbsrc,
                                     "Error during imcrement query for OffsetY. Using inital desired value: %s",
                                     ErrorCodeToMessage(result));
                }
            }
            else
            {
                GST_DEBUG_OBJECT(vmbsrc,
                                 "Error during range query for OffsetY. Using inital desired value: %s",
                                 ErrorCodeToMessage(result));
            }
            g_object_set(vmbsrc, "offsety", (int)vmb_offsety, NULL);
        }
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"OffsetY\" to %d", vmbsrc->properties.offsety);
        result = VmbFeatureIntSet(vmbsrc->camera.handle, "OffsetY", vmbsrc->properties.offsety);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_WARNING_OBJECT(vmbsrc,
                               "Failed to set \"OffsetY\" to value \"%d\". Return code was: %s",
                               vmbsrc->properties.offsety,
                               ErrorCodeToMessage(result));
        }
    }

    return result;
}

/**
 * @brief Helper function to apply values to TriggerSelector, TriggerMode, TriggerSource and TriggerActivation in the
 * correct order
 *
 * Trigger settings are always applied in the order
 * 1. TriggerSelector
 * 2. TriggerActivation
 * 3. TriggerSource
 * 4. TriggerMode
 *
 * @param vmbsrc Provides access to the camera handle used for the VmbC calls and holds the desired values for the
 * modified features
 * @return VmbError_t Return status indicating errors if they occurred
 */
VmbError_t apply_trigger_settings(GstVmbSrc *vmbsrc)
{
    GST_DEBUG_OBJECT(vmbsrc, "Applying trigger settings");

    VmbError_t result = VmbErrorSuccess;
    GEnumValue *enum_entry;

    // TODO: Should  the function start by disabling triggering for all TriggerSelectors to make sure only one is
    // enabled after the function is done?

    // TriggerSelector
    enum_entry = g_enum_get_value(g_type_class_ref(GST_ENUM_TRIGGERSELECTOR_VALUES),
                                  vmbsrc->properties.triggerselector);
    if (enum_entry->value == GST_VMBSRC_TRIGGERSELECTOR_UNCHANGED)
    {
        GST_DEBUG_OBJECT(vmbsrc,
                         "\"TriggerSelector\" is set to %s. Not changing camera value", enum_entry->value_nick);
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"TriggerSelector\" to %s", enum_entry->value_nick);
        result = VmbFeatureEnumSet(vmbsrc->camera.handle, "TriggerSelector", enum_entry->value_nick);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_ERROR_OBJECT(vmbsrc,
                             "Failed to set \"TriggerSelector\" to %s. Return code was: %s",
                             enum_entry->value_nick,
                             ErrorCodeToMessage(result));
            if (result == VmbErrorInvalidValue)
            {
                log_available_enum_entries(vmbsrc, "TriggerSelector");
            }
        }
    }

    // TriggerActivation
    enum_entry = g_enum_get_value(g_type_class_ref(GST_ENUM_TRIGGERACTIVATION_VALUES),
                                  vmbsrc->properties.triggeractivation);
    if (enum_entry->value == GST_VMBSRC_TRIGGERACTIVATION_UNCHANGED)
    {
        GST_DEBUG_OBJECT(vmbsrc,
                         "\"TriggerActivation\" is set to %s. Not changing camera value", enum_entry->value_nick);
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"TriggerActivation\" to %s", enum_entry->value_nick);
        result = VmbFeatureEnumSet(vmbsrc->camera.handle, "TriggerActivation", enum_entry->value_nick);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_ERROR_OBJECT(vmbsrc,
                             "Failed to set \"TriggerActivation\" to %s. Return code was: %s",
                             enum_entry->value_nick,
                             ErrorCodeToMessage(result));
            if (result == VmbErrorInvalidValue)
            {
                log_available_enum_entries(vmbsrc, "TriggerActivation");
            }
        }
    }

    // TriggerSource
    enum_entry = g_enum_get_value(g_type_class_ref(GST_ENUM_TRIGGERSOURCE_VALUES),
                                  vmbsrc->properties.triggersource);
    if (enum_entry->value == GST_VMBSRC_TRIGGERSOURCE_UNCHANGED)
    {

        GST_DEBUG_OBJECT(vmbsrc,
                         "\"TriggerSource\" is set to %s. Not changing camera value", enum_entry->value_nick);
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"TriggerSource\" to %s", enum_entry->value_nick);
        result = VmbFeatureEnumSet(vmbsrc->camera.handle, "TriggerSource", enum_entry->value_nick);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_ERROR_OBJECT(vmbsrc,
                             "Failed to set \"TriggerSource\" to %s. Return code was: %s",
                             enum_entry->value_nick,
                             ErrorCodeToMessage(result));
            if (result == VmbErrorInvalidValue)
            {
                log_available_enum_entries(vmbsrc, "TriggerSource");
            }
        }
    }

    // TriggerMode
    enum_entry = g_enum_get_value(g_type_class_ref(GST_ENUM_TRIGGERMODE_VALUES),
                                  vmbsrc->properties.triggermode);
    if (enum_entry->value == GST_VMBSRC_TRIGGERMODE_UNCHANGED)
    {
        GST_DEBUG_OBJECT(vmbsrc,
                         "\"TriggerMode\" is set to %s. Not changing camera value", enum_entry->value_nick);
    }
    else
    {
        GST_DEBUG_OBJECT(vmbsrc, "Setting \"TriggerMode\" to %s", enum_entry->value_nick);
        result = VmbFeatureEnumSet(vmbsrc->camera.handle, "TriggerMode", enum_entry->value_nick);
        if (result == VmbErrorSuccess)
        {
            GST_DEBUG_OBJECT(vmbsrc, "Setting was changed successfully");
        }
        else
        {
            GST_ERROR_OBJECT(vmbsrc,
                             "Failed to set \"TriggerMode\" to %s. Return code was: %s",
                             enum_entry->value_nick,
                             ErrorCodeToMessage(result));
        }
    }

    return result;
}

#if HAVE_NVMM
static NvBufSurfaceColorFormat get_nvmm_format(GstVideoInfo *video_info)
{
    int video_fmt = GST_VIDEO_INFO_FORMAT(video_info);
    switch (video_fmt)
    {
    case GST_VIDEO_FORMAT_GRAY8:
        return NVBUF_COLOR_FORMAT_GRAY8;
    case GST_VIDEO_FORMAT_UYVY:
        return NVBUF_COLOR_FORMAT_UYVY;  
    case GST_VIDEO_FORMAT_YUY2:
        return NVBUF_COLOR_FORMAT_YUYV;
    case GST_VIDEO_FORMAT_RGB:
        return NVBUF_COLOR_FORMAT_RGB;
    case GST_VIDEO_FORMAT_BGR:
        return NVBUF_COLOR_FORMAT_BGR;
    case GST_VIDEO_FORMAT_BGRx:
        return NVBUF_COLOR_FORMAT_BGRx;
    case GST_VIDEO_FORMAT_RGBA:
        return NVBUF_COLOR_FORMAT_RGBA;
    default:
        GST_ERROR("Format %d not supported", video_fmt);
        return NVBUF_COLOR_FORMAT_INVALID;
    }
}

static uint32_t align_to(uint32_t value, uint32_t alignment)
{
    const uint32_t mask = alignment - 1;
    const uint32_t offset_to_next = (alignment - (value & mask)) & mask;
    return value + offset_to_next;
}
#endif

/**
 * @brief Gets the PayloadSize from the connected camera, allocates and announces frame buffers for capturing
 *
 * @param vmbsrc Provides the camera handle used for the VmbC calls and holds the frame buffers
 * @return VmbError_t Return status indicating errors if they occurred
 */
VmbError_t alloc_and_announce_buffers(GstVmbSrc *vmbsrc, GstVideoInfo *video_info)
{
    VmbUint32_t payload_size;
    VmbError_t result = VmbPayloadSizeGet(vmbsrc->camera.handle, &payload_size);
    if (result == VmbErrorSuccess)
    {
        GST_DEBUG_OBJECT(vmbsrc, "Got \"PayloadSize\" of: %u", payload_size);
        GST_DEBUG_OBJECT(vmbsrc, "Allocating and announcing %d VimbaX frames", vmbsrc->num_frame_buffers);
        GEnumValue *allocation_mode = g_enum_get_value(g_type_class_ref(GST_ENUM_ALLOCATIONMODE_VALUES), vmbsrc->properties.allocation_mode);
        GST_DEBUG_OBJECT(vmbsrc, "Using allocation mode %s", allocation_mode->value_nick);
        for (int i = 0; i < vmbsrc->num_frame_buffers; i++)
        {
            
            
            if (vmbsrc->use_nvmm && HAVE_NVMM)
            {
#if HAVE_NVMM
                /*   In the NVMM buffer allocation it is not possible to pass a specific buffer size. 
                 *   The buffer size also always automatically calculated based on the width and height,
                 *   but for certain interfaces meta data e.g. chunk is stored in the same
                 *   buffer as the image data. To ensure that the NVMM buffer is always large enough
                 *   the height used for allocation is adjusted. Once the image is received completly
                 *   the height in overriden with the actual value.
                 */
                const uint32_t pitch = align_to(GST_VIDEO_INFO_PLANE_STRIDE(video_info, 0), 256);
                const uint32_t aligned_payload_size = align_to(payload_size, 4096);
                const uint32_t buffer_height = aligned_payload_size / pitch;

                GST_DEBUG("Using a height of %u for NVMM allocation\n", buffer_height);

                NvBufSurface *surf = NULL;
                NvBufSurfaceAllocateParams paramsext = { 0 };       
                paramsext.params.gpuId = 0;
                paramsext.params.width = GST_VIDEO_INFO_WIDTH(video_info);
                paramsext.params.height = buffer_height;
                paramsext.params.size = payload_size; 
                paramsext.params.colorFormat = get_nvmm_format(video_info); 
                paramsext.params.layout = NVBUF_LAYOUT_PITCH;
                paramsext.params.memType = NVBUF_MEM_SURFACE_ARRAY;
                paramsext.memtag = NvBufSurfaceTag_CAMERA;

                int err = NvBufSurfaceAllocate(&surf, 1, &paramsext);
                if (err)
                {
                    result = VmbErrorOther;
                    break;
                }

                const uint32_t surf_pitch = surf->surfaceList[0].planeParams.pitch[0];
                const uint32_t camera_pitch = surf->surfaceList[0].planeParams.bytesPerPix[0] * paramsext.params.width;

                if (camera_pitch != surf_pitch) 
                {
                    g_printerr ("width pitch missmatch detected got: %u, required: %u\n", camera_pitch, surf_pitch);
                    return VmbErrorBadParameter;
                }

                vmbsrc->frame_buffers[i].context[2] = surf;

                surf->numFilled = 1;
                
                err = NvBufSurfaceMap(surf, 0, 0, NVBUF_MAP_READ_WRITE);
                if (err)
                {
                    result = VmbErrorInvalidAddress;
                    break;
                }

                vmbsrc->frame_buffers[i].buffer = surf->surfaceList[0].mappedAddr.addr[0];
#else
                UNUSED(video_info);
#endif  
            }
            else if (vmbsrc->properties.allocation_mode == GST_VMBSRC_ALLOCATION_MODE_ANNOUNCE_FRAME)
            {
                // The element is responsible for allocating frame buffers. Some transport layers
                // provide higher performance if specific alignment is observed. Check if this
                // camera has such a requirement. If not this basically becomes a regular allocation
                VmbInt64_t buffer_alignment = 1;
                result = VmbFeatureIntGet(vmbsrc->camera.info.streamHandles[0],
                                        "StreamBufferAlignment",
                                        &buffer_alignment);
                // The result is not really important so we do not have to check it. If the camera
                // requires alignment, the call will have succeeded. If alignment does not matter,
                // the call failed but the default value of 1 was not changed
                GST_DEBUG_OBJECT(vmbsrc,
                                "Using \"StreamBufferAlignment\" of: %llu (read result was %s)",
                                buffer_alignment,
                                ErrorCodeToMessage(result));
                vmbsrc->frame_buffers[i].buffer = VmbAlignedAlloc(buffer_alignment, payload_size);
                if (NULL == vmbsrc->frame_buffers[i].buffer)
                {
                    result = VmbErrorResources;
                    break;
                }
            }
            else
            {
                // The transport layer will allocate suitable buffers
                vmbsrc->frame_buffers[i].buffer = NULL;
            }

            vmbsrc->frame_buffers[i].bufferSize = (VmbUint32_t)payload_size;
            vmbsrc->frame_buffers[i].context[0] = vmbsrc->filled_frame_queue;   // used in vimbax_frame_callback to put filled frames to the queue
            vmbsrc->frame_buffers[i].context[1] = vmbsrc->camera.handle;        // used in glib_destroy_callback to requeue frame for future transfers

            // Announce Frame
            result = VmbFrameAnnounce(vmbsrc->camera.handle,
                                      &vmbsrc->frame_buffers[i],
                                      (VmbUint32_t)sizeof(VmbFrame_t));
            if (result != VmbErrorSuccess)
            {
                free(vmbsrc->frame_buffers[i].buffer);
                memset(&vmbsrc->frame_buffers[i], 0, sizeof(VmbFrame_t));
                break;
            }
        }
    }
    return result;
}

/**
 * @brief Revokes frame buffers, frees their memory and overwrites old pointers with 0
 *
 * @param vmbsrc Provides the camera handle used for the VmbC calls and the frame buffers
 */
void revoke_and_free_buffers(GstVmbSrc *vmbsrc)
{
    for (int i = 0; i < vmbsrc->num_frame_buffers; i++)
    {
        if (NULL != vmbsrc->frame_buffers[i].buffer)
        {
            VmbFrameRevoke(vmbsrc->camera.handle, &vmbsrc->frame_buffers[i]);
            if (vmbsrc->use_nvmm  && HAVE_NVMM)
            {
#if HAVE_NVMM           
                NvBufSurface *surf = vmbsrc->frame_buffers[i].context[2];

                NvBufSurfaceUnMap(surf, 0, 0);
                NvBufSurfaceDestroy(surf);
#endif                
            }
            else if (vmbsrc->properties.allocation_mode == GST_VMBSRC_ALLOCATION_MODE_ANNOUNCE_FRAME)
            {
                // The element allocated the frame buffers, so it must free the memory also
                VmbAlignedFree(vmbsrc->frame_buffers[i].buffer);
            }
            memset(&vmbsrc->frame_buffers[i], 0, sizeof(VmbFrame_t));
        }
    }
}

/**
 * @brief Starts the capture engine, queues VimbaX frames and runs the AcquisitionStart command feature. Frame buffers
 * must be allocated before running this function.
 *
 * @param vmbsrc Provides the camera handle used for the VmbC calls and access to the queued frame buffers
 * @return VmbError_t Return status indicating errors if they occurred
 */
VmbError_t start_image_acquisition(GstVmbSrc *vmbsrc)
{
    // Start Capture Engine
    GST_DEBUG_OBJECT(vmbsrc, "Starting the capture engine");
    VmbError_t result = VmbCaptureStart(vmbsrc->camera.handle);
    if (result == VmbErrorSuccess)
    {
        GST_DEBUG_OBJECT(vmbsrc, "Queueing the VimbaX frames");
        for (int i = 0; i < vmbsrc->num_frame_buffers; i++)
        {
            // Queue Frame
            result = VmbCaptureFrameQueue(vmbsrc->camera.handle, &vmbsrc->frame_buffers[i], &vimbax_frame_callback);
            if (VmbErrorSuccess != result)
            {
                break;
            }
        }

        if (VmbErrorSuccess == result)
        {
            // Start Acquisition
            GST_DEBUG_OBJECT(vmbsrc, "Running \"AcquisitionStart\" feature");
            result = VmbFeatureCommandRun(vmbsrc->camera.handle, "AcquisitionStart");
            VmbBool_t acquisition_start_done = VmbBoolFalse;
            do
            {
                if (VmbErrorSuccess != VmbFeatureCommandIsDone(vmbsrc->camera.handle,
                                                               "AcquisitionStart",
                                                               &acquisition_start_done))
                {
                    break;
                }
            } while (VmbBoolFalse == acquisition_start_done);
            vmbsrc->camera.is_acquiring = true;
        }
    }
    return result;
}

/**
 * @brief Runs the AcquisitionStop command feature, stops the capture engine and flushes the capture queue
 *
 * @param vmbsrc Provides the camera handle which is used for the VmbC function calls
 * @return VmbError_t Return status indicating errors if they occurred
 */
VmbError_t stop_image_acquisition(GstVmbSrc *vmbsrc)
{
    // Stop Acquisition
    GST_DEBUG_OBJECT(vmbsrc, "Running \"AcquisitionStop\" feature");
    VmbError_t result = VmbFeatureCommandRun(vmbsrc->camera.handle, "AcquisitionStop");
    VmbBool_t acquisition_stop_done = VmbBoolFalse;
    do
    {
        if (VmbErrorSuccess != VmbFeatureCommandIsDone(vmbsrc->camera.handle,
                                                       "AcquisitionStop",
                                                       &acquisition_stop_done))
        {
            break;
        }
    } while (VmbBoolFalse == acquisition_stop_done);
    vmbsrc->camera.is_acquiring = false;

    // Stop Capture Engine
    GST_DEBUG_OBJECT(vmbsrc, "Stopping the capture engine");
    result = VmbCaptureEnd(vmbsrc->camera.handle);

    // Flush the capture queue
    GST_DEBUG_OBJECT(vmbsrc, "Flushing the capture queue");
    VmbCaptureQueueFlush(vmbsrc->camera.handle);

    return result;
}

// Callback that will be executed when the GstBuffer instances created by this source are no longer
// referenced by any elements further down the pipeline. Since the GstBuffer uses the same memory as
// the VmbFrame_t that is used to transfer images to avoid copies, this callback is used to requeue
// the frame back for further transmissions from the device
void glib_destroy_callback(gpointer data)
{
    GST_TRACE("glib_destroy_callback is called");
    VmbFrame_t *frame = data;
    VmbError_t err = VmbCaptureFrameQueue(frame->context[1], frame, &vimbax_frame_callback);
    if (err != VmbErrorSuccess)
    {
        GST_ERROR("VmbCaptureFrameQueue failed with error code %i", err);
    }
    else
    {
        GST_DEBUG("VmbCaptureFrameQueue returned %i", err);
    }
}

void VMB_CALL vimbax_frame_callback(const VmbHandle_t camera_handle, const VmbHandle_t stream_handle, VmbFrame_t *frame)
{
    UNUSED(camera_handle); // enable compilation while treating warning of unused vairable as error
    UNUSED(stream_handle);
    GST_TRACE("Got Frame %llu", frame->frameID);
    // Record the moment the frame arrived from the camera, as early as possible, so that the
    // buffer can later be timestamped with its true pipeline-entry time instead of the (possibly
    // much later) time create() dequeues it. g_get_monotonic_time() returns microseconds and is
    // stored in the otherwise unused user context slot context[3]. Storing the value in the
    // pointer-sized slot relies on 64 bit pointers, which holds on the supported platforms and is
    // consistent with the handles already kept in context[0]/context[1].
    frame->context[3] = (void *)(guintptr)g_get_monotonic_time();
    g_async_queue_push(frame->context[0], frame); // context[0] holds vmbsrc->filled_frame_queue

    // requeueing the frame is done after the GstBuffer created in vmbsrc_create is no longer
    // referenced in the pipeline. For this the callback function glib_destroy_callback is used
}

/**
 * @brief Get the VimbaX pixel formats the camera supports and create a mapping of them to compatible GStreamer formats
 * (stored in vmbsrc->camera.supported_formats)
 *
 * @param vmbsrc provides the camera handle and holds the generated mapping
 */
void map_supported_pixel_formats(GstVmbSrc *vmbsrc)
{
    // get number of supported formats from the camera
    VmbUint32_t camera_format_count;
    VmbFeatureEnumRangeQuery(
        vmbsrc->camera.handle,
        "PixelFormat",
        NULL,
        0,
        &camera_format_count);

    // get the VimbaX format string supported by the camera
    const char **supported_formats = malloc(camera_format_count * sizeof(char *));
    VmbFeatureEnumRangeQuery(
        vmbsrc->camera.handle,
        "PixelFormat",
        supported_formats,
        camera_format_count,
        NULL);

    GST_DEBUG_OBJECT(vmbsrc, "Camera returned %d supported formats", camera_format_count);
    VmbBool_t is_available;
    for (unsigned int i = 0; i < camera_format_count; i++)
    {
        VmbFeatureEnumIsAvailable(vmbsrc->camera.handle, "PixelFormat", supported_formats[i], &is_available);
        if (is_available)
        {
            const VimbaXGstFormatMatch_t *format_map = gst_format_from_vimbax_format(supported_formats[i]);
            if (format_map != NULL)
            {
                GST_DEBUG_OBJECT(vmbsrc,
                                 "VimbaX format \"%s\" corresponds to GStreamer format \"%s\"",
                                 supported_formats[i],
                                 format_map->gst_format_name);
                vmbsrc->camera.supported_formats[vmbsrc->camera.supported_formats_count] = format_map;
                vmbsrc->camera.supported_formats_count++;
            }
            else
            {
                GST_DEBUG_OBJECT(vmbsrc,
                                 "No corresponding GStreamer format found for VimbaX format \"%s\"",
                                 supported_formats[i]);
            }
        }
        else
        {
            GST_DEBUG_OBJECT(vmbsrc, "Reported format \"%s\" is not available", supported_formats[i]);
        }
    }
    free((void *)supported_formats);
}

void log_available_enum_entries(GstVmbSrc *vmbsrc, const char *feat_name)
{
    VmbUint32_t trigger_source_count;
    VmbFeatureEnumRangeQuery(
        vmbsrc->camera.handle,
        feat_name,
        NULL,
        0,
        &trigger_source_count);

    const char **trigger_source_values = malloc(trigger_source_count * sizeof(char *));
    VmbFeatureEnumRangeQuery(
        vmbsrc->camera.handle,
        feat_name,
        trigger_source_values,
        trigger_source_count,
        NULL);

    VmbBool_t is_available;
    GST_ERROR_OBJECT(vmbsrc, "The following values for the \"%s\" feature are available", feat_name);
    for (unsigned int i = 0; i < trigger_source_count; i++)
    {
        VmbFeatureEnumIsAvailable(vmbsrc->camera.handle,
                                  feat_name,
                                  trigger_source_values[i],
                                  &is_available);
        if (is_available)
        {
            GST_ERROR_OBJECT(vmbsrc, "    %s", trigger_source_values[i]);
        }
    }

    free((void *)trigger_source_values);
}
