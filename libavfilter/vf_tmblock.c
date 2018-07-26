/**
 * libavfilter adapter for libtmblock
 */

#include "libavutil/opt.h"
#include "internal.h"
#include "bufferqueue.h"
#include "framesync.h"

#include "tmblock.h"

typedef int TM_Function(TM_Picture *input, TM_Picture *logo, int offset_x,
        int offset_y, TM_Picture *output, TM_Layout layout);

typedef enum {
    FUNC_EMBED,
    FUNC_PRE,
    FUNC_POST,
    NB_FUNC,
} TMFunctionType ;


typedef struct TMBlockContext {
    const AVClass *class;
    int offset_x, offset_y;
    TM_Picture input, logo, output;
    struct FFBufQueue queue_input, queue_logo;
    TMFunctionType func_type;
    TM_Function *func;
    FFFrameSync fs;
} TMBlockContext;

#define OFFSET(x) offsetof(TMBlockContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption tmblock_options[] = {
    { "x", "set offset at x direction", OFFSET(offset_x), AV_OPT_TYPE_INT, {.i64=0 }, INT_MIN, INT_MAX, FLAGS},
    { "y", "set offset at y direction", OFFSET(offset_y), AV_OPT_TYPE_INT, {.i64=0 }, INT_MIN, INT_MAX, FLAGS},
    { "func", "set func", OFFSET(func_type), AV_OPT_TYPE_INT, {.i64=FUNC_EMBED}, 0, NB_FUNC-1, FLAGS, "func" },
        { "embed", "embed watermark", 0, AV_OPT_TYPE_CONST, {.i64=FUNC_EMBED}, INT_MIN, INT_MAX, FLAGS, "func" },
        { "pre", "remove watermark by pre-processing", 0, AV_OPT_TYPE_CONST, {.i64=FUNC_PRE}, INT_MIN, INT_MAX, FLAGS, "func" },
        { "post", "remove watermark by post-processing", 0, AV_OPT_TYPE_CONST, {.i64=FUNC_POST}, INT_MIN, INT_MAX, FLAGS, "func" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(tmblock);

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat io_fmts[] = {AV_PIX_FMT_RGB24, AV_PIX_FMT_NONE};
    static const enum AVPixelFormat logo_fmts[] = {AV_PIX_FMT_RGBA, AV_PIX_FMT_NONE};
    int ret;

    AVFilterLink* input_link = ctx->inputs[0];
    AVFilterLink* logo_link = ctx->inputs[1];
    AVFilterLink* output_link = ctx->outputs[0];
    AVFilterFormats *io_formats = NULL, *logo_formats = NULL;

    if (!(io_formats = ff_make_format_list(io_fmts)) ||
        !(logo_formats = ff_make_format_list(logo_fmts))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = ff_formats_ref(io_formats, &input_link->out_formats)) < 0 ||
        (ret = ff_formats_ref(logo_formats, &logo_link->out_formats)) < 0 ||
        (ret = ff_formats_ref(io_formats, &output_link->in_formats)) < 0) {
        goto fail;
    }
    return 0;

fail:
    if (io_formats)
        av_freep(&io_formats->formats);
    av_freep(&io_formats);
    if (logo_formats)
        av_freep(&logo_formats->formats);
    av_freep(&logo_formats);
    return ret;
}

static void copy_AVFrame_to_TM_Picture(TM_Picture *picture, AVFrame *frame) {
    picture->height = frame->height;
    picture->width = frame->width;
    picture->linesize = frame->linesize[0];
    picture->mode = frame->format == AV_PIX_FMT_RGBA ? TM_RGBA : AV_PIX_FMT_RGB24 ? TM_RGB: -1;
    assert(picture->mode != -1);
    picture->ptr = frame->data[0];
}

static int draw_frame(AVFilterContext *ctx,
        AVFrame *input_buf, AVFrame *logo_buf, AVFrame *output_buf) {
    TMBlockContext *tmblock = ctx->priv;
    copy_AVFrame_to_TM_Picture(&tmblock->input, input_buf);
    copy_AVFrame_to_TM_Picture(&tmblock->logo, logo_buf);
    copy_AVFrame_to_TM_Picture(&tmblock->output, output_buf);
    return tmblock->func(&tmblock->input, &tmblock->logo,
            tmblock->offset_x, tmblock->offset_y,
            &tmblock->output, TM_PACKED);
}

static int process_frame(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    TMBlockContext *tmblock = ctx->priv;
    AVFilterLink *output_link = ctx->outputs[0];
    AVFrame *input_buf, *logo_buf, *output_buf;

    int ret = 0;
    if ((ret = ff_framesync_get_frame(&tmblock->fs, 0, &input_buf, 0)) < 0 ||
            (ret = ff_framesync_get_frame(&tmblock->fs, 0, &logo_buf, 0)) < 0) {
        return ret;
    }

    output_buf =
        ff_get_video_buffer(output_link, output_link->w, output_link->h);

    if (!output_buf) {
        ret = AVERROR(ENOMEM);
        goto out;
    }
    av_frame_copy_props(output_buf, input_buf);

    if (ret = draw_frame(ctx, input_buf, logo_buf, output_buf))
        goto out;

    if (ret = ff_filter_frame(output_link, output_buf))
        goto out;

out:
    av_frame_free(&output_buf);
    av_frame_free(&input_buf);
    av_frame_free(&logo_buf);
    return ret;
}

static av_cold int init(AVFilterContext *ctx) {
    TMBlockContext *tmblock = ctx->priv;
    switch (tmblock->func_type) {
        case FUNC_EMBED:
            tmblock->func = TM_embed;
            break;
        case FUNC_POST:
            tmblock->func = TM_post;
            break;
        case FUNC_PRE:
            tmblock->func = TM_pre;
            break;
        default:
            return 1;
    }

    tmblock->fs.on_event = process_frame;
    return ff_framesync_configure(&tmblock->fs);
}


static av_cold void uninit(AVFilterContext *ctx) {
    TMBlockContext *tmblock = ctx->priv;
    ff_bufqueue_discard_all(&tmblock->queue_input);
    ff_bufqueue_discard_all(&tmblock->queue_logo);
}

static const AVFilterPad tmblock_inputs[] = {
    {
        .name = "input",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    {
        .name = "logo",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    {NULL}};

static int config_output(AVFilterLink *output_link) {
    AVFilterContext *ctx = output_link->src;
    AVFilterLink *input_link = ctx->inputs[0];
    output_link->w = input_link->w;
    output_link->h = input_link->h;
    output_link->time_base = input_link->time_base;
    output_link->sample_aspect_ratio = input_link->sample_aspect_ratio;
    output_link->frame_rate = input_link->frame_rate;
    return 0;
}

static const AVFilterPad tmblock_outputs[] = {
    {
        .name = "output",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    {NULL}};

AVFilter ff_vf_tmblock = {
    .name = "tmblock",
    .description = NULL_IF_CONFIG_SMALL("Process the video with TMBlock."),
    .priv_size = sizeof(TMBlockContext),
    .init = init,
    .uninit = uninit,
    .query_formats = query_formats,
    .inputs = tmblock_inputs,
    .outputs = tmblock_outputs,
    .priv_class = &tmblock_class,
};
