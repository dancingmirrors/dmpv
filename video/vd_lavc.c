/*
 * This file is part of dmpv.
 *
 * dmpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * dmpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with dmpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include "misc/mp_assert.h"
#include "osdep/threads.h"
#include <stdbool.h>

#include <libavcodec/avcodec.h>
#include <libavformat/version.h>
#include <libavutil/common.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>

#include "misc/dmpv_talloc.h"
#include "common/global.h"
#include "common/msg.h"
#include "options/m_config_core.h"
#include "options/options.h"
#include "misc/bstr.h"
#include "common/av_common.h"
#include "common/codecs.h"

#include "video/fmt-conversion.h"

#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"
#include "video/hwdec.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"
#include "demux/demux.h"
#include "demux/stheader.h"
#include "demux/packet.h"
#include "demux/packet_pool.h"
#include "video/csputils.h"
#include "video/sws_utils.h"
#include "video/out/vo.h"

#include "options/m_option.h"

static void init_avctx(struct mp_filter *vd);
static void uninit_avctx(struct mp_filter *vd);

static int get_buffer2_direct(AVCodecContext *avctx, AVFrame *pic, int flags);
static enum AVPixelFormat get_format_hwdec(struct AVCodecContext *avctx,
                                           const enum AVPixelFormat *pix_fmt);

static int hwdec_opt_help(struct mp_log *log, const m_option_t *opt,
                          struct bstr name);

#define HWDEC_DELAY_QUEUE_COUNT 2

#define OPT_BASE_STRUCT struct vd_lavc_params

struct vd_lavc_params {
    bool fast;
    int film_grain;
    bool show_all;
    int skip_loop_filter;
    int skip_idct;
    int skip_frame;
    int framedrop;
    int threads;
    bool bitexact;
    bool old_x264;
    bool check_hw_profile;
    int software_fallback;
    char **avopts;
    int dr;
    char **hwdec_api;
    char *hwdec_codecs;
    int hwdec_image_format;
    int hwdec_extra_frames;
    int hwdec_threads;
    int hwdec_max_size;
};

static const struct m_opt_choice_alternatives discard_names[] = {
    {"none",        AVDISCARD_NONE},
    {"default",     AVDISCARD_DEFAULT},
    {"nonref",      AVDISCARD_NONREF},
    {"bidir",       AVDISCARD_BIDIR},
    {"nonkey",      AVDISCARD_NONKEY},
    {"all",         AVDISCARD_ALL},
    {0}
};
#define OPT_DISCARD(field) OPT_CHOICE_C(field, discard_names)

const struct m_sub_options vd_lavc_conf = {
    .opts = (const m_option_t[]){
        {"vd-lavc-fast", OPT_BOOL(fast)},
        {"vd-lavc-film-grain", OPT_CHOICE(film_grain,
            {"auto", -1}, {"cpu", 0}, {"gpu", 1})},
        {"vd-lavc-show-all", OPT_BOOL(show_all)},
        {"vd-lavc-skiploopfilter", OPT_DISCARD(skip_loop_filter)},
        {"vd-lavc-skipidct", OPT_DISCARD(skip_idct)},
        {"vd-lavc-skipframe", OPT_DISCARD(skip_frame)},
        {"vd-lavc-framedrop", OPT_DISCARD(framedrop)},
        {"vd-lavc-threads", OPT_INT(threads), M_RANGE(0, DBL_MAX)},
        {"vd-lavc-bitexact", OPT_BOOL(bitexact)},
        {"vd-lavc-assume-old-x264", OPT_BOOL(old_x264)},
        {"vd-lavc-check-hw-profile", OPT_BOOL(check_hw_profile)},
        {"hwdec-threads", OPT_INT(hwdec_threads), M_RANGE(0, DBL_MAX)},
        {"hwdec-max-size", OPT_INT(hwdec_max_size), M_RANGE(0, INT_MAX),
            .flags = UPDATE_HWDEC},
        {"vd-lavc-software-fallback", OPT_CHOICE(software_fallback,
            {"no", INT_MAX}, {"yes", 1}), M_RANGE(1, INT_MAX)},
        {"vd-lavc-o", OPT_KEYVALUELIST(avopts)},
        {"vd-lavc-dr", OPT_CHOICE(dr,
            {"auto", -1}, {"no", 0}, {"yes", 1})},
        {"dr", OPT_ALIAS("vd-lavc-dr")},
        {"hwdec", OPT_STRINGLIST(hwdec_api),
            .help = hwdec_opt_help,
            .flags = M_OPT_OPTIONAL_PARAM | M_OPT_ALLOW_NO | UPDATE_HWDEC},
        {"hwdec-codecs", OPT_STRING(hwdec_codecs)},
        {"hwdec-image-format", OPT_IMAGEFORMAT(hwdec_image_format)},
        {"hwdec-extra-frames", OPT_INT(hwdec_extra_frames), M_RANGE(0, 256)},
        {0}
    },
    .size = sizeof(struct vd_lavc_params),
    .defaults = &(const struct vd_lavc_params){
        .film_grain = -1 /*auto*/,
        .check_hw_profile = true,
        .software_fallback = 3,
        .skip_loop_filter = AVDISCARD_DEFAULT,
        .skip_idct = AVDISCARD_DEFAULT,
        .skip_frame = AVDISCARD_DEFAULT,
        .framedrop = AVDISCARD_NONREF,
        .dr = -1,
        .hwdec_api = (char *[]){"auto", NULL,},
        .hwdec_codecs = "h264,vc1,hevc,vp8,vp9,av1,prores,prores_raw,ffv1,dpx",
        // Maximum number of surfaces the player wants to buffer. This number
        // might require adjustment depending on whatever the player does;
        // for example, if a VO increases the number of reference surfaces for
        // interpolation, this value has to be increased too.
        .hwdec_extra_frames = 6,
        .hwdec_threads = 4,
        .hwdec_max_size = 4096,
    },
    .change_flags = UPDATE_VD,
};

struct hwdec_info {
    char name[64];
    char method_name[24]; // non-unique name describing the hwdec method
    const AVCodec *codec; // implemented by this codec
    enum AVHWDeviceType lavc_device; // if not NONE, get a hwdevice
    bool copying; // if true, outputs sw frames, or copy to sw ourselves
    enum AVPixelFormat pix_fmt; // if not NONE, select in get_format
    bool use_hw_frames; // set AVCodecContext.hw_frames_ctx
    bool use_hw_device; // set AVCodecContext.hw_device_ctx
    unsigned int flags; // HWDEC_FLAG_*

    // for internal sorting
    int auto_pos;
    int rank;
};

typedef struct lavc_ctx {
    struct mp_log *log;
    struct m_config_cache *opts_cache;
    struct vd_lavc_params *opts;
    struct mp_codec_params *codec;
    AVCodecContext *avctx;
    AVFrame *pic;
    AVPacket *avpkt;
    bool use_hwdec;
    struct hwdec_info hwdec; // valid only if use_hwdec==true
    bstr *attempted_hwdecs;
    int num_attempted_hwdecs;
    AVRational codec_timebase;
    enum AVDiscard skip_frame;
    bool flushing;
    struct lavc_state state;
    const char *decoder;
    bool hwdec_failed;
    bool hwdec_notified;
    bool force_eof;

    bool intra_only;
    int framedrop_flags;

    bool hw_probing;
    struct demux_packet **sent_packets;
    int num_sent_packets;

    struct demux_packet **requeue_packets;
    int num_requeue_packets;

    struct mp_image **delay_queue;
    int num_delay_queue;
    int max_delay_queue;

    // From VO
    struct vo *vo;
    struct mp_hwdec_devices *hwdec_devs;

    // Wrapped AVHWDeviceContext* used for decoding.
    AVBufferRef *hwdec_dev;

    bool hwdec_request_reinit;
    int hwdec_fail_count;

    struct mp_image_pool *hwdec_swpool;

    AVBufferRef *cached_hw_frames_ctx;

    // --- The following fields are protected by dr_lock.
    pthread_mutex_t dr_lock;
    bool dr_failed;
    struct mp_image_pool *dr_pool;
    int dr_imgfmt, dr_w, dr_h, dr_stride_align;

    struct mp_decoder public;
} vd_ffmpeg_ctx;

enum {
    HWDEC_FLAG_AUTO         = (1 << 0), // prioritize in autoprobe order
    HWDEC_FLAG_WHITELIST    = (1 << 1), // whitelist for auto-safe
};

struct autoprobe_info {
    const char *method_name;
    unsigned int flags;         // HWDEC_FLAG_*
};

const struct autoprobe_info hwdec_autoprobe_info[] = {
    {"vulkan",          HWDEC_FLAG_AUTO | HWDEC_FLAG_WHITELIST},
    {"vulkan-copy",     HWDEC_FLAG_AUTO | HWDEC_FLAG_WHITELIST},
    {"vaapi",           HWDEC_FLAG_AUTO | HWDEC_FLAG_WHITELIST},
    {"vaapi-copy",      HWDEC_FLAG_AUTO | HWDEC_FLAG_WHITELIST},
    {"vdpau-copy",      HWDEC_FLAG_AUTO | HWDEC_FLAG_WHITELIST},
    {"vdpau",           HWDEC_FLAG_AUTO},
    {"no",              HWDEC_FLAG_AUTO | HWDEC_FLAG_WHITELIST},
    {0}
};

static int hwdec_compare(const void *p1, const void *p2)
{
    struct hwdec_info *h1 = (void *)p1;
    struct hwdec_info *h2 = (void *)p2;

    if (h1 == h2)
        return 0;

    // Strictly put non-preferred hwdecs to the end of the list.
    if ((h1->auto_pos == INT_MAX) != (h2->auto_pos == INT_MAX))
        return h1->auto_pos == INT_MAX ? 1 : -1;
    // List non-copying entries first, so --hwdec=auto takes them.
    if (h1->copying != h2->copying)
        return h1->copying ? 1 : -1;
    // Order by autoprobe preference order.
    if (h1->auto_pos != h2->auto_pos)
        return h1->auto_pos > h2->auto_pos ? 1 : -1;
    // Put hwdecs without hw_device_ctx last
    if ((!!h1->lavc_device) != (!!h2->lavc_device))
        return h1->lavc_device ? -1 : 1;
    // Fallback sort order to make sorting stable.
    return h1->rank > h2->rank ? 1 :-1;
}

// (This takes care of some bookkeeping too, like setting info.name)
static void add_hwdec_item(struct hwdec_info **infos, int *num_infos,
                           struct hwdec_info info)
{
    if (info.copying)
        mp_snprintf_cat(info.method_name, sizeof(info.method_name), "-copy");

    // (Including the codec name in case this is a wrapper looks pretty dumb,
    // but better not have them clash with hwaccels and others.)
    snprintf(info.name, sizeof(info.name), "%s-%s",
             info.codec->name, info.method_name);

    info.rank = *num_infos;
    info.auto_pos = INT_MAX;

    for (int x = 0; hwdec_autoprobe_info[x].method_name; x++) {
        const struct autoprobe_info *entry = &hwdec_autoprobe_info[x];
        if (strcmp(entry->method_name, info.method_name) == 0) {
            info.flags |= entry->flags;
            if (info.flags & HWDEC_FLAG_AUTO)
                info.auto_pos = x;
        }
    }

    MP_TARRAY_APPEND(NULL, *infos, *num_infos, info);
}

static void add_all_hwdec_methods(struct hwdec_info **infos, int *num_infos)
{
    const AVCodec *codec = NULL;
    void *iter = NULL;
    while (1) {
        codec = av_codec_iterate(&iter);
        if (!codec)
            break;
        if (codec->type != AVMEDIA_TYPE_VIDEO || !av_codec_is_decoder(codec))
            continue;

        struct hwdec_info info_template = {
            .pix_fmt = AV_PIX_FMT_NONE,
            .codec = codec,
        };

        const char *wrapper = NULL;
        if (codec->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID))
            wrapper = codec->wrapper_name;

        // A decoder can provide multiple methods. In particular, hwaccels
        // provide various methods (e.g. native h264 with VA-API), but
        // even wrapper decoders could provide multiple methods.
        bool found_any = false;
        for (int n = 0; ; n++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, n);
            if (!cfg)
                break;

            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) ||
                (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            {
                struct hwdec_info info = info_template;
                info.lavc_device = cfg->device_type;
                info.pix_fmt = cfg->pix_fmt;

                const char *name = av_hwdevice_get_type_name(cfg->device_type);
                mp_assert(name); // API violation by libavcodec

                snprintf(info.method_name, sizeof(info.method_name), "%s", name);

                // Usually we want to prefer using hw_frames_ctx for true
                // hwaccels only, but we actually don't have any way to detect
                // those, so always use hw_frames_ctx if offered.
                if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) {
                    info.use_hw_frames = true;
                } else {
                    info.use_hw_device = true;
                }

                // Direct variant.
                add_hwdec_item(infos, num_infos, info);

                // Copy variant.
                info.copying = true;
                if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
                    info.use_hw_frames = false;
                    info.use_hw_device = true;
                }
                add_hwdec_item(infos, num_infos, info);

                found_any = true;
            } else if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL) {
                struct hwdec_info info = info_template;
                info.pix_fmt = cfg->pix_fmt;

                const char *name = wrapper;
                if (!name)
                    name = av_get_pix_fmt_name(info.pix_fmt);
                mp_assert(name); // API violation by libavcodec

                snprintf(info.method_name, sizeof(info.method_name), "%s", name);

                // Direct variant.
                add_hwdec_item(infos, num_infos, info);

                // Copy variant.
                info.copying = true;
                info.pix_fmt = AV_PIX_FMT_NONE; // trust it can do sw output
                add_hwdec_item(infos, num_infos, info);

                found_any = true;
            }
        }

        if (!found_any && wrapper) {
            // We _know_ there's something supported here, usually outputting
            // sw surfaces.

            struct hwdec_info info = info_template;
            info.copying = true; // probably

            snprintf(info.method_name, sizeof(info.method_name), "%s", wrapper);
            add_hwdec_item(infos, num_infos, info);
        }
    }

    qsort(*infos, *num_infos, sizeof(struct hwdec_info), hwdec_compare);
}

static bool hwdec_codec_allowed(struct mp_filter *vd, const char *codec)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    bstr s = bstr0(ctx->opts->hwdec_codecs);
    while (s.len) {
        bstr item;
        bstr_split_tok(s, ",", &item, &s);
        if (bstr_equals0(item, "all") || bstr_equals0(item, codec))
            return true;
    }
    return false;
}

static AVBufferRef *hwdec_create_dev(struct mp_filter *vd,
                                     struct hwdec_info *hwdec,
                                     bool autoprobe)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    mp_assert(hwdec->lavc_device);

    if (hwdec->copying) {
        const struct hwcontext_fns *fns =
            hwdec_get_hwcontext_fns(hwdec->lavc_device);
        if (fns && fns->create_dev) {
            struct hwcontext_create_dev_params params = {
                .probing = autoprobe,
            };
            return fns->create_dev(vd->global, vd->log, &params);
        } else {
            AVBufferRef* ref = NULL;
            av_hwdevice_ctx_create(&ref, hwdec->lavc_device, NULL, NULL, 0);
            return ref;
        }
    } else if (ctx->hwdec_devs) {
        int imgfmt = pixfmt2imgfmt(hwdec->pix_fmt);
        struct hwdec_imgfmt_request params = {
            .imgfmt = imgfmt,
            .probing = autoprobe,
        };
        hwdec_devices_request_for_img_fmt(ctx->hwdec_devs, &params);

        const struct mp_hwdec_ctx *hw_ctx =
            hwdec_devices_get_by_imgfmt_and_type(ctx->hwdec_devs, imgfmt,
                                                 hwdec->lavc_device);

        if (hw_ctx && hw_ctx->av_device_ref)
            return av_buffer_ref(hw_ctx->av_device_ref);
    }

    return NULL;
}

// Select if and which hwdec to use. Also makes sure to get the decode device.
static void select_and_set_hwdec(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    const char *codec = ctx->codec->codec;

    m_config_cache_update(ctx->opts_cache);

    struct hwdec_info *hwdecs = NULL;
    int num_hwdecs = 0;
    add_all_hwdec_methods(&hwdecs, &num_hwdecs);

    // Check if video filters (vf) are in use
    struct m_obj_settings *vf_settings = NULL;
    mp_read_option_raw(vd->global, "vf", &m_option_type_obj_settings_list,
                       &vf_settings);
    bool vf_in_use = vf_settings && vf_settings[0].name;
    bool vf_requested_copy = false;

    char **hwdec_api = ctx->opts->hwdec_api;
    for (int i = 0; hwdec_api && hwdec_api[i]; i++) {
        bstr opt = bstr0(hwdec_api[i]);

        bool hwdec_requested = !bstr_equals0(opt, "no");
        bool hwdec_auto_all = bstr_equals0(opt, "auto") ||
                            bstr_equals0(opt, "");
        bool hwdec_auto_safe = bstr_equals0(opt, "auto-safe") ||
                            bstr_equals0(opt, "auto-copy-safe") ||
                            bstr_equals0(opt, "yes");
        bool hwdec_auto_copy = bstr_equals0(opt, "auto-copy") ||
                            bstr_equals0(opt, "auto-copy-safe");
        bool hwdec_auto = hwdec_auto_all || hwdec_auto_copy || hwdec_auto_safe;

        if (!hwdec_requested) {
            MP_VERBOSE(vd, "No hardware decoding requested.\n");
            break;
        } else if (!hwdec_codec_allowed(vd, codec)) {
            MP_VERBOSE(vd, "Not trying to use hardware decoding: codec %s is not "
                    "on whitelist.\n", codec);
            break;
        } else if (ctx->opts->hwdec_max_size > 0 &&
                   (ctx->codec->disp_w > ctx->opts->hwdec_max_size ||
                    ctx->codec->disp_h > ctx->opts->hwdec_max_size)) {
            MP_INFO(vd, "Not trying to use hardware decoding: "
                           "video resolution %dx%d exceeds maximum %d.\n",
                       ctx->codec->disp_w, ctx->codec->disp_h,
                       ctx->opts->hwdec_max_size);
            break;
        } else {
            bool hwdec_name_supported = false; // relevant only if !hwdec_auto
            for (int n = 0; n < num_hwdecs; n++) {
                struct hwdec_info *hwdec = &hwdecs[n];

                // Match hwdec by name
                bool name_matches = bstr_equals0(opt, hwdec->method_name) ||
                                   bstr_equals0(opt, hwdec->name);

                // When vf is in use and user specified an explicit hwdec,
                // also match the copy variant (e.g., "vaapi" matches "vaapi-copy")
                if (!hwdec_auto && vf_in_use && hwdec->copying) {
                    // Check if the requested name matches this hwdec's method_name
                    // without the "-copy" suffix
                    bstr method = bstr0(hwdec->method_name);
                    bstr copy_suffix = bstr0("-copy");
                    if (bstr_endswith(method, copy_suffix)) {
                        bstr base_method = bstr_splice(method, 0, method.len - copy_suffix.len);
                        if (bstr_equals(opt, base_method))
                            name_matches = true;
                    }
                }

                if (!hwdec_auto && !name_matches)
                    continue;
                hwdec_name_supported = true;

                bool already_attempted = false;
                for (int j = 0; j < ctx->num_attempted_hwdecs; j++) {
                    if (bstr_equals0(ctx->attempted_hwdecs[j], hwdec->name)) {
                        MP_DBG(vd, "Skipping previously attempted hwdec: %s\n",
                               hwdec->name);
                        already_attempted = true;
                        break;
                    }
                }
                if (already_attempted)
                    continue;

                const char *hw_codec = mp_codec_from_av_codec_id(hwdec->codec->id);
                if (!hw_codec || strcmp(hw_codec, codec) != 0)
                    continue;

                if (hwdec_auto && !(hwdec->flags & HWDEC_FLAG_AUTO)) {
                    continue;
                }

                if (hwdec_auto_safe && !(hwdec->flags & HWDEC_FLAG_WHITELIST))
                    continue;

                MP_VERBOSE(vd, "Looking at hwdec %s...\n", hwdec->name);

                /*
                 * Past this point, any kind of failure that results in us
                 * looking for a new hwdec should not lead to use trying this
                 * hwdec again - so add it to the list, regardless of whether
                 * initialisation will succeed or not.
                 */
                MP_TARRAY_APPEND(ctx, ctx->attempted_hwdecs,
                                 ctx->num_attempted_hwdecs,
                                 bstrdup(ctx, bstr0(hwdec->name)));

                if (hwdec_auto_copy && !hwdec->copying) {
                    MP_VERBOSE(vd, "Not using this for auto-copy.\n");
                    continue;
                }

                // When video filters are in use, we need the -copy variant
                // to avoid filter graph creation issues
                if (vf_in_use && !hwdec->copying) {
                    if (!vf_requested_copy) {
                        MP_INFO(vd, "INFO: Video filters are active, switching to %s-copy for compatibility.\n",
                                hwdec->method_name);
                        vf_requested_copy = true;
                    }
                    MP_VERBOSE(vd, "Skipping non-copy hwdec due to video filters.\n");
                    continue;
                }

                if (hwdec->lavc_device) {
                    ctx->hwdec_dev = hwdec_create_dev(vd, hwdec, hwdec_auto);
                    if (!ctx->hwdec_dev) {
                        MP_VERBOSE(vd, "Could not create device.\n");
                        continue;
                    }

                    const struct hwcontext_fns *fns =
                                hwdec_get_hwcontext_fns(hwdec->lavc_device);
                    if (fns && fns->is_emulated && fns->is_emulated(ctx->hwdec_dev)) {
                        if (hwdec_auto) {
                            MP_VERBOSE(vd, "Not using emulated API.\n");
                            av_buffer_unref(&ctx->hwdec_dev);
                            continue;
                        }
                        MP_VERBOSE(vd, "Using emulated hardware decoding API.\n");
                    }
                } else if (!hwdec->copying) {
                    // Most likely METHOD_INTERNAL, which often use delay-loaded
                    // VO support as well.
                    if (ctx->hwdec_devs) {
                        struct hwdec_imgfmt_request params = {
                            .imgfmt = pixfmt2imgfmt(hwdec->pix_fmt),
                            .probing = hwdec_auto,
                        };
                        hwdec_devices_request_for_img_fmt(
                            ctx->hwdec_devs, &params);
                    }
                }

                ctx->use_hwdec = true;
                ctx->hwdec = *hwdec;
                break;
            }
            if (ctx->use_hwdec)
                break;
            else if (!hwdec_auto && !hwdec_name_supported)
                MP_WARN(vd, "Unsupported hwdec: %.*s\n", BSTR_P(opt));
        }
    }
    talloc_free(hwdecs);


    if (ctx->use_hwdec) {
        MP_VERBOSE(vd, "Trying hardware decoding via %s.\n", ctx->hwdec.name);
        if (strcmp(ctx->decoder, ctx->hwdec.codec->name) != 0)
            MP_VERBOSE(vd, "Using underlying hw-decoder '%s'\n",
                       ctx->hwdec.codec->name);
    } else {
        // If software fallback is disabled and we get here, all hwdec must
        // have failed. Tell the ctx to always force an eof.
        if (ctx->opts->software_fallback == INT_MAX) {
            MP_WARN(ctx, "Software decoding fallback is disabled.\n");
            ctx->force_eof = true;
        } else {
            MP_VERBOSE(vd, "Using software decoding.\n");
        }
    }
}

static int hwdec_opt_help(struct mp_log *log, const m_option_t *opt,
                          struct bstr name)
{
    mp_info(log, "Supported values (if enabled in FFmpeg):\n");
    mp_info(log, "    auto (yes '')\n");
    #if HAVE_HWDEC_VULKAN
    mp_info(log, "    vulkan\n");
    mp_info(log, "    vulkan-copy\n");
    #endif
    #if HAVE_VAAPI
    mp_info(log, "    vaapi\n");
    mp_info(log, "    vaapi-copy\n");
    #endif
    #if HAVE_VDPAU
    mp_info(log, "    vdpau-copy\n");
    mp_info(log, "    vdpau\n");
    #endif
    mp_info(log, "    no\n");
    mp_info(log, "    auto-safe\n");
    mp_info(log, "    auto-copy\n");
    mp_info(log, "    auto-copy-safe\n");

    return M_OPT_EXIT;
}

static void force_fallback(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    uninit_avctx(vd);
    int lev = ctx->hwdec_notified ? MSGL_WARN : MSGL_V;
    mp_msg(vd->log, lev, "Attempting next decoding method after failure of %.*s.\n",
           BSTR_P(ctx->attempted_hwdecs[ctx->num_attempted_hwdecs - 1]));
    select_and_set_hwdec(vd);
    init_avctx(vd);
}

static void reinit(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    uninit_avctx(vd);

    /*
     * Reset attempted hwdecs so that if the hwdec list is reconfigured
     * we attempt all of them from the beginning. The most practical
     * reason for this is that ctrl+h toggles between `no` and
     * `auto-safe`, and we want to reevaluate from a clean slate each time.
     */
    TA_FREEP(&ctx->attempted_hwdecs);
    ctx->num_attempted_hwdecs = 0;
    ctx->hwdec_notified = false;

    select_and_set_hwdec(vd);

    bool use_hwdec = ctx->use_hwdec;
    init_avctx(vd);
    if (!ctx->avctx && use_hwdec) {
        do {
            force_fallback(vd);
        } while (!ctx->avctx);
    }
}

static void init_avctx(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    struct vd_lavc_params *lavc_param = ctx->opts;
    struct mp_codec_params *c = ctx->codec;

    m_config_cache_update(ctx->opts_cache);

    mp_assert(!ctx->avctx);

    const AVCodec *lavc_codec = NULL;

    if (ctx->use_hwdec) {
        lavc_codec = ctx->hwdec.codec;
        MP_VERBOSE(vd, "FFmpeg: Using hwdec codec '%s' for %s\n",
                   lavc_codec ? lavc_codec->name : "NULL",
                   ctx->decoder);
    } else {
        lavc_codec = avcodec_find_decoder_by_name(ctx->decoder);
        MP_VERBOSE(vd, "FFmpeg: Using software decoder '%s'\n", ctx->decoder);
    }
    if (!lavc_codec) {
        MP_VERBOSE(vd, "FFmpeg: Failed to find codec '%s'\n", ctx->decoder);
        return;
    }

    const AVCodecDescriptor *desc = avcodec_descriptor_get(lavc_codec->id);
    ctx->intra_only = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);

    MP_VERBOSE(vd, "FFmpeg: Codec '%s' (ID=%d) %s intra-only\n",
               lavc_codec->name, lavc_codec->id,
               ctx->intra_only ? "is" : "is not");

    ctx->codec_timebase = mp_get_codec_timebase(ctx->codec);

    ctx->hwdec_failed = false;
    ctx->hwdec_request_reinit = false;
    ctx->avctx = avcodec_alloc_context3(lavc_codec);
    AVCodecContext *avctx = ctx->avctx;
    if (!ctx->avctx) {
        MP_VERBOSE(vd, "FFmpeg: Failed to allocate codec context\n");
        goto error;
    }
    avctx->codec_type = AVMEDIA_TYPE_VIDEO;
    avctx->codec_id = lavc_codec->id;
    avctx->pkt_timebase = ctx->codec_timebase;

    MP_VERBOSE(vd, "FFmpeg: Codec context allocated, timebase=%d/%d\n",
               ctx->codec_timebase.num, ctx->codec_timebase.den);

    ctx->pic = av_frame_alloc();
    if (!ctx->pic)
        goto error;

    ctx->avpkt = av_packet_alloc();
    if (!ctx->avpkt)
        goto error;

    int threads = lavc_param->threads;
    if (ctx->use_hwdec) {
        MP_VERBOSE(vd, "FFmpeg: Configuring hardware decoding\n");
        avctx->opaque = vd;
        avctx->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
        if (!lavc_param->check_hw_profile) {
            avctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
            MP_VERBOSE(vd, "FFmpeg: Allowing hwdec profile mismatch\n");
        }

#ifdef AV_HWACCEL_FLAG_UNSAFE_OUTPUT
        avctx->hwaccel_flags |= AV_HWACCEL_FLAG_UNSAFE_OUTPUT;
        MP_VERBOSE(vd, "FFmpeg: Using unsafe output flag for hwdec\n");
#endif

        if (ctx->hwdec.use_hw_device) {
            if (ctx->hwdec_dev) {
                avctx->hw_device_ctx = av_buffer_ref(ctx->hwdec_dev);
                MP_VERBOSE(vd, "FFmpeg: Using hardware device context\n");
            }
            if (!avctx->hw_device_ctx) {
                MP_VERBOSE(vd, "FFmpeg: Failed to reference hw_device_ctx\n");
                goto error;
            }
        }
        if (ctx->hwdec.use_hw_frames) {
            if (!ctx->hwdec_dev) {
                MP_VERBOSE(vd, "FFmpeg: hwdec requires hw_frames but no device\n");
                goto error;
            }
            MP_VERBOSE(vd, "FFmpeg: Using hardware frames\n");
        }

        if (ctx->hwdec.pix_fmt != AV_PIX_FMT_NONE) {
            avctx->get_format = get_format_hwdec;
            MP_VERBOSE(vd, "FFmpeg: Using hwdec pixel format callback\n");
        }

        // Some APIs benefit from this, for others it's additional bloat.
        if (ctx->hwdec.copying) {
            ctx->max_delay_queue = HWDEC_DELAY_QUEUE_COUNT;
            MP_VERBOSE(vd, "FFmpeg: Hwdec is copy-back, setting delay queue to %d\n",
                       HWDEC_DELAY_QUEUE_COUNT);
        }
        ctx->hw_probing = true;

        threads = ctx->opts->hwdec_threads;
        MP_VERBOSE(vd, "FFmpeg: Using %d threads for hwdec\n", threads);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(62, 11, 100)
        // Vulkan threading was not safe before 62.11.100
        bstr hwdec_name = bstr0(ctx->hwdec.name);
        if (bstr_endswith0(hwdec_name, "vulkan") || bstr_endswith0(hwdec_name, "vulkan-copy")) {
            threads = 1;
            MP_VERBOSE(vd, "FFmpeg: Limiting Vulkan hwdec to 1 thread (old FFmpeg)\n");
        }
#endif
    }

    mp_set_avcodec_threads(vd->log, avctx, threads);

    if (!ctx->use_hwdec && ctx->vo && lavc_param->dr) {
        avctx->opaque = vd;
        avctx->get_buffer2 = get_buffer2_direct;
        MP_VERBOSE(vd, "FFmpeg: Using direct rendering (DR)\n");
    }

    avctx->flags |= lavc_param->bitexact ? AV_CODEC_FLAG_BITEXACT : 0;
    avctx->flags2 |= lavc_param->fast ? AV_CODEC_FLAG2_FAST : 0;

    if (lavc_param->show_all)
        avctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;

    avctx->skip_loop_filter = lavc_param->skip_loop_filter;
    avctx->skip_idct = lavc_param->skip_idct;
    avctx->skip_frame = lavc_param->skip_frame;

    if (lavc_codec->id == AV_CODEC_ID_H264 && lavc_param->old_x264)
        av_opt_set(avctx, "x264_build", "150", AV_OPT_SEARCH_CHILDREN);

#ifndef AV_CODEC_EXPORT_DATA_FILM_GRAIN
    if (ctx->opts->film_grain == 1)
        MP_WARN(vd, "GPU film grain requested, but FFmpeg too old to expose "
                    "film grain parameters. Please update to latest master, "
                    "or at least to release 4.4.\n");
#else
    switch(ctx->opts->film_grain) {
    case 0: /*CPU*/
        // default lavc flags handle film grain within the decoder.
        break;
    case 1: /*GPU*/
        if (!ctx->vo ||
            (ctx->vo && !(ctx->vo->driver->caps & VO_CAP_FILM_GRAIN))) {
            MP_MSG(vd, ctx->vo ? MSGL_WARN : MSGL_V,
                   "GPU film grain requested, but VO %s, expect wrong output.\n",
                   ctx->vo ?
                   "does not support applying film grain" :
                   "is not available at decoder initialization to verify support");
        }

        avctx->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;
        break;
    default:
        if (ctx->vo && (ctx->vo->driver->caps & VO_CAP_FILM_GRAIN))
            avctx->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;

        break;
    }
#endif

    mp_set_avopts(vd->log, avctx, lavc_param->avopts);

    // Do this after the above avopt handling in case it changes values
    ctx->skip_frame = avctx->skip_frame;

    if (mp_set_avctx_codec_headers(avctx, c) < 0) {
        MP_ERR(vd, "Could not set codec parameters.\n");
        goto error;
    }

    /* open it */
    MP_VERBOSE(vd, "FFmpeg: Opening codec '%s'\n", lavc_codec->name);
    if (avcodec_open2(avctx, lavc_codec, NULL) < 0) {
        MP_VERBOSE(vd, "FFmpeg: Failed to open codec\n");
        goto error;
    }
    MP_VERBOSE(vd, "FFmpeg: Codec opened successfully\n");

    // Sometimes, the first packet contains information required for correct
    // decoding of the rest of the stream. The only currently known case is the
    // x264 build number (encoded in a SEI element), needed to enable a
    // workaround for broken 4:4:4 streams produced by older x264 versions.
    if (lavc_codec->id == AV_CODEC_ID_H264 && c->first_packet) {
        mp_set_av_packet(ctx->avpkt, c->first_packet, &ctx->codec_timebase);
        avcodec_send_packet(avctx, ctx->avpkt);
        avcodec_receive_frame(avctx, ctx->pic);
        av_frame_unref(ctx->pic);
        avcodec_flush_buffers(ctx->avctx);
    }

    return;

error:
    MP_ERR(vd, "Could not open codec.\n");
    uninit_avctx(vd);
}

static void reset_avctx(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    if (ctx->avctx && avcodec_is_open(ctx->avctx))
        avcodec_flush_buffers(ctx->avctx);
    ctx->flushing = false;
    ctx->hwdec_request_reinit = false;
}

static void flush_all(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    for (int n = 0; n < ctx->num_delay_queue; n++)
        talloc_free(ctx->delay_queue[n]);
    ctx->num_delay_queue = 0;

    for (int n = 0; n < ctx->num_sent_packets; n++)
        talloc_free(ctx->sent_packets[n]);
    ctx->num_sent_packets = 0;

    for (int n = 0; n < ctx->num_requeue_packets; n++)
        talloc_free(ctx->requeue_packets[n]);
    ctx->num_requeue_packets = 0;

    reset_avctx(vd);
}

static void uninit_avctx(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    flush_all(vd);
    av_frame_free(&ctx->pic);
    mp_free_av_packet(&ctx->avpkt);
    av_buffer_unref(&ctx->cached_hw_frames_ctx);

    avcodec_free_context(&ctx->avctx);

    av_buffer_unref(&ctx->hwdec_dev);

    ctx->hwdec_failed = false;
    ctx->hwdec_fail_count = 0;
    ctx->max_delay_queue = 0;
    ctx->hw_probing = false;
    ctx->hwdec = (struct hwdec_info){0};
    ctx->use_hwdec = false;
}

static int init_generic_hwaccel(struct AVCodecContext *avctx, enum AVPixelFormat hw_fmt)
{
    struct mp_filter *vd = avctx->opaque;
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVBufferRef *new_frames_ctx = NULL;

    MP_VERBOSE(vd, "FFmpeg: Initializing hwaccel for format %s\n",
               av_get_pix_fmt_name(hw_fmt));

    if (!ctx->hwdec.use_hw_frames) {
        MP_VERBOSE(vd, "FFmpeg: hwdec does not use hw_frames\n");
        return 0;
    }

    if (!ctx->hwdec_dev) {
        MP_ERR(ctx, "Missing device context.\n");
        goto error;
    }

    if (avcodec_get_hw_frames_parameters(avctx,
                                ctx->hwdec_dev, hw_fmt, &new_frames_ctx) < 0)
    {
        MP_VERBOSE(ctx, "Hardware decoding of this stream is unsupported?\n");
        goto error;
    }

    AVHWFramesContext *new_fctx = (void *)new_frames_ctx->data;

    MP_VERBOSE(vd, "FFmpeg: HW frames context: format=%s sw_format=%s size=%dx%d pool=%d\n",
               av_get_pix_fmt_name(new_fctx->format),
               av_get_pix_fmt_name(new_fctx->sw_format),
               new_fctx->width, new_fctx->height,
               new_fctx->initial_pool_size);

    if (ctx->opts->hwdec_image_format) {
        new_fctx->sw_format = imgfmt2pixfmt(ctx->opts->hwdec_image_format);
        MP_VERBOSE(vd, "FFmpeg: Overriding sw_format to %s\n",
                   av_get_pix_fmt_name(new_fctx->sw_format));
    }

    // 1 surface is already included by libavcodec. The field is 0 if the
    // hwaccel supports dynamic surface allocation.
    if (new_fctx->initial_pool_size) {
        int old_size = new_fctx->initial_pool_size;
        new_fctx->initial_pool_size += ctx->opts->hwdec_extra_frames - 1;
        MP_VERBOSE(vd, "FFmpeg: Adjusting pool size from %d to %d (+%d extra)\n",
                   old_size, new_fctx->initial_pool_size,
                   ctx->opts->hwdec_extra_frames - 1);
    }

    const struct hwcontext_fns *fns =
        hwdec_get_hwcontext_fns(new_fctx->device_ctx->type);

    if (fns && fns->refine_hwframes) {
        MP_VERBOSE(vd, "FFmpeg: Refining hwframes with device-specific callback\n");
        fns->refine_hwframes(new_frames_ctx);
    }

    // We might be able to reuse a previously allocated frame pool.
    if (ctx->cached_hw_frames_ctx) {
        AVHWFramesContext *old_fctx = (void *)ctx->cached_hw_frames_ctx->data;

        if (new_fctx->format            != old_fctx->format ||
            new_fctx->sw_format         != old_fctx->sw_format ||
            new_fctx->width             != old_fctx->width ||
            new_fctx->height            != old_fctx->height ||
            new_fctx->initial_pool_size != old_fctx->initial_pool_size) {
            MP_VERBOSE(vd, "FFmpeg: HW frames context changed, releasing cached context\n");
            av_buffer_unref(&ctx->cached_hw_frames_ctx);
        } else {
            MP_VERBOSE(vd, "FFmpeg: Reusing cached HW frames context\n");
        }
    }

    if (!ctx->cached_hw_frames_ctx) {
        MP_VERBOSE(vd, "FFmpeg: Initializing new HW frames context\n");
        if (av_hwframe_ctx_init(new_frames_ctx) < 0) {
            MP_ERR(ctx, "Failed to allocate hw frames.\n");
            goto error;
        }

        ctx->cached_hw_frames_ctx = new_frames_ctx;
        new_frames_ctx = NULL;
    }

    avctx->hw_frames_ctx = av_buffer_ref(ctx->cached_hw_frames_ctx);
    if (!avctx->hw_frames_ctx) {
        MP_VERBOSE(vd, "FFmpeg: Failed to reference HW frames context\n");
        goto error;
    }

    MP_VERBOSE(vd, "FFmpeg: HW frames context configured successfully\n");

    av_buffer_unref(&new_frames_ctx);
    return 0;

error:
    MP_VERBOSE(vd, "FFmpeg: Failed to initialize generic hwaccel\n");
    av_buffer_unref(&new_frames_ctx);
    av_buffer_unref(&ctx->cached_hw_frames_ctx);
    return -1;
}

static enum AVPixelFormat get_format_hwdec(struct AVCodecContext *avctx,
                                           const enum AVPixelFormat *fmt)
{
    struct mp_filter *vd = avctx->opaque;
    vd_ffmpeg_ctx *ctx = vd->priv;

    MP_VERBOSE(vd, "Pixel formats supported by decoder:");
    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; i++)
        MP_VERBOSE(vd, " %s", av_get_pix_fmt_name(fmt[i]));
    MP_VERBOSE(vd, "\n");

    const char *profile = avcodec_profile_name(avctx->codec_id, avctx->profile);
    MP_VERBOSE(vd, "Codec profile: %s (0x%x)\n", profile ? profile : "unknown",
               avctx->profile);

    mp_assert(ctx->use_hwdec);

    ctx->hwdec_request_reinit |= ctx->hwdec_failed;
    ctx->hwdec_failed = false;

    enum AVPixelFormat select = AV_PIX_FMT_NONE;
    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; i++) {
        if (ctx->hwdec.pix_fmt == fmt[i]) {
            if (init_generic_hwaccel(avctx, fmt[i]) < 0)
                break;
            select = fmt[i];
            break;
        }
    }

    if (select == AV_PIX_FMT_NONE)
        ctx->hwdec_failed = true;

    const char *name = av_get_pix_fmt_name(select);
    MP_VERBOSE(vd, "Requesting pixfmt '%s' from decoder.\n", name ? name : "-");
    return select;
}

static int get_buffer2_direct(AVCodecContext *avctx, AVFrame *pic, int flags)
{
    struct mp_filter *vd = avctx->opaque;
    vd_ffmpeg_ctx *p = vd->priv;

    mp_mutex_lock(&p->dr_lock);

    int w = pic->width;
    int h = pic->height;
    int linesize_align[AV_NUM_DATA_POINTERS] = {0};
    avcodec_align_dimensions2(avctx, &w, &h, linesize_align);

    // We assume that different alignments are just different power-of-2s.
    // Thus, a higher alignment always satisfies a lower alignment.
    int stride_align = MP_IMAGE_BYTE_ALIGN;
    for (int n = 0; n < AV_NUM_DATA_POINTERS; n++)
        stride_align = MPMAX(stride_align, linesize_align[n]);

    // Note: texel sizes may be NPOT, so use full lcm instead of max
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pic->format);
    if (!(desc->flags & AV_PIX_FMT_FLAG_BITSTREAM)) {
        for (int n = 0; n < desc->nb_components; n++)
            stride_align = mp_lcm(stride_align, desc->comp[n].step);
    }

    int imgfmt = pixfmt2imgfmt(pic->format);
    if (!imgfmt)
        goto fallback;

    if (p->dr_failed)
        goto fallback;

    // (For simplicity, we realloc on any parameter change, instead of trying
    // to be clever.)
    if (unlikely(stride_align != p->dr_stride_align || w != p->dr_w || h != p->dr_h ||
        imgfmt != p->dr_imgfmt))
    {
        mp_image_pool_clear(p->dr_pool);
        p->dr_imgfmt = imgfmt;
        p->dr_w = w;
        p->dr_h = h;
        p->dr_stride_align = stride_align;
        MP_DBG(p, "DR parameter change to %dx%d %s align=%d\n", w, h,
               mp_imgfmt_to_name(imgfmt), stride_align);
    }

    struct mp_image *img = mp_image_pool_get_no_alloc(p->dr_pool, imgfmt, w, h);
    if (!img) {
        bool host_cached = p->opts->dr == -1; // auto
        int dr_flags = host_cached ? VO_DR_FLAG_HOST_CACHED : 0;
        MP_DBG(p, "Allocating new%s DR image...\n", host_cached ? " (host-cached)" : "");
        img = vo_get_image(p->vo, imgfmt, w, h, stride_align, dr_flags);
        if (!img) {
            MP_DBG(p, "...failed..\n");
            goto fallback;
        }

        // Now make the mp_image part of the pool. This requires doing magic to
        // the image, so just add it to the pool and get it back to avoid
        // dealing with magic ourselves. (Normally this never fails.)
        mp_image_pool_add(p->dr_pool, img);
        img = mp_image_pool_get_no_alloc(p->dr_pool, imgfmt, w, h);
        if (!img)
            goto fallback;
    }

    // get_buffer2 callers seem very unappreciative of overwriting pic with a
    // new reference. The AVCodecContext.get_buffer2 comments tell us exactly
    // what we should do, so follow that.
    for (int n = 0; n < 4; n++) {
        pic->data[n] = img->planes[n];
        pic->linesize[n] = img->stride[n];
        pic->buf[n] = img->bufs[n];
        img->bufs[n] = NULL;
    }
    talloc_free(img);

    mp_mutex_unlock(&p->dr_lock);

    return 0;

fallback:
    if (!p->dr_failed)
        MP_VERBOSE(p, "DR failed - disabling.\n");
    p->dr_failed = true;
    mp_mutex_unlock(&p->dr_lock);

    return avcodec_default_get_buffer2(avctx, pic, flags);
}

static void prepare_decoding(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVCodecContext *avctx = ctx->avctx;
    struct vd_lavc_params *opts = ctx->opts;

    if (!avctx)
        return;

    int drop = ctx->framedrop_flags;
    if (drop == 1) {
        avctx->skip_frame = opts->framedrop;    // normal framedrop
    } else if (drop == 2) {
        avctx->skip_frame = AVDISCARD_NONREF;   // hr-seek framedrop
        // Can be much more aggressive for true intra codecs.
        if (ctx->intra_only)
            avctx->skip_frame = AVDISCARD_ALL;
    } else {
        avctx->skip_frame = ctx->skip_frame;    // normal playback
    }

    if (ctx->hwdec_request_reinit)
        reset_avctx(vd);
}

static void handle_err(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    struct vd_lavc_params *opts = ctx->opts;

    MP_WARN(vd, "Error while decoding frame%s!\n",
            ctx->use_hwdec ? " (hardware decoding)" : "");

    if (ctx->use_hwdec) {
        ctx->hwdec_fail_count += 1;
        if (ctx->hwdec_fail_count >= opts->software_fallback)
            ctx->hwdec_failed = true;
    }
}

static int send_packet(struct mp_filter *vd, struct demux_packet *pkt)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVCodecContext *avctx = ctx->avctx;

    if (ctx->num_requeue_packets && ctx->requeue_packets[0] != pkt)
        return AVERROR(EAGAIN); // cannot consume the packet

    if (ctx->hwdec_failed)
        return AVERROR(EAGAIN);

    if (!ctx->avctx)
        return AVERROR_EOF;

    prepare_decoding(vd);

    if (avctx->skip_frame == AVDISCARD_ALL)
        return 0;

    mp_set_av_packet(ctx->avpkt, pkt, &ctx->codec_timebase);

    int ret = avcodec_send_packet(avctx, pkt ? ctx->avpkt : NULL);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return ret;

    if (ctx->hw_probing && ctx->num_sent_packets < 32 &&
        ctx->opts->software_fallback <= 32)
    {
        pkt = pkt ? demux_copy_packet(vd->packet_pool, pkt) : NULL;
        MP_TARRAY_APPEND(ctx, ctx->sent_packets, ctx->num_sent_packets, pkt);
    }

    if (ret < 0)
        handle_err(vd);
    return ret;
}

static void send_queued_packet(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    mp_assert(ctx->num_requeue_packets);

    if (send_packet(vd, ctx->requeue_packets[0]) != AVERROR(EAGAIN)) {
        talloc_free(ctx->requeue_packets[0]);
        MP_TARRAY_REMOVE_AT(ctx->requeue_packets, ctx->num_requeue_packets, 0);
    }
}

// Returns whether decoder is still active (!EOF state).
static int decode_frame(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    AVCodecContext *avctx = ctx->avctx;

    if (!avctx || ctx->force_eof)
        return AVERROR_EOF;

    prepare_decoding(vd);

    // Re-send old packets (typically after a hwdec fallback during init).
    if (ctx->num_requeue_packets)
        send_queued_packet(vd);

    int ret = avcodec_receive_frame(avctx, ctx->pic);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            // If flushing was initialized earlier and has ended now, make it
            // start over in case we get new packets at some point in the future.
            // This must take the delay queue into account, so avctx returns EOF
            // until the delay queue has been drained.
            if (!ctx->num_delay_queue)
                reset_avctx(vd);
        } else if (ret == AVERROR(EAGAIN)) {
            // just retry after caller writes a packet
        } else {
            handle_err(vd);
        }
        return ret;
    }

    // If something was decoded successfully, it must return a frame with valid
    // data.
    mp_assert(ctx->pic->buf[0]);

    struct mp_image *mpi = mp_image_from_av_frame(ctx->pic);
    if (!mpi) {
        av_frame_unref(ctx->pic);
        return ret;
    }

    ctx->hwdec_fail_count = 0;

    mpi->pts = mp_pts_from_av(ctx->pic->pts, &ctx->codec_timebase);
    mpi->dts = mp_pts_from_av(ctx->pic->pkt_dts, &ctx->codec_timebase);

    mpi->pkt_duration =
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(59, 30, 100)
        mp_pts_from_av(ctx->pic->duration, &ctx->codec_timebase);
#else
        mp_pts_from_av(ctx->pic->pkt_duration, &ctx->codec_timebase);
#endif

    av_frame_unref(ctx->pic);

    MP_TARRAY_APPEND(ctx, ctx->delay_queue, ctx->num_delay_queue, mpi);
    return ret;
}

static int receive_frame(struct mp_filter *vd, struct mp_frame *out_frame)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    int ret = decode_frame(vd);

    if (ctx->hwdec_failed) {
        // Failed hardware decoding? Try the next one, and eventually software.
        struct demux_packet **pkts = ctx->sent_packets;
        int num_pkts = ctx->num_sent_packets;
        ctx->sent_packets = NULL;
        ctx->num_sent_packets = 0;

        /*
         * We repeatedly force_fallback until we get an avctx, because there are
         * certain hwdecs that are really full decoders, and so if these fail,
         * they also fail to give us a valid avctx, and the early return path
         * here will simply give up on decoding completely if there is no
         * decoder. We should never hit an infinite loop as the hwdec list is
         * finite and we will eventually exhaust it and fall back to software
         * decoding (and in practice, most hwdecs are hwaccels and so the
         * decoder will successfully init even if the hwaccel fails later.)
         */
        do {
            force_fallback(vd);
        } while (!ctx->avctx);

        ctx->requeue_packets = pkts;
        ctx->num_requeue_packets = num_pkts;

        return 0; // force retry
    }

    if (ret == AVERROR(EAGAIN) && ctx->num_requeue_packets)
        return 0; // force retry, so send_queued_packet() gets called

    if (ctx->num_delay_queue <= ctx->max_delay_queue && ret != AVERROR_EOF)
        return ret;

    if (!ctx->num_delay_queue)
        return ret;

    struct mp_image *res = ctx->delay_queue[0];
    MP_TARRAY_REMOVE_AT(ctx->delay_queue, ctx->num_delay_queue, 0);

    res = res ? mp_img_swap_to_native(res) : NULL;
    if (!res)
        return AVERROR_UNKNOWN;

    if (ctx->use_hwdec && ctx->hwdec.copying && res->hwctx) {
        struct mp_image *sw = mp_image_hw_download(res, ctx->hwdec_swpool);
        mp_image_unrefp(&res);
        res = sw;
        if (!res) {
            MP_ERR(vd, "Could not copy back hardware decoded frame.\n");
            ctx->hwdec_fail_count = INT_MAX - 1; // force fallback
            handle_err(vd);
            return AVERROR_UNKNOWN;
        }
    }

    if (!ctx->hwdec_notified) {
        if (ctx->use_hwdec) {
            MP_INFO(vd, "INFO: Using hardware decoding (%s).\n",
                    ctx->hwdec.method_name);
        } else {
            MP_INFO(vd, "INFO: Using software decoding.\n");
        }
        ctx->hwdec_notified = true;
    }

    if (ctx->hw_probing) {
        for (int n = 0; n < ctx->num_sent_packets; n++)
            demux_packet_pool_push(vd->packet_pool, ctx->sent_packets[n]);
        ctx->num_sent_packets = 0;
        ctx->hw_probing = false;
    }

    *out_frame = MAKE_FRAME(MP_FRAME_VIDEO, res);
    return 0;
}

static int control(struct mp_filter *vd, enum dec_ctrl cmd, void *arg)
{
    vd_ffmpeg_ctx *ctx = vd->priv;
    switch (cmd) {
    case VDCTRL_SET_FRAMEDROP:
        ctx->framedrop_flags = *(int *)arg;
        return CONTROL_TRUE;
    case VDCTRL_CHECK_FORCED_EOF: {
        *(bool *)arg = ctx->force_eof;
        return CONTROL_TRUE;
    }
    case VDCTRL_GET_BFRAMES: {
        AVCodecContext *avctx = ctx->avctx;
        if (!avctx)
            break;
        *(int *)arg = avctx->has_b_frames;
        return CONTROL_TRUE;
    }
    case VDCTRL_GET_HWDEC: {
        if (!ctx->hwdec_notified)
            return CONTROL_FALSE;
        *(char **)arg = ctx->use_hwdec ? ctx->hwdec.method_name : NULL;
        return CONTROL_TRUE;
    }
    case VDCTRL_FORCE_HWDEC_FALLBACK:
        if (ctx->use_hwdec) {
            force_fallback(vd);
            return ctx->avctx ? CONTROL_OK : CONTROL_ERROR;
        }
        return CONTROL_FALSE;
    case VDCTRL_REINIT:
        reinit(vd);
        return CONTROL_TRUE;
    }
    return CONTROL_UNKNOWN;
}

static void vd_lavc_process(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    lavc_process(vd, &ctx->state, send_packet, receive_frame);
}

static void vd_lavc_reset(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    flush_all(vd);

    ctx->state = (struct lavc_state){0};
    ctx->framedrop_flags = 0;
}

static void vd_lavc_destroy(struct mp_filter *vd)
{
    vd_ffmpeg_ctx *ctx = vd->priv;

    uninit_avctx(vd);

    mp_mutex_destroy(&ctx->dr_lock);
}

static const struct mp_filter_info vd_lavc_filter = {
    .name = "vd_lavc",
    .priv_size = sizeof(vd_ffmpeg_ctx),
    .process = vd_lavc_process,
    .reset = vd_lavc_reset,
    .destroy = vd_lavc_destroy,
};

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    struct mp_filter *vd = mp_filter_create(parent, &vd_lavc_filter);
    if (!vd)
        return NULL;

    mp_filter_add_pin(vd, MP_PIN_IN, "in");
    mp_filter_add_pin(vd, MP_PIN_OUT, "out");

    vd->log = mp_log_new(vd, parent->log, NULL);

    vd_ffmpeg_ctx *ctx = vd->priv;
    ctx->log = vd->log;
    ctx->opts_cache = m_config_cache_alloc(ctx, vd->global, &vd_lavc_conf);
    ctx->opts = ctx->opts_cache->opts;
    ctx->codec = codec;
    ctx->decoder = talloc_strdup(ctx, decoder);
    ctx->hwdec_swpool = mp_image_pool_new(ctx);
    ctx->dr_pool = mp_image_pool_new(ctx);

    ctx->public.f = vd;
    ctx->public.control = control;

    pthread_mutex_init(&ctx->dr_lock, NULL);

    // hwdec/DR
    struct mp_stream_info *info = mp_filter_find_stream_info(vd);
    if (info) {
        ctx->hwdec_devs = info->hwdec_devs;
        ctx->vo = info->dr_vo;
    }

    reinit(vd);

    if (!ctx->avctx) {
        talloc_free(vd);
        return NULL;
    }
    return &ctx->public;
}

static void add_decoders(struct mp_decoder_list *list)
{
    mp_add_lavc_decoders(list, AVMEDIA_TYPE_VIDEO);
}

const struct mp_decoder_fns vd_lavc = {
    .create = create,
    .add_decoders = add_decoders,
};
