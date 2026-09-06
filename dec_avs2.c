/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / AVS2 decoder filter, based on davs2.
 *
 *  AVS2 (AVS2-P2) is the Chinese standard that preceded AVS3, and its test
 *  material comes the same way: a raw elementary stream of start-code
 *  delimited units, with no container. The filter takes the whole file, walks
 *  it unit by unit and emits one packet per decoded frame - the shape libgif
 *  and libmng use for animation.
 *
 *  davs2's API is a push/pull pair rather than a callback: send a packet, then
 *  pull whatever frames became ready. That means, unlike the AVS3 filter next
 *  door, nothing here has to be reached through a file-static pointer and
 *  several streams could be decoded side by side.
 *
 *  It is opened with threads = 1: side modules are single-threaded.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <davs2.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;
	u32 width, height, timescale, frame_dur;
	u64 next_cts;
	Bool cfg_done;
} GF_AVS2DecCtx;

static GF_Err avs2dec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_AVS2DecCtx *ctx = (GF_AVS2DecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* Corrected from the sequence header as soon as one is decoded; GPAC
	 * resolves the graph from these first. */
	ctx->width = 320;
	ctx->height = 180;
	ctx->timescale = 25;
	ctx->frame_dur = 1;
	ctx->next_cts = 0;
	ctx->cfg_done = GF_FALSE;

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(ctx->height));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(ctx->width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->timescale));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FPS, &PROP_FRAC_INT(ctx->timescale, ctx->frame_dur));

	return GF_OK;
}

/* Next unit boundary at or after "from", or size when there is none. AVS2 uses
 * MPEG-style 00 00 01 xx start codes; as in the AVS3 filter, only the five
 * picture- and sequence-level codes begin a new unit - extensions, user data
 * and the slices of a picture carry their own code and belong with it. */
static u32 avs2_next_start_code(const u8 *data, u32 size, u32 from)
{
	u32 i;
	for (i = from; (i + 4) <= size; i++)
	{
		if (data[i] || data[i + 1] || (data[i + 2] != 1))
			continue;
		switch (data[i + 3])
		{
		case 0x00: /* slice 0 */
		case 0xB0: /* sequence header */
		case 0xB1: /* sequence end */
		case 0xB3: /* I picture */
		case 0xB6: /* PB picture */
			return i;
		default:
			break;
		}
	}
	return size;
}

/* Ships one decoded picture. davs2 hands each plane over with its own stride,
 * so the copy is row by row into the packed 4:2:0 buffer the pid expects. */
static GF_Err avs2dec_send_frame(GF_AVS2DecCtx *ctx, davs2_seq_info_t *hdr, davs2_picture_t *pic)
{
	GF_FilterPacket *dst_pck;
	u8 *output;
	u32 w, h, i;

	if (!pic || !pic->planes[0])
		return GF_OK;
	if (pic->bytes_per_sample != 1)
	{
		/* 10-bit AVS2 exists; converting it to the 8-bit the pid carries would
		 * be a lossy step taken silently, so it is refused instead. */
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AVS2Dec] Only 8-bit streams are handled (%d bytes per sample)\n",
		                                    pic->bytes_per_sample));
		return GF_NOT_SUPPORTED;
	}

	w = (u32)pic->widths[0];
	h = (u32)pic->lines[0];
	if (!w || !h)
		return GF_OK;

	if (!ctx->cfg_done || (w != ctx->width) || (h != ctx->height))
	{
		ctx->width = w;
		ctx->height = h;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(w));
		if (hdr && (hdr->frame_rate > 0.f))
		{
			/* the header carries a float frame rate; 1000 as the timescale
			 * keeps 23.976 and friends exact enough for frame timing */
			ctx->timescale = 1000;
			ctx->frame_dur = (u32)(1000.f / hdr->frame_rate + 0.5f);
			if (!ctx->frame_dur)
				ctx->frame_dur = 40;
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->timescale));
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_FPS,
			                           &PROP_FRAC_INT(ctx->timescale, ctx->frame_dur));
		}
		ctx->cfg_done = GF_TRUE;
	}

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, w * h * 3 / 2, &output);
	if (!dst_pck)
		return GF_OUT_OF_MEM;

	for (i = 0; i < h; i++)
		memcpy(output + (size_t)i * w, pic->planes[0] + (size_t)i * pic->strides[0], w);
	{
		u8 *dst_u = output + (size_t)w * h;
		u8 *dst_v = dst_u + (size_t)(w / 2) * (h / 2);
		for (i = 0; i < h / 2; i++)
		{
			memcpy(dst_u + (size_t)i * (w / 2), pic->planes[1] + (size_t)i * pic->strides[1], w / 2);
			memcpy(dst_v + (size_t)i * (w / 2), pic->planes[2] + (size_t)i * pic->strides[2], w / 2);
		}
	}

	gf_filter_pck_set_cts(dst_pck, ctx->next_cts);
	gf_filter_pck_set_duration(dst_pck, ctx->frame_dur);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
	ctx->next_cts += ctx->frame_dur;
	return GF_OK;
}

static GF_Err avs2dec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck;
	u8 *data;
	u32 size, pos;
	void *dec = NULL;
	davs2_param_t param;
	davs2_seq_info_t hdr;
	davs2_picture_t pic;
	GF_Err e = GF_OK;
	GF_AVS2DecCtx *ctx = (GF_AVS2DecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data || (size < 4) || data[0] || data[1] || (data[2] != 1))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AVS2Dec] Stream does not start with an AVS2 start code\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	memset(&param, 0, sizeof(param));
	param.threads = 1; /* side modules are single-threaded */
	param.info_level = DAVS2_LOG_ERROR;
	dec = davs2_decoder_open(&param);
	if (!dec)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AVS2Dec] Could not open the decoder\n"));
		return GF_NOT_SUPPORTED;
	}

	pos = 0;
	while (pos < size)
	{
		davs2_packet_t packet;
		u32 next = avs2_next_start_code(data, size, pos + 3);
		int ret;

		memset(&packet, 0, sizeof(packet));
		packet.data = data + pos;
		packet.len = (int)(next - pos);
		packet.pts = (int64_t)ctx->next_cts;
		packet.dts = packet.pts;

		if (davs2_decoder_send_packet(dec, &packet) == DAVS2_ERROR)
		{
			GF_LOG(GF_LOG_WARNING, GF_LOG_CODEC, ("[AVS2Dec] Decoder rejected a unit at offset %u\n", pos));
			pos = next;
			continue;
		}

		/* pull everything that became ready */
		do
		{
			memset(&hdr, 0, sizeof(hdr));
			memset(&pic, 0, sizeof(pic));
			ret = davs2_decoder_recv_frame(dec, &hdr, &pic);
			if (ret == DAVS2_GOT_FRAME)
			{
				e = avs2dec_send_frame(ctx, &hdr, &pic);
				davs2_decoder_frame_unref(dec, &pic);
				if (e != GF_OK)
					break;
			}
		} while ((ret == DAVS2_GOT_FRAME) || (ret == DAVS2_GOT_HEADER));

		if (e != GF_OK)
			break;
		pos = next;
	}

	/* frames the decoder was still holding back for reordering */
	if (e == GF_OK)
	{
		int ret;
		do
		{
			memset(&hdr, 0, sizeof(hdr));
			memset(&pic, 0, sizeof(pic));
			ret = davs2_decoder_flush(dec, &hdr, &pic);
			if (ret == DAVS2_GOT_FRAME)
			{
				e = avs2dec_send_frame(ctx, &hdr, &pic);
				davs2_decoder_frame_unref(dec, &pic);
			}
		} while ((e == GF_OK) && (ret != DAVS2_END) && (ret != DAVS2_ERROR));
	}

	davs2_decoder_close(dec);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (e != GF_OK)
		return e;
	if (!ctx->cfg_done)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[AVS2Dec] No frame decoded\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void avs2dec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability AVS2DecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "avs2"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/avs2|video/x-avs2"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister AVS2DecoderRegister = {
	.name = "avs2dec",
	GF_FS_SET_DESCRIPTION("AVS2 (AVS2-P2) video decoder")
		GF_FS_SET_HELP("This filter decodes raw AVS2 elementary streams using davs2, emitting one raw YUV 4:2:0 frame per picture.")
			.private_size = sizeof(GF_AVS2DecCtx),
	SETCAPS(AVS2DecCaps),
	.configure_pid = avs2dec_configure_pid,
	.process = avs2dec_process,
	.finalize = avs2dec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE avs2dec_register(GF_FilterSession *session)
{
	return &AVS2DecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_avs2dec(void) {
    gf_filter_auto_register("avs2dec", avs2dec_register);
}
