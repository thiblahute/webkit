/*
 * Copyright (C) 2018 Metrological Group B.V.
 * Copyright (C) 2020 Igalia S.L.
 * Author: Thibault Saunier <tsaunier@igalia.com>
 * Author: Alejandro G. Castro  <alex@igalia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"

#if ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
#include "GStreamerVideoCapturer.h"

GST_DEBUG_CATEGORY(webkit_video_capturer_debug);
#define GST_CAT_DEFAULT webkit_video_capturer_debug

namespace WebCore {

static void initializeDebugCategory()
{
    ensureGStreamerInitialized();

    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_video_capturer_debug, "webkitvideocapturer", 0, "WebKit Video Capturer");
    });
}

GStreamerVideoCapturer::GStreamerVideoCapturer(GStreamerCaptureDevice device)
    : GStreamerCapturer(device, adoptGRef(gst_caps_new_empty_simple("video/x-raw")))
{
    initializeDebugCategory();
}

GStreamerVideoCapturer::GStreamerVideoCapturer(const char* sourceFactory, CaptureDevice::DeviceType deviceType)
    : GStreamerCapturer(sourceFactory, adoptGRef(gst_caps_new_empty_simple("video/x-raw")), deviceType)
{
    initializeDebugCategory();
}

GstElement* GStreamerVideoCapturer::createSource()
{
    GST_ERROR("Here I am!");
    auto* src = GStreamerCapturer::createSource();
    if (m_fd)
        g_object_set(m_src.get(), "fd", *m_fd, nullptr);
    return src;
}

GstElement* GStreamerVideoCapturer::createConverter()
{
    // https://gitlab.freedesktop.org/gstreamer/gst-plugins-base/issues/97#note_56575
    auto bin = makeGStreamerBin("capsfilter name=capturer_capsfilter ! decodebin3 name=capturer_decodebin ! videoconvert name=capturer_converter ! videoscale ! videoconvert ! videorate drop-only=1 average-period=1 name=capturer_videorate", false);

    auto capsfilter = adoptGRef(gst_bin_get_by_name (GST_BIN (bin), "capturer_capsfilter"));
    auto caps = gst_caps_new_empty();

    // FIXME: What about CapsFeature?
    gst_caps_append_structure (caps, gst_structure_new ("video/x-raw",
        // Enforce source framerate higher or equal to what user requested
        "framerate", GST_TYPE_FRACTION_RANGE,
            m_fps_n.has_value() ? m_fps_n.value() : 30,
            m_fps_n.has_value() ? m_fps_d.value() : 1,
            G_MAXINT, 1,
        nullptr)
    );

    gst_caps_append_structure (caps,
        gst_structure_new ("video/x-raw",
            "video/x-h264",
            // Enforce source framerate higher or equal to what user requested
            "framerate", GST_TYPE_FRACTION_RANGE,
                m_fps_n.has_value() ? m_fps_n.value() : 30,
                m_fps_n.has_value() ? m_fps_d.value() : 1,
                G_MAXINT, 1,
            "width", GST_TYPE_INT_RANGE, 1280, G_MAXINT,
            "height", GST_TYPE_INT_RANGE, 720, G_MAXINT,
            nullptr
        )
    );
    GST_ERROR("Setting caps: %" GST_PTR_FORMAT, caps);
    g_object_set (capsfilter.get(), "caps", caps, nullptr);

    gst_element_add_pad (bin, gst_ghost_pad_new ("sink", GST_PAD (capsfilter->sinkpads->data)));

    auto videorate = adoptGRef(gst_bin_get_by_name (GST_BIN (bin), "capturer_videorate"));
    ASSERT (videorate);
    gst_element_add_pad (bin, gst_ghost_pad_new ("src", GST_PAD (videorate.get()->srcpads->data)));

    auto decodebin = adoptGRef(gst_bin_get_by_name (GST_BIN (bin), "capturer_decodebin"));
    ASSERT (decodebin);
    GST_ERROR_OBJECT(decodebin.get(), "Added decodebin");
    g_signal_connect(decodebin.get(), "pad-added", G_CALLBACK(+[](GstElement *decodebin, GstPad *pad, GstBin *bin)  {
        UNUSED_PARAM(decodebin);
        RELEASE_ASSERT (GST_PAD_IS_SRC (pad));

        GST_ERROR("Got new pad: %" GST_PTR_FORMAT, pad);
        auto scaler = adoptGRef(gst_bin_get_by_name (GST_BIN (bin), "capturer_converter"));
        GST_ERROR_OBJECT(scaler.get(), ".");

        ASSERT (scaler);

        auto sinkpad = adoptGRef(gst_element_get_static_pad (scaler.get(), "sink"));
        GST_ERROR_OBJECT(sinkpad.get(), ".");
        ASSERT (sinkpad);

        GUniquePtr<char> dumpName(g_strdup_printf("%s_dbin_srcpad_added", GST_OBJECT_NAME(bin)));
        // gst_debug_set_threshold_from_string ("5", TRUE);
        auto lret = gst_pad_link (pad, sinkpad.get());
        GST_ERROR_OBJECT(sinkpad.get(), "Linked? %s", gst_pad_link_get_name (lret));
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(GST_OBJECT_PARENT (bin)), GST_DEBUG_GRAPH_SHOW_ALL, dumpName.get());
        RELEASE_ASSERT_WITH_MESSAGE(lret == GST_PAD_LINK_OK, "Linking decodebin pad with scaler failed: %s",
            gst_pad_link_get_name (lret));

    }), bin);

    return bin;
}

GstVideoInfo GStreamerVideoCapturer::getBestFormat()
{
    GRefPtr<GstCaps> caps = adoptGRef(gst_caps_fixate(gst_device_get_caps(m_device.get())));
    GstVideoInfo info;
    gst_video_info_from_caps(&info, caps.get());

    return info;
}

bool GStreamerVideoCapturer::setSize(int width, int height)
{
    if (m_fd.has_value()) {
        // Pipewiresrc doesn't seem to support caps re-negotiation and framerate configuration properly.
        GST_FIXME_OBJECT(m_pipeline.get(), "Resizing disabled on display capture source");
        return true;
    }

    if (!width || !height)
        return false;

    auto videoResolution = getVideoResolutionFromCaps(m_caps.get());
    if (videoResolution && videoResolution->width() == width && videoResolution->height() == height) {
        GST_DEBUG_OBJECT(m_pipeline.get(), "Size has not changed");
        return true;
    }

    GST_INFO_OBJECT(m_pipeline.get(), "Setting size to %dx%d", width, height);
    m_caps = adoptGRef(gst_caps_copy(m_caps.get()));
    gst_caps_set_simple(m_caps.get(), "width", G_TYPE_INT, width, "height", G_TYPE_INT, height, nullptr);

    if (!m_capsfilter)
        return false;

    g_object_set(m_capsfilter.get(), "caps", m_caps.get(), nullptr);
    return true;
}

bool GStreamerVideoCapturer::setFrameRate(double frameRate)
{
    if (m_fd.has_value()) {
        // Pipewiresrc doesn't seem to support caps re-negotiation and framerate configuration properly.
        GST_FIXME_OBJECT(m_pipeline.get(), "Framerate override disabled on display capture source");
        return true;
    }

    gst_util_double_to_fraction(frameRate, &m_fps_n.value(), &m_fps_d.value());

    if (m_fps_n < -G_MAXINT) {
        m_fps_n.reset();
        m_fps_d.reset();
        GST_INFO_OBJECT(m_pipeline.get(), "Framerate %f not allowed", frameRate);
        return false;
    }

    if (!m_fps_n) {
        m_fps_n.reset();
        m_fps_d.reset();
        GST_INFO_OBJECT(m_pipeline.get(), "Do not force variable framerate");
        return false;
    }

    m_caps = adoptGRef(gst_caps_copy(m_caps.get()));
    gst_caps_set_simple(m_caps.get(), "framerate", GST_TYPE_FRACTION, m_fps_n, m_fps_n, nullptr);

    if (!m_capsfilter)
        return false;

    GST_INFO_OBJECT(m_pipeline.get(), "Setting framerate to %f fps", frameRate);
    g_object_set(m_capsfilter.get(), "caps", m_caps.get(), nullptr);

    return true;
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM) && USE(GSTREAMER)
