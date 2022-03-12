/* GStreamer
 * Copyright 2018 Rockchip Electronics Co., Ltd
 *   Author: James <james.lin@rock-chips.com>
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
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
     
#include <assert.h>
#include <string.h>
     
#include <libavcodec/avcodec.h>

#include <gst/gst.h>
#include "gstav.h"
#include "gstavsubdec.h"
//#include <ass/ass_render.h>


#define GST_FFDEC_PARAMS_QDATA g_quark_from_static_string("avdec-params")

#define gst_ffmpeg_sub_dec_parent_class parent_class
G_DEFINE_TYPE (GstFFMpegSubDec, gst_ffmpeg_sub_dec, GST_TYPE_ELEMENT);

#define MAX_SUBTITLE_LENGTH 1024
#define ZS_SUBTITLE_4K_DEN (4)


/* libass stores an RGBA color in the format RRGGBBTT, where TT is the transparency level */
#define AR(c)  ( (c)>>24)
#define AG(c)  (((c)>>16)&0xFF)
#define AB(c)  (((c)>>8) &0xFF)
#define AA(c)  ((0xFF-(c)) &0xFF)
#define UNPREMULTIPLY_ALPHA(x, y) ((((x) << 16) - ((x) << 9) + (x)) / ((((x) + (y)) << 8) - ((x) + (y)) - (y) * (x)))
#define FAST_DIV255(x) ((((x) + 128) * 257) >> 16)

enum
{
  PROP_0,
  PROP_SUR_WIDTH,
  PROP_SUR_HEIGHT
};

static int gst_ffmpeg_sub_dec_init_ass(GstFFMpegSubDec * ffmpegdec);

static void gst_ffmpeg_sub_dec_base_init(GstFFMpegSubDecClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstPadTemplate *sinktempl, *srctempl;
  GstCaps *sinkcaps = NULL, *srccaps = NULL;
  AVCodec *in_plugin;
  gchar *longname, *description;

  in_plugin =
      (AVCodec *) g_type_get_qdata (G_OBJECT_CLASS_TYPE (klass),
      GST_FFDEC_PARAMS_QDATA);
  g_assert (in_plugin != NULL);

  /* construct the element details struct */
  longname = g_strdup_printf ("libav %s decoder", in_plugin->long_name);
  description = g_strdup_printf ("libav %s decoder", in_plugin->name);
  gst_element_class_set_metadata (element_class, longname,
      "Codec/Decoder/Subtitle", description,
      "James Lin <smallhujiu@gmail.com>");
  g_free (longname);
  g_free (description);

  /* set the caps */
  //sinkcaps = gst_caps_new_empty_simple ("ANY");
  if (!sinkcaps) {
      if(strstr(in_plugin->name,"ssa")){
        sinkcaps = gst_caps_new_empty_simple ("application/x-ssa");
      } else if (strstr(in_plugin->name,"srt")) {
        sinkcaps = gst_caps_new_empty_simple ("application/x-subtitle");
        GST_WARNING ("AV_CODEC_ID_SRT: application/x-subtitle");
      } else {
          switch (in_plugin->id) {
          case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
            sinkcaps = gst_caps_new_empty_simple ("subpicture/x-pgs");
            break;
          case AV_CODEC_ID_DVD_SUBTITLE:
            sinkcaps = gst_caps_new_empty_simple ("subpicture/x-dvd");
            break;
          case AV_CODEC_ID_DVB_SUBTITLE:
            sinkcaps = gst_caps_new_empty_simple ("subpicture/x-dvb");
            break;
          case AV_CODEC_ID_XSUB:
            sinkcaps = gst_caps_new_empty_simple ("subpicture/x-xsub");
            break;
          case AV_CODEC_ID_TEXT:
            sinkcaps = gst_caps_new_empty_simple ("text/x-raw");
            break;
          case AV_CODEC_ID_ASS:
            sinkcaps = gst_caps_new_empty_simple ("application/x-ass");
            break;
          case AV_CODEC_ID_SUBRIP:
            sinkcaps = gst_caps_new_empty_simple ("text/x-raw");
            break;
          default:
            sinkcaps = gst_caps_from_string ("unknown/unknown");
          }
      }
  }

  GST_WARNING ("Init libav codec %s, in_plugin->id %d", in_plugin->name, in_plugin->id);

  srccaps = gst_caps_new_simple("video/x-raw",
    "format", G_TYPE_STRING, "RGBA",
    "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
    "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
    NULL);

  if (!srccaps) {
    GST_ERROR ("Couldn't get source caps for decoder '%s'", in_plugin->name);
    srccaps = gst_caps_from_string ("video/x-raw");
  }

  /* pad templates */
  sinktempl = gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sinkcaps);
  srctempl = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, srccaps);

  gst_element_class_add_pad_template (element_class, srctempl);
  gst_element_class_add_pad_template (element_class, sinktempl);

  klass->in_plugin = in_plugin;
  klass->srctempl = srctempl;
  klass->sinktempl = sinktempl;

};

static void
gst_ffmpeg_sub_dec_finalize (GObject * gobject)
{
  GstFFMpegSubDec *ffmpegdec = (GstFFMpegSubDec *) gobject;

  avsubtitle_free (ffmpegdec->frame);

  if (ffmpegdec->track)
    ass_free_track(ffmpegdec->track);
  if (ffmpegdec->renderer)
    ass_renderer_done(ffmpegdec->renderer);
  if (ffmpegdec->library)
    ass_library_done(ffmpegdec->library);

  if (ffmpegdec->context != NULL) {
    gst_ffmpeg_avcodec_close (ffmpegdec->context);
    av_free (ffmpegdec->context);
    ffmpegdec->context = NULL;
  }

  //GST_ERROR_OBJECT(ffmpegdec, " #########**********************#############");
  G_OBJECT_CLASS (parent_class)->finalize (gobject);
}

static void
gst_ffmpeg_sub_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstFFMpegSubDec *ffmpegdec = NULL;
  AVDictionary *codec_opts = NULL;

  g_return_if_fail (GST_IS_FFMPEG_SUB_DEC (object));
  ffmpegdec = GST_FFMPEG_SUB_DEC (object);

  switch (prop_id) {
    case PROP_SUR_WIDTH:
      ffmpegdec->surface_w = g_value_get_int (value);
      break;
    case PROP_SUR_HEIGHT:
      ffmpegdec->surface_h = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  if (ffmpegdec->surface_w > 0 && ffmpegdec->surface_h > 0) {
    ffmpegdec->context->width = ffmpegdec->surface_w;
    ffmpegdec->context->height = ffmpegdec->surface_h;
    if (ffmpegdec->opened == FALSE) {
        switch (ffmpegdec->codec->id) {
            case AV_CODEC_ID_TEXT:
            case AV_CODEC_ID_ASS:
            case AV_CODEC_ID_SSA:
            case AV_CODEC_ID_SRT:
            case AV_CODEC_ID_SUBRIP:
                ffmpegdec->need_ass = TRUE;
                if (gst_ffmpeg_sub_dec_init_ass(ffmpegdec) < 0) {
                    goto failed_open;
                }
                ffmpegdec->context->pkt_timebase.num = 1;
                ffmpegdec->context->pkt_timebase.den = 1000;
                av_dict_set(&codec_opts, "sub_text_format", "ass", 0);
                av_dict_set(&codec_opts, "sub_charenc", "UTF-8", 0);
                if (gst_ffmpeg_avcodec_open_sub (ffmpegdec->context, ffmpegdec->codec, &codec_opts) < 0)
                  goto failed_open;
                /* Decode subtitles and push them into the renderer (libass) */
                if (ffmpegdec->context->subtitle_header)
                        ass_process_codec_private(ffmpegdec->track,
                                      ffmpegdec->context->subtitle_header,
                                      ffmpegdec->context->subtitle_header_size);
                av_dict_free(&codec_opts);
                break;
            default:
                if (gst_ffmpeg_avcodec_open (ffmpegdec->context, ffmpegdec->codec) < 0)
                  goto failed_open;
                break;
        }
    }

    if (ffmpegdec->need_ass) {
        GST_DEBUG_OBJECT(ffmpegdec, "ass_set_frame_size (%d, %d)!\n", ffmpegdec->surface_w, ffmpegdec->surface_h);
        ass_set_frame_size(ffmpegdec->renderer, ffmpegdec->surface_w, ffmpegdec->surface_h);
    }
    ffmpegdec->opened = TRUE;
  }

failed_open:

    return;
}

static void
gst_ffmpeg_sub_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstFFMpegSubDec *ffmpegdec = NULL;

  g_return_if_fail (GST_IS_FFMPEG_SUB_DEC (object));
  ffmpegdec = GST_FFMPEG_SUB_DEC (object);

  switch (prop_id) {
    case PROP_SUR_WIDTH:
      g_value_set_int (value, ffmpegdec->surface_w);
      break;
    case PROP_SUR_HEIGHT:
      g_value_set_int (value, ffmpegdec->surface_h);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void gst_ffmpeg_sub_dec_class_init(GstFFMpegSubDecClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = gst_ffmpeg_sub_dec_finalize;
  gobject_class->set_property = gst_ffmpeg_sub_dec_set_property;
  gobject_class->get_property = gst_ffmpeg_sub_dec_get_property;

  g_object_class_install_property (gobject_class, PROP_SUR_WIDTH,
      g_param_spec_int ("surface-w", "Surface width", "SURFACE's width which to display subtitle", -1,
          G_MAXINT32, -1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));

  g_object_class_install_property (gobject_class, PROP_SUR_HEIGHT,
      g_param_spec_int ("surface-h", "Surface height", "SURFACE's height which to display subtitle", -1,
          G_MAXINT32, -1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));
  
};

static gboolean
gst_ffmpegsubdec_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean res = FALSE;
  GstFFMpegSubDec *ffmpegdec = GST_FFMPEG_SUB_DEC (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_STOP:
    {
      if (ffmpegdec && ffmpegdec->track) {
        g_mutex_lock (&ffmpegdec->flow_lock);
        ass_flush_events(ffmpegdec->track);
        g_mutex_unlock (&ffmpegdec->flow_lock);
        GST_WARNING_OBJECT (pad, "Forwarding event %" GST_PTR_FORMAT, event);
      }

      res = gst_pad_event_default (pad, parent, event);
      break;
    }
    default:{
      res = gst_pad_event_default (pad, parent, event);
      break;
    }
  }

  return res;
}

static gboolean
gst_ffmpegdec_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }

  return res;
}

static void
gst_avpacket_init (AVPacket * packet, guint8 * data, guint size)
{
  memset (packet, 0, sizeof (AVPacket));
  packet->data = data;
  packet->size = size;
}

static void gst_ffmpegsubdec_picture(GstFFMpegSubDec *ffmpegdec, AVSubtitle* sub, GstClockTime ts, GstClockTime dur)
{
  gint i, j, w, h, size, x, y;
  guint8 **data;
  guint8 *ptr;
  GstBuffer *buffer;
  GstVideoCropMeta *vcmeta;

  if (sub && sub->num_rects) {
    //GST_WARNING_OBJECT(ffmpegdec, " sub->num_rects=(%d), width=%d, height=%d", sub->num_rects, ffmpegdec->context->width, ffmpegdec->context->height);
    for (i = 0; i < sub->num_rects; i++) {
      w = sub->rects[i]->w;
      h = sub->rects[i]->h;
      x = sub->rects[i]->x;// * ffmpegdec->surface_w / ffmpegdec->context->width;
      y = sub->rects[i]->y;// * ffmpegdec->surface_h / ffmpegdec->context->height;
      //GST_WARNING_OBJECT(ffmpegdec, "Picture parametes (%d:%d) [%d,%d]", x, y, w, h);

      if ( (ffmpegdec->surface_w >= 3840 /*&& ffmpegdec->surface_h >= 2160*/) && (h >= ffmpegdec->surface_h / ZS_SUBTITLE_4K_DEN) ) {
          GST_WARNING_OBJECT(ffmpegdec, " 4k video(%d), picture subtitle only show %d height", h, (ffmpegdec->surface_h / ZS_SUBTITLE_4K_DEN));
          //continue;
      }

      size = w*h*4;
      data = sub->rects[i]->pict.data;
      ptr = g_malloc0(size);
      for (j = 0; j < w * h; j++) {
        gint index = data[0][j];
        guint8 r = data[1][index*4];
        guint8 g = data[1][index*4 + 1];
        guint8 b = data[1][index*4 + 2];
        guint8 a = data[1][index*4 + 3];

        ptr[4*j + 0] = a*r/255;
        ptr[4*j + 1] = a*g/255;
        ptr[4*j + 2] = a*b/255;
        ptr[4*j + 3] = a*a/255;
      }
      buffer = gst_buffer_new_allocate (NULL, size, NULL);
      gst_buffer_fill(buffer, 0, ptr, size);
      gst_buffer_add_video_meta (buffer, GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_RGBA, w, h);
      GST_BUFFER_PTS(buffer) = ts;
      GST_BUFFER_DURATION(buffer) = dur;

      vcmeta = gst_buffer_add_video_crop_meta (buffer);
      vcmeta->x = x;
      vcmeta->y = y;
      vcmeta->width = ffmpegdec->context->width;
      vcmeta->height = ffmpegdec->context->height;

      /*GST_WARNING_OBJECT(ffmpegdec, "Have picture w:%d, h:%d, ts %"
        GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, w, h, GST_TIME_ARGS (ts), GST_BUFFER_DURATION (buffer));*/

      gst_pad_push(ffmpegdec->srcpad, buffer);
      g_free(ptr);
    }
  }else {
     buffer = gst_buffer_new_allocate (NULL, 1, NULL);
     gst_buffer_add_video_meta (buffer, GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_RGBA, 0, 0);
     GST_BUFFER_PTS(buffer) = ts;
     GST_BUFFER_DURATION(buffer) = dur;
     gst_pad_push(ffmpegdec->srcpad, buffer);

     /*GST_WARNING_OBJECT(ffmpegdec, "Have empty picture ts %"
        GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, GST_TIME_ARGS (ts), GST_BUFFER_DURATION (buffer));*/
  }
}


static void gst_ffmpegsubdec_text(GstFFMpegSubDec *ffmpegdec, AVSubtitle* sub, GstClockTime ts, GstClockTime dur)
{
    gint i, j, w, h, size;
    guint8 *data;
    guint8 *ptr;
    GstBuffer *buffer;
    if (sub && sub->num_rects) {
        for (i = 0; i < sub->num_rects; i++) {
            w = sub->rects[i]->w;
            h = sub->rects[i]->h;
            size = MAX_SUBTITLE_LENGTH;
            if( sub->rects[i]->ass!= NULL){
             data = sub->rects[i]->ass;
             char * rawContent = sub->rects[i]->ass;
             ptr = g_malloc(size);
             int hour1, hour2,hour3,min1, min2, min3,sec1, sec2,sec3, msec1, msec2,msec3, index;
             char* txtContent[MAX_SUBTITLE_LENGTH]= {0};
             if(strlen(rawContent)>= (MAX_SUBTITLE_LENGTH-1)){
                 GST_ERROR("invalid rawContent; Too long");
             }
             if(ffmpegdec->codec->id == AV_CODEC_ID_TEXT){
                 if(sscanf(rawContent, "Dialogue: %2d,%02d:%02d:%02d.%02d,%02d:%02d:%02d.%02d,Default,%[^\r\n]",
                           &index, &hour1, &min1, &sec1, &msec1, &hour2, &min2, &sec2, &msec2, txtContent) != 10) {
                        GST_ERROR(" ERROR_MALFORMED; txtContent:%s", txtContent);
                  }
             } else if(ffmpegdec->codec->id == AV_CODEC_ID_ASS){
                 if(sscanf(rawContent, "Dialogue: %d,%d:%2d:%2d.%2d,%d:%2d:%2d.%2d,Default,,%4d,%4d,%4d,,%[^\r\n]", 
                           &index, &hour1, &min1, &sec1, &msec1, &hour2, &min2, &sec2, &msec2, &hour3, &min3, &sec3, txtContent) != 12) {
                        GST_ERROR(" ERROR_MALFORMED; txtContent:%s", txtContent);
                  }
             }

             int  txtLen = strlen(txtContent);
             if(txtLen <= 0) {
                GST_ERROR("invalid rawContent;NULL");
             }
             filterSpecialChar(txtContent);
             strcpy(ptr, txtContent);
            GST_ERROR(">>>>>>>>>>>>>>> txtContent:%s", txtContent);
            buffer = gst_buffer_new_allocate (NULL, size, NULL);
            gst_buffer_fill(buffer, 0, ptr, size);
            gst_buffer_add_video_meta (buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV21, w, h);

            GST_BUFFER_PTS(buffer) = ts;
            GST_BUFFER_DURATION(buffer) = dur;

            GST_DEBUG_OBJECT(ffmpegdec, " Have text w:%d, h:%d, ts %"
            GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, w, h, GST_TIME_ARGS (ts), GST_BUFFER_DURATION (buffer));

            gst_pad_push(ffmpegdec->srcpad, buffer);
            g_free(ptr);

            push_void_buffer(ffmpegdec, ts+dur, 0);
               
            }
          }
        }else {
	      buffer = gst_buffer_new_allocate (NULL, 0, NULL);
	      gst_buffer_add_video_meta (buffer, GST_VIDEO_FRAME_FLAG_NONE,
	         GST_VIDEO_FORMAT_NV21, 0, 0);
	      GST_BUFFER_PTS(buffer) = ts;
	      GST_BUFFER_DURATION(buffer) = dur;
	      gst_pad_push(ffmpegdec->srcpad, buffer);

	      GST_DEBUG_OBJECT(ffmpegdec, "Have empty picture ts %"
	         GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, GST_TIME_ARGS (ts), GST_BUFFER_DURATION (buffer));
      }
}

static void gst_ffmpegsubdec_ass2picture(GstFFMpegSubDec *ffmpegdec, AVSubtitle* sub, GstClockTime ts, GstClockTime dur)
{
    gint i, j, w, h, size;
    guint8 *data;
    guint8 *ptr;
    GstBuffer *buffer;
    int64_t start_time = (int64_t)ts;
    int64_t duration = (int64_t)dur;
    int detect_change = 0;
    int stride = 0;
    GstVideoCropMeta *vcmeta;

    //GST_WARNING_OBJECT(ffmpegdec, " sub->num_rects=(%d), width=%d, height=%d", sub->num_rects, ffmpegdec->context->width, ffmpegdec->context->height);
    if (sub && sub->num_rects) {
        for (i = 0; i < sub->num_rects; i++) {
            char *ass_line = sub->rects[i]->ass;
            if (!ass_line)
                break;
            ass_process_chunk(ffmpegdec->track, ass_line, strlen(ass_line),
                              start_time, duration);
        }

        ASS_Image *image = ass_render_frame(ffmpegdec->renderer, ffmpegdec->track,
                                        start_time, &detect_change);
        if (detect_change) {
            w = ffmpegdec->surface_w;
            h = ffmpegdec->surface_h;
            if (w >= 3840 /*&& h >= 2160*/) {
                h /= ZS_SUBTITLE_4K_DEN;
                //GST_DEBUG_OBJECT(ffmpegdec, " 4k video(%d), text subtitle only show %d height", w, h);
            }

            ASS_Image *image_tmp = image;
            gint dst_y_least = image_tmp?image_tmp->dst_y:0;
            for (; image_tmp; image_tmp = image_tmp->next) {
                if (dst_y_least > image_tmp->dst_y)
                    dst_y_least = image_tmp->dst_y;
            }
            size = w * h * 4;
            ptr = g_malloc0(size);
            stride = w * 4;
            //GST_DEBUG_OBJECT(ffmpegdec, " Text sub position image:%p", image);
            for (; image; image = image->next) {
                uint8_t rgba_color[] = {AR(image->color), AG(image->color), AB(image->color), AA(image->color)};
                int x, y, dst_y_new;
                
                unsigned char *src = NULL;
                unsigned char *dst = NULL;

                src = image->bitmap;
                if (image->h > h) {
                    GST_WARNING_OBJECT(ffmpegdec, " 4k video, image->h=%d, h=%d,drop it!!!", image->h, h);
                    continue;
                }else if ( h <= ffmpegdec->surface_h/ZS_SUBTITLE_4K_DEN ) {//4K
                    #if 0
                    if (image->dst_y > (ffmpegdec->surface_h - h)) {
                        dst_y_new = image->dst_y - (ffmpegdec->surface_h - h);
                    }else {
                        GST_WARNING_OBJECT(ffmpegdec, " 4k video, image->dst_y=%d, drop it!!!", image->dst_y);
                        continue;
                    }
                    #endif
                    dst_y_new = image->dst_y - dst_y_least;
                    if (dst_y_new + image->h > h) {
                        GST_WARNING_OBJECT(ffmpegdec, " 4k video, image->h=%d, h=%d, dst_y_new=%d,drop it!!!", image->h, h, dst_y_new);
                        continue;
                    }
                }else {
                    dst_y_new = image->dst_y;
                }
                dst = (unsigned char *)ptr + dst_y_new * stride + image->dst_x * 4;
                /*GST_WARNING_OBJECT(ffmpegdec, " Text sub position image->dst_x:%d, image->dst_y:%d, image->w=%d, image->h=%d, dst_y_new=%d", 
                    image->dst_x, image->dst_y, image->w, image->h, dst_y_new);*/

                for (y = 0; y < image->h; ++y) {
                    for (x = 0; x < image->w; ++x) {
                        unsigned alpha = ((unsigned)src[x]) * rgba_color[3] / 255;
                        unsigned alphasrc = alpha;
                        if (alpha != 0 && alpha != 255)
                            alpha = UNPREMULTIPLY_ALPHA(alpha, dst[x * 4 + 3]);
                        switch (alpha)
                        {
                            case 0:
                                break;
                            case 255:
                                dst[x * 4 + 0] = rgba_color[2];//b
                                dst[x * 4 + 1] = rgba_color[1];//g
                                dst[x * 4 + 2] = rgba_color[0];//r
                                dst[x * 4 + 3] = alpha;
                                break;
                            default:
                                dst[x * 4 + 0] = FAST_DIV255(dst[x * 4 + 0] * (255 - alpha) + rgba_color[2] * alpha);//b
                                dst[x * 4 + 1] = FAST_DIV255(dst[x * 4 + 1] * (255 - alpha) + rgba_color[1] * alpha);//g
                                dst[x * 4 + 2] = FAST_DIV255(dst[x * 4 + 2] * (255 - alpha) + rgba_color[0] * alpha);//r
                                dst[x * 4 + 3]+= FAST_DIV255((255 - dst[x * 4 + 3]) * alphasrc);
                        }
                    }
                    src += image->stride;
                    dst += stride;
                }
            }

            buffer = gst_buffer_new_allocate (NULL, size, NULL);
            gst_buffer_fill(buffer, 0, ptr, size);
            gst_buffer_add_video_meta (buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGBA, w, h);
            GST_BUFFER_PTS(buffer) = ts;
            GST_BUFFER_DURATION(buffer) = dur;
            if (h <= ffmpegdec->surface_h/ZS_SUBTITLE_4K_DEN) {//4K
                vcmeta = gst_buffer_add_video_crop_meta (buffer);

                vcmeta->x = 0;
                vcmeta->y = dst_y_least;
                vcmeta->width = 0;
                vcmeta->height = ffmpegdec->context->height;
            }

            /*GST_DEBUG_OBJECT(ffmpegdec, " Have pic w:%d, h:%d, ts %"
                GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, w, h, GST_TIME_ARGS (ts), GST_BUFFER_DURATION (buffer));*/

            gst_pad_push(ffmpegdec->srcpad, buffer);
            
            g_free(ptr);
            //gst_buffer_unref(buffer);
            push_void_buffer(ffmpegdec, ts+dur, 0);
        }
    }else {
        buffer = gst_buffer_new_allocate (NULL, 1, NULL);
        gst_buffer_add_video_meta (buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGBA, 0, 0);
        GST_BUFFER_PTS(buffer) = ts;
        GST_BUFFER_DURATION(buffer) = dur;
        gst_pad_push(ffmpegdec->srcpad, buffer);

        /*GST_DEBUG_OBJECT(ffmpegdec, "Have empty picture ts %"
            GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, GST_TIME_ARGS (ts), GST_BUFFER_DURATION (buffer));*/
        //gst_buffer_unref(buffer);
    }
}


void push_void_buffer(GstFFMpegSubDec *ffmpegdec,GstClockTime ts, GstClockTime dur)
{

     GstBuffer *buffer;
     buffer = gst_buffer_new_allocate (NULL, 1, NULL);
     gst_buffer_add_video_meta (buffer, GST_VIDEO_FRAME_FLAG_NONE,
         GST_VIDEO_FORMAT_RGBA , 0, 0);
     GST_BUFFER_PTS(buffer) = ts;
     GST_BUFFER_DURATION(buffer) = dur;
     gst_pad_push(ffmpegdec->srcpad, buffer);

     GST_DEBUG_OBJECT(ffmpegdec, "Text Have empty picture ts %"
         GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, GST_TIME_ARGS (ts), GST_BUFFER_DURATION (buffer));
     //gst_buffer_unref(buffer);

}

void filterSpecialChar(char* rawContent)
{
    int i;
    if(NULL != rawContent && strlen(rawContent) > 0) {
        for (i =0; i <strlen(rawContent) - 1; i++) {
            if ((rawContent + i) && (*(rawContent + i) == 0x5C)) {
                if (((*(rawContent + i + 1) == 0x6E) ||
                        (*(rawContent + i + 1) == 0x4E))) {
                    //replace "\n" or "\N" with "\r\n"
                    *(rawContent + i + 0) = 0x0D;
                    *(rawContent + i + 1) = 0x0A;
                } else if (*(rawContent + i + 1) == 0x68) {
                    // replace "\h" with space
                    *(rawContent + i + 0) = 0x20;
                    *(rawContent + i + 1) = 0x20;
                }
            }
        }
    }

    int error = 0;
    do {
        error = filterSpecialTag(rawContent);
    } while(0 == error);
}

int  filterSpecialTag(char* rawContent)
{
    int pattern[3] = {0};
    int len,idx,i;
    len = idx = i =0;
    pattern[0] = pattern[1] = pattern[2] = -1;
    if((NULL!=rawContent)||(strlen(rawContent)>0)) {
        len = strlen(rawContent);
        for (i =0; i < len; i++) {
            if(rawContent[i]=='{') {
                pattern[0] = i;
            }
            if(rawContent[i]=='\\') {
                if((-1 != pattern[0])&&(i==(pattern[0]+1))) {
                    pattern[1] = i;
                    //ALOGE("filterSpecialTag pattern[1] = %d", pattern[1]);
                } else {
                    pattern[0] = -1;
                }
            }
            if((rawContent[i]=='}')&&(-1 != pattern[0])&&(-1 != pattern[1])) {
                pattern[2] = i;
                //ALOGE("filterSpecialTag pattern[2] = %d", pattern[2]);
                break;
            }
        }
    }
    if((pattern[0] >= 0)&&(pattern[2] > 0)&&(len>0)) {
        //ALOGE("filterSpecialTag pattern[0] =%d; pattern[2]=%d; rawContent = %s", pattern[0], pattern[2], rawContent);
        char newContent[MAX_SUBTITLE_LENGTH]= {0};
        memset(newContent, 0, MAX_SUBTITLE_LENGTH);
        for(i = 0; i < pattern[0]; i++) {
            newContent[idx] = rawContent[i];
            idx++;
        }

        for(i = pattern[2]+1; i < len; i++) {
            newContent[idx] = rawContent[i];
            idx++;
        }
        snprintf(rawContent, MAX_SUBTITLE_LENGTH, "%s", newContent);
        return 0;
    }
    return -1;
}

static void gst_ffmpegsubdec_text_direct(GstFFMpegSubDec *ffmpegdec, gchar* sub, gint bsize, GstClockTime ts, GstClockTime dur)
{

    if(bsize > MAX_SUBTITLE_LENGTH){
        return;
    }
    gint w = 0, h = 0;
    guint8 *ptr;
    GstBuffer *buffer;
    if (sub ) {

        gchar  rawContent[MAX_SUBTITLE_LENGTH]={0};
        gchar *tempContent, dstContent[MAX_SUBTITLE_LENGTH]={0};
        gint rawsize, tempsize, dstsize = 0;
         // copy sub 
        strncpy(rawContent ,sub,bsize);
        rawsize = strlen(rawContent);
        GST_ERROR("rawContent:%s, rawsize:%d", rawContent, rawsize);       

        int32_t needFindCommaCnt = 8;
        if(ffmpegdec->codec->id == AV_CODEC_ID_TEXT){
            needFindCommaCnt = 4;
        }
        int32_t cnt =0;
        int i =0; 

        tempContent = rawContent;
        tempsize = rawsize;


        // find text part
        for (i =0; i <rawsize; i++) {
            if (*(rawContent + i) == 0x2C) {
                cnt++;
                if (cnt == needFindCommaCnt) {
                    break;
                }
            }
        }
        GST_ERROR("tempContent:%s, i:%d", tempContent, i);

        if ((i+1) >= rawsize) {
           ;
        } else {
            tempContent = rawContent + i + 1;
            tempsize -=i;
        }

        GST_ERROR("tempContent:%s, tempsize:%d", tempContent, tempsize);

        gboolean enAbleCopyFlag = FALSE;
        guint dstFindSize =0;

        strncpy(dstContent,tempContent, tempsize);
        dstsize = tempsize;
        for (i =0; i < tempsize; i++) {
            if ((*(tempContent + i) == '{')) {
                enAbleCopyFlag = FALSE;
                continue;
            } else if ((*(tempContent + i) == '}')) {
                enAbleCopyFlag = TRUE;
                continue;
            }

            if (enAbleCopyFlag == TRUE) {
                dstContent[dstFindSize] = *(tempContent + i);
                dstFindSize++;
                dstsize = dstFindSize;
            }
        }


        filterSpecialChar(dstContent);

        ptr = g_malloc(dstsize);
        strncpy(ptr, dstContent, dstsize);
        GST_ERROR("dstContent:%s, dstsize:%d", dstContent, dstsize);
        buffer = gst_buffer_new_allocate (NULL, dstsize, NULL);
        gst_buffer_fill(buffer, 0, ptr, dstsize);
        gst_buffer_add_video_meta (buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV21, w, h);

        GST_BUFFER_PTS(buffer) = ts;
        GST_BUFFER_DURATION(buffer) = dur;

        GST_DEBUG_OBJECT(ffmpegdec, " Have text w:%d, h:%d, ts %"
        GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, w, h, GST_TIME_ARGS (ts), GST_BUFFER_DURATION (buffer));

        gst_pad_push(ffmpegdec->srcpad, buffer);
        g_free(ptr);

        push_void_buffer(ffmpegdec, ts+dur, 0);
  
    }
}

static GstFlowReturn
gst_ffmpegsubdec_chain (GstPad * pad, GstObject * parent, GstBuffer * inbuf)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstFFMpegSubDec *ffmpegdec;
  GstMapInfo map;
  guint8 *bdata;
  glong bsize = 0;
  guint8 retry = 10;

  ffmpegdec = GST_FFMPEG_SUB_DEC (parent);

  /*GST_WARNING_OBJECT (ffmpegdec, "Have buffer of size %" G_GSIZE_FORMAT ", ts %"
      GST_TIME_FORMAT ", dur %" G_GINT64_FORMAT, gst_buffer_get_size (inbuf),
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (inbuf)), GST_BUFFER_DURATION (inbuf));*/

  g_mutex_lock (&ffmpegdec->flow_lock);
  gst_buffer_map (inbuf, &map, GST_MAP_READ);

  bdata = map.data;
  bsize = map.size;
  /*
  if (ffmpegdec->context->codec_id == AV_CODEC_ID_SSA ||ffmpegdec->context->codec_id == AV_CODEC_ID_ASS || ffmpegdec->context->codec_id == AV_CODEC_ID_TEXT) {
      gst_ffmpegsubdec_text_direct(ffmpegdec, (gchar*)bdata, bsize,GST_BUFFER_TIMESTAMP (inbuf),GST_BUFFER_DURATION (inbuf));
      goto beach;
  }*/
  //GST_WARNING_OBJECT (ffmpegdec, "Have buffer[%s] ", (gchar*)bdata);

  do {
    AVPacket packet = {0};
    gint have_data = 0;    
    gint len = -1;

    if (ffmpegdec->context->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
      if (bsize >= 3 && bdata[0] == 0x20 && bdata[1] == 0x00) {
        bdata += 2;
        bsize -= 2;
      }
    }

    gst_avpacket_init (&packet, bdata, bsize);

    if (0 && ffmpegdec->context->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE && ffmpegdec->surface_w >= 3840 && ffmpegdec->surface_h >= 2160) {
        len = bsize;
    }else {
        len = avcodec_decode_subtitle2(ffmpegdec->context, ffmpegdec->frame,
                               &have_data,
                               &packet);
    }

    if (len >= 0) {
      bdata += len;
      bsize -= len;
    }

    if (have_data) {
      if (ffmpegdec->frame->format == 0 /* graphics */) {
        switch(ffmpegdec->context->codec_id) {
        case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        case AV_CODEC_ID_DVB_SUBTITLE:
        case AV_CODEC_ID_DVD_SUBTITLE:
        case AV_CODEC_ID_XSUB:
          gst_ffmpegsubdec_picture(ffmpegdec, ffmpegdec->frame, 
            GST_BUFFER_TIMESTAMP (inbuf), GST_BUFFER_DURATION (inbuf));
          break;
        default:
          break;
        }
      } else if ( ffmpegdec->frame->format == 1 ){
        switch(ffmpegdec->context->codec_id) {
        case AV_CODEC_ID_TEXT:
        case AV_CODEC_ID_ASS:
        case AV_CODEC_ID_SSA:
        case AV_CODEC_ID_SRT:
        case AV_CODEC_ID_SUBRIP:
           gst_ffmpegsubdec_ass2picture(ffmpegdec, ffmpegdec->frame,
            GST_BUFFER_TIMESTAMP (inbuf), GST_BUFFER_DURATION (inbuf));
          break;
        default:
          break;
        }
      }
      avsubtitle_free(ffmpegdec->frame);
    }
  } while (bsize > 0 && retry--);
  g_mutex_unlock (&ffmpegdec->flow_lock);
beach:

  gst_buffer_unmap(inbuf, &map);
  gst_buffer_unref(inbuf);

  return ret;
}

static int gst_ffmpeg_sub_dec_init_ass(GstFFMpegSubDec * ffmpegdec)
{
    int ret = 0;
    
    ffmpegdec->library = ass_library_init();
    if (!ffmpegdec->library) {
        GST_ERROR_OBJECT(ffmpegdec, "ass_library_init() failed!\n");
        return -1;
    }
    ass_set_fonts_dir(ffmpegdec->library, NULL);

    ffmpegdec->renderer = ass_renderer_init(ffmpegdec->library);
    if (!ffmpegdec->renderer) {
        GST_ERROR_OBJECT(ffmpegdec, "ass_renderer_init() failed!\n");
        return -1;
    }

    ffmpegdec->track = ass_new_track(ffmpegdec->library);
    if (!ffmpegdec->track) {
        GST_ERROR_OBJECT(ffmpegdec, "ass_new_track() failed!\n");
        return -1;
    }

    /* Initialize fonts */
    ass_set_fonts(ffmpegdec->renderer, NULL, NULL, 1, NULL, 1);

    return ret;
}

static void gst_ffmpeg_sub_dec_init(GstFFMpegSubDec * ffmpegdec)
{
  GstFFMpegSubDecClass *klass =
      (GstFFMpegSubDecClass *) G_OBJECT_GET_CLASS (ffmpegdec);
  AVDictionary *codec_opts = NULL;

  ffmpegdec->opened = FALSE;
  ffmpegdec->need_ass = FALSE;

  if (!klass->in_plugin)
    return;

  ffmpegdec->sinkpad = gst_pad_new_from_template (klass->sinktempl, "sink");
  gst_pad_set_chain_function (ffmpegdec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ffmpegsubdec_chain));
  gst_pad_set_event_function (ffmpegdec->sinkpad,
      GST_DEBUG_FUNCPTR (gst_ffmpegsubdec_sink_event));
  gst_element_add_pad (GST_ELEMENT (ffmpegdec), ffmpegdec->sinkpad);

  ffmpegdec->srcpad = gst_pad_new_from_template (klass->srctempl, "src");
  gst_pad_set_event_function (ffmpegdec->srcpad,
      GST_DEBUG_FUNCPTR (gst_ffmpegdec_src_event));
  gst_element_add_pad (GST_ELEMENT (ffmpegdec), ffmpegdec->srcpad);

  /* some ffmpeg data */
  ffmpegdec->codec = avcodec_find_decoder(klass->in_plugin->id);
  if (ffmpegdec->codec) {
    GST_DEBUG_OBJECT(ffmpegdec, "try open codec %d name %s\n", ffmpegdec->codec->id, ffmpegdec->codec->name);
    ffmpegdec->context = avcodec_alloc_context3 (ffmpegdec->codec);
    ffmpegdec->context->opaque = ffmpegdec;

    ffmpegdec->frame = av_mallocz(sizeof(AVSubtitle));

    switch (ffmpegdec->codec->id) {
        case AV_CODEC_ID_TEXT:
        case AV_CODEC_ID_ASS:
        case AV_CODEC_ID_SSA:
        case AV_CODEC_ID_SRT:
        case AV_CODEC_ID_SUBRIP:
            ffmpegdec->need_ass = TRUE;
            if (gst_ffmpeg_sub_dec_init_ass(ffmpegdec) < 0) {
                goto could_not_open;
            }
            ffmpegdec->context->pkt_timebase.num = 1;
            ffmpegdec->context->pkt_timebase.den = 1000;
            av_dict_set(&codec_opts, "sub_text_format", "ass", 0);
            av_dict_set(&codec_opts, "sub_charenc", "UTF-8", 0);
            if (gst_ffmpeg_avcodec_open_sub (ffmpegdec->context, ffmpegdec->codec, &codec_opts) < 0)
              goto could_not_open;
            /* Decode subtitles and push them into the renderer (libass) */
            if (ffmpegdec->context->subtitle_header)
                    ass_process_codec_private(ffmpegdec->track,
                                  ffmpegdec->context->subtitle_header,
                                  ffmpegdec->context->subtitle_header_size);
            av_dict_free(&codec_opts);
            break;
        default:
            if (gst_ffmpeg_avcodec_open (ffmpegdec->context, ffmpegdec->codec) < 0)
              goto could_not_open;
            break;
    }
    
    ffmpegdec->opened = TRUE;

    GST_WARNING_OBJECT (ffmpegdec, "Opened libav codec %s, id %d",
        ffmpegdec->codec->name, ffmpegdec->codec->id);
  }

  return;

could_not_open:

  gst_ffmpeg_avcodec_close (ffmpegdec->context);
  GST_ERROR_OBJECT (ffmpegdec, "avdec_%s: Failed to open libav codec",
      klass->in_plugin->name);
};

gboolean
gst_ffmpegsubdec_register (GstPlugin * plugin)
{
  GTypeInfo typeinfo = {
    sizeof (GstFFMpegSubDecClass),
    (GBaseInitFunc) gst_ffmpeg_sub_dec_base_init,
    NULL,
    (GClassInitFunc) gst_ffmpeg_sub_dec_class_init,
    NULL,
    NULL,
    sizeof (GstFFMpegSubDec),
    0,
    (GInstanceInitFunc) gst_ffmpeg_sub_dec_init,
  };
  GType type;
  AVCodec *in_plugin;
  gint rank;

  in_plugin = av_codec_next(NULL);

  GST_DEBUG ("[zspace] Registering ffmpeg subtitle decoders ************************");

  while (in_plugin) {
    gchar *type_name;

    /* only decoders */
    if (!av_codec_is_decoder (in_plugin)
        || in_plugin->type != AVMEDIA_TYPE_SUBTITLE) {
      goto next;
    }

    switch (in_plugin->id) {
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
    case AV_CODEC_ID_DVD_SUBTITLE:
    case AV_CODEC_ID_DVB_SUBTITLE:
    case AV_CODEC_ID_XSUB:
    case AV_CODEC_ID_TEXT:
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_SSA:
    case AV_CODEC_ID_SRT:
    case AV_CODEC_ID_SUBRIP:
      break;
    default:
      goto next;
    }

    GST_DEBUG ("Trying plugin %s [%s] id=%d ", in_plugin->name, in_plugin->long_name, in_plugin->id);

    /* construct the type */
    type_name = g_strdup_printf ("avdec_%s", in_plugin->name);
    g_strdelimit (type_name, ".,|-<> ", '_');

    type = g_type_from_name (type_name);

    if (!type) {
      /* create the gtype now */
      type =
          g_type_register_static (GST_TYPE_FFMPEG_SUB_DEC, type_name, &typeinfo,
          0);
      g_type_set_qdata (type, GST_FFDEC_PARAMS_QDATA, (gpointer) in_plugin);
    }

    switch(in_plugin->id){
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
    case AV_CODEC_ID_DVD_SUBTITLE:
      rank = GST_RANK_PRIMARY;
      break;
    default:
      rank = GST_RANK_PRIMARY;
    };

    if (!gst_element_register (plugin, type_name, rank, type)) {
      g_warning ("Failed to register %s", type_name);
      g_free (type_name);
      return FALSE;
    }

    g_free (type_name);

  next:
    in_plugin = av_codec_next (in_plugin);
  }

  GST_DEBUG ("Finished Registering subtitle decoders");

  return TRUE;
}
