/*****************************************************************************
 * h264.c: h264/avc video packetizer
 *****************************************************************************
 * Copyright (C) 2001, 2002, 2006 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Eric Petit <titer@videolan.org>
 *          Gildas Bazin <gbazin@videolan.org>
 *          Derk-Jan Hartman <hartman at videolan dot org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_codec.h>
#include <vlc_block.h>

#include <vlc_block_helper.h>
#include <vlc_bits.h>
#include "../codec/cc.h"
#include "h264_nal.h"
#include "packetizer_helper.h"
#include "../demux/mpeg/mpeg_parser_helpers.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin ()
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_PACKETIZER )
    set_description( N_("H.264 video packetizer") )
    set_capability( "packetizer", 50 )
    set_callbacks( Open, Close )
vlc_module_end ()


/****************************************************************************
 * Local prototypes
 ****************************************************************************/
typedef struct
{
    int i_nal_type;
    int i_nal_ref_idc;

    int i_frame_type;
    int i_pic_parameter_set_id;
    int i_frame_num;

    int i_field_pic_flag;
    int i_bottom_field_flag;

    int i_idr_pic_id;

    int i_pic_order_cnt_lsb;
    int i_delta_pic_order_cnt_bottom;

    int i_delta_pic_order_cnt0;
    int i_delta_pic_order_cnt1;
} slice_t;

struct decoder_sys_t
{
    /* */
    packetizer_t packetizer;

    /* */
    bool    b_slice;
    block_t *p_frame;
    bool    b_frame_sps;
    bool    b_frame_pps;

    bool   b_header;
    bool   b_sps;
    bool   b_pps;
    block_t *pp_sps[SPS_MAX];
    block_t *pp_pps[PPS_MAX];
    int    i_recovery_frames;  /* -1 = no recovery */

    /* avcC data */
    int i_avcC_length_size;

    /* Useful values of the Sequence Parameter Set */
    int i_log2_max_frame_num;
    int b_frame_mbs_only;
    int i_pic_order_cnt_type;
    int i_delta_pic_order_always_zero_flag;
    int i_log2_max_pic_order_cnt_lsb;

    /* Value from Picture Parameter Set */
    int i_pic_order_present_flag;

    /* VUI */
    bool b_timing_info_present_flag;
    uint32_t i_num_units_in_tick;
    uint32_t i_time_scale;
    bool b_fixed_frame_rate;
    bool b_pic_struct_present_flag;
    uint8_t i_pic_struct;
    bool b_cpb_dpb_delays_present_flag;
    uint8_t i_cpb_removal_delay_length_minus1;
    uint8_t i_dpb_output_delay_length_minus1;

    /* Useful values of the Slice Header */
    slice_t slice;

    /* */
    bool b_even_frame;
    mtime_t i_frame_pts;
    mtime_t i_frame_dts;
    mtime_t i_prev_pts;
    mtime_t i_prev_dts;

    /* */
    uint32_t i_cc_flags;
    mtime_t i_cc_pts;
    mtime_t i_cc_dts;
    cc_data_t cc;

    cc_data_t cc_next;
};

#define BLOCK_FLAG_PRIVATE_AUD (1 << BLOCK_FLAG_PRIVATE_SHIFT)

static block_t *Packetize( decoder_t *, block_t ** );
static block_t *PacketizeAVC1( decoder_t *, block_t ** );
static block_t *GetCc( decoder_t *p_dec, bool pb_present[4] );

static void PacketizeReset( void *p_private, bool b_broken );
static block_t *PacketizeParse( void *p_private, bool *pb_ts_used, block_t * );
static int PacketizeValidate( void *p_private, block_t * );

static block_t *ParseNALBlock( decoder_t *, bool *pb_ts_used, block_t * );
static block_t *CreateAnnexbNAL( decoder_t *, const uint8_t *p, int );

static block_t *OutputPicture( decoder_t *p_dec );
static void PutSPS( decoder_t *p_dec, block_t *p_frag );
static void PutPPS( decoder_t *p_dec, block_t *p_frag );
static void ParseSlice( decoder_t *p_dec, bool *pb_new_picture, slice_t *p_slice,
                        int i_nal_ref_idc, int i_nal_type, const block_t *p_frag );
static void ParseSei( decoder_t *, block_t * );


static const uint8_t p_h264_startcode[3] = { 0x00, 0x00, 0x01 };

/*****************************************************************************
 * Open: probe the packetizer and return score
 * When opening after demux, the packetizer is only loaded AFTER the decoder
 * That means that what you set in fmt_out is ignored by the decoder in this special case
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    decoder_t     *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;
    int i;

    if( p_dec->fmt_in.i_codec != VLC_CODEC_H264 )
        return VLC_EGENERIC;
    if( p_dec->fmt_in.i_original_fourcc == VLC_FOURCC( 'a', 'v', 'c', '1') &&
        p_dec->fmt_in.i_extra < 7 )
        return VLC_EGENERIC;

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_dec->p_sys = p_sys = malloc( sizeof(decoder_sys_t) ) ) == NULL )
    {
        return VLC_ENOMEM;
    }

    packetizer_Init( &p_sys->packetizer,
                     p_h264_startcode, sizeof(p_h264_startcode),
                     p_h264_startcode, 1, 5,
                     PacketizeReset, PacketizeParse, PacketizeValidate, p_dec );

    p_sys->b_slice = false;
    p_sys->p_frame = NULL;
    p_sys->b_frame_sps = false;
    p_sys->b_frame_pps = false;

    p_sys->b_header= false;
    p_sys->b_sps   = false;
    p_sys->b_pps   = false;
    for( i = 0; i < SPS_MAX; i++ )
        p_sys->pp_sps[i] = NULL;
    for( i = 0; i < PPS_MAX; i++ )
        p_sys->pp_pps[i] = NULL;
    p_sys->i_recovery_frames = -1;

    p_sys->slice.i_nal_type = -1;
    p_sys->slice.i_nal_ref_idc = -1;
    p_sys->slice.i_idr_pic_id = -1;
    p_sys->slice.i_frame_num = -1;
    p_sys->slice.i_frame_type = 0;
    p_sys->slice.i_pic_parameter_set_id = -1;
    p_sys->slice.i_field_pic_flag = 0;
    p_sys->slice.i_bottom_field_flag = -1;
    p_sys->slice.i_pic_order_cnt_lsb = -1;
    p_sys->slice.i_delta_pic_order_cnt_bottom = -1;

    p_sys->b_timing_info_present_flag = false;
    p_sys->b_pic_struct_present_flag = false;
    p_sys->b_cpb_dpb_delays_present_flag = false;
    p_sys->i_cpb_removal_delay_length_minus1 = 0;
    p_sys->i_dpb_output_delay_length_minus1 = 0;

    p_sys->b_even_frame = false;
    p_sys->i_frame_dts = VLC_TS_INVALID;
    p_sys->i_frame_pts = VLC_TS_INVALID;
    p_sys->i_prev_dts = VLC_TS_INVALID;
    p_sys->i_prev_pts = VLC_TS_INVALID;

    /* Setup properties */
    es_format_Copy( &p_dec->fmt_out, &p_dec->fmt_in );
    p_dec->fmt_out.i_codec = VLC_CODEC_H264;

    if( p_dec->fmt_in.i_original_fourcc == VLC_FOURCC( 'a', 'v', 'c', '1' ) )
    {
        /* This type of stream is produced by mp4 and matroska
         * when we want to store it in another streamformat, you need to convert
         * The fmt_in.p_extra should ALWAYS contain the avcC
         * The fmt_out.p_extra should contain all the SPS and PPS with 4 byte startcodes */
        uint8_t *p = &((uint8_t*)p_dec->fmt_in.p_extra)[4];
        int i_sps, i_pps;
        bool b_dummy;
        int i;

        /* Parse avcC */
        p_sys->i_avcC_length_size = 1 + ((*p++)&0x03);

        /* Read SPS */
        i_sps = (*p++)&0x1f;
        for( i = 0; i < i_sps; i++ )
        {
            uint16_t i_length = GetWBE( p ); p += 2;
            if( i_length >
                (uint8_t*)p_dec->fmt_in.p_extra + p_dec->fmt_in.i_extra - p )
            {
                return VLC_EGENERIC;
            }
            block_t *p_sps = CreateAnnexbNAL( p_dec, p, i_length );
            if( !p_sps )
                return VLC_EGENERIC;
            ParseNALBlock( p_dec, &b_dummy, p_sps );
            p += i_length;
        }
        /* Read PPS */
        i_pps = *p++;
        for( i = 0; i < i_pps; i++ )
        {
            uint16_t i_length = GetWBE( p ); p += 2;
            if( i_length >
                (uint8_t*)p_dec->fmt_in.p_extra + p_dec->fmt_in.i_extra - p )
            {
                return VLC_EGENERIC;
            }
            block_t *p_pps = CreateAnnexbNAL( p_dec, p, i_length );
            if( !p_pps )
                return VLC_EGENERIC;
            ParseNALBlock( p_dec, &b_dummy, p_pps );
            p += i_length;
        }
        msg_Dbg( p_dec, "avcC length size=%d, sps=%d, pps=%d",
                 p_sys->i_avcC_length_size, i_sps, i_pps );

        if( !p_sys->b_sps || !p_sys->b_pps )
            return VLC_EGENERIC;

        /* FIXME: FFMPEG isn't happy at all if you leave this */
        if( p_dec->fmt_out.i_extra > 0 )
            free( p_dec->fmt_out.p_extra );
        p_dec->fmt_out.i_extra = 0;
        p_dec->fmt_out.p_extra = NULL;

        /* Set the new extradata */
        for( i = 0; i < SPS_MAX; i++ )
        {
            if( p_sys->pp_sps[i] )
                p_dec->fmt_out.i_extra += p_sys->pp_sps[i]->i_buffer;
        }
        for( i = 0; i < PPS_MAX; i++ )
        {
            if( p_sys->pp_pps[i] )
                p_dec->fmt_out.i_extra += p_sys->pp_pps[i]->i_buffer;
        }
        p_dec->fmt_out.p_extra = malloc( p_dec->fmt_out.i_extra );
        if( p_dec->fmt_out.p_extra )
        {
            uint8_t *p_dst = p_dec->fmt_out.p_extra;

            for( i = 0; i < SPS_MAX; i++ )
            {
                if( p_sys->pp_sps[i] )
                {
                    memcpy( p_dst, p_sys->pp_sps[i]->p_buffer, p_sys->pp_sps[i]->i_buffer );
                    p_dst += p_sys->pp_sps[i]->i_buffer;
                }
            }
            for( i = 0; i < PPS_MAX; i++ )
            {
                if( p_sys->pp_pps[i] )
                {
                    memcpy( p_dst, p_sys->pp_pps[i]->p_buffer, p_sys->pp_pps[i]->i_buffer );
                    p_dst += p_sys->pp_pps[i]->i_buffer;
                }
            }
            p_sys->b_header = true;
        }
        else
        {
            p_dec->fmt_out.i_extra = 0;
        }

        /* Set callback */
        p_dec->pf_packetize = PacketizeAVC1;
        /* TODO CC ? */
    }
    else
    {
        /* This type of stream contains data with 3 of 4 byte startcodes
         * The fmt_in.p_extra MAY contain SPS/PPS with 4 byte startcodes
         * The fmt_out.p_extra should be the same */

        /* Set callback */
        p_dec->pf_packetize = Packetize;
        p_dec->pf_get_cc = GetCc;

        /* */
        p_sys->i_cc_pts = VLC_TS_INVALID;
        p_sys->i_cc_dts = VLC_TS_INVALID;
        p_sys->i_cc_flags = 0;
        cc_Init( &p_sys->cc );
        cc_Init( &p_sys->cc_next );

        /* */
        if( p_dec->fmt_in.i_extra > 0 )
            packetizer_Header( &p_sys->packetizer,
                               p_dec->fmt_in.p_extra, p_dec->fmt_in.i_extra );
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: clean up the packetizer
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;
    int i;

    if( p_sys->p_frame )
        block_ChainRelease( p_sys->p_frame );
    for( i = 0; i < SPS_MAX; i++ )
    {
        if( p_sys->pp_sps[i] )
            block_Release( p_sys->pp_sps[i] );
    }
    for( i = 0; i < PPS_MAX; i++ )
    {
        if( p_sys->pp_pps[i] )
            block_Release( p_sys->pp_pps[i] );
    }
    packetizer_Clean( &p_sys->packetizer );

    if( p_dec->pf_get_cc )
    {
         cc_Exit( &p_sys->cc_next );
         cc_Exit( &p_sys->cc );
    }

    free( p_sys );
}

/****************************************************************************
 * Packetize: the whole thing
 * Search for the startcodes 3 or more bytes
 * Feed ParseNALBlock ALWAYS with 4 byte startcode prepended NALs
 ****************************************************************************/
static block_t *Packetize( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    return packetizer_Packetize( &p_sys->packetizer, pp_block );
}

/****************************************************************************
 * PacketizeAVC1: Takes VCL blocks of data and creates annexe B type NAL stream
 * Will always use 4 byte 0 0 0 1 startcodes
 * Will prepend a SPS and PPS before each keyframe
 ****************************************************************************/
static block_t *PacketizeAVC1( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t       *p_block;
    block_t       *p_ret = NULL;
    uint8_t       *p;

    if( !pp_block || !*pp_block )
        return NULL;
    if( (*pp_block)->i_flags&(BLOCK_FLAG_DISCONTINUITY|BLOCK_FLAG_CORRUPTED) )
    {
        block_Release( *pp_block );
        return NULL;
    }

    p_block = *pp_block;
    *pp_block = NULL;

    for( p = p_block->p_buffer; p < &p_block->p_buffer[p_block->i_buffer]; )
    {
        block_t *p_pic;
        bool b_dummy;
        int i_size = 0;
        int i;

        for( i = 0; i < p_sys->i_avcC_length_size; i++ )
        {
            i_size = (i_size << 8) | (*p++);
        }

        if( i_size <= 0 ||
            i_size > ( p_block->p_buffer + p_block->i_buffer - p ) )
        {
            msg_Err( p_dec, "Broken frame : size %d is too big", i_size );
            break;
        }

        block_t *p_part = CreateAnnexbNAL( p_dec, p, i_size );
        if( !p_part )
            break;

        p_part->i_dts = p_block->i_dts;
        p_part->i_pts = p_block->i_pts;

        /* Parse the NAL */
        if( ( p_pic = ParseNALBlock( p_dec, &b_dummy, p_part ) ) )
        {
            block_ChainAppend( &p_ret, p_pic );
        }
        p += i_size;
    }
    block_Release( p_block );

    return p_ret;
}

/*****************************************************************************
 * GetCc:
 *****************************************************************************/
static block_t *GetCc( decoder_t *p_dec, bool pb_present[4] )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_cc;

    for( int i = 0; i < 4; i++ )
        pb_present[i] = p_sys->cc.pb_present[i];

    if( p_sys->cc.i_data <= 0 )
        return NULL;

    p_cc = block_Alloc( p_sys->cc.i_data);
    if( p_cc )
    {
        memcpy( p_cc->p_buffer, p_sys->cc.p_data, p_sys->cc.i_data );
        p_cc->i_dts =
        p_cc->i_pts = p_sys->cc.b_reorder ? p_sys->i_cc_pts : p_sys->i_cc_dts;
        p_cc->i_flags = ( p_sys->cc.b_reorder  ? p_sys->i_cc_flags : BLOCK_FLAG_TYPE_P ) & BLOCK_FLAG_TYPE_MASK;
    }
    cc_Flush( &p_sys->cc );
    return p_cc;
}

/****************************************************************************
 * Helpers
 ****************************************************************************/
static void PacketizeReset( void *p_private, bool b_broken )
{
    decoder_t *p_dec = p_private;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( b_broken )
    {
        if( p_sys->p_frame )
            block_ChainRelease( p_sys->p_frame );
        p_sys->p_frame = NULL;
        p_sys->b_frame_sps = false;
        p_sys->b_frame_pps = false;
        p_sys->slice.i_frame_type = 0;
        p_sys->b_slice = false;
    }
    p_sys->i_frame_pts = VLC_TS_INVALID;
    p_sys->i_frame_dts = VLC_TS_INVALID;
    p_sys->i_prev_dts = VLC_TS_INVALID;
    p_sys->i_prev_pts = VLC_TS_INVALID;
    p_sys->b_even_frame = false;
}
static block_t *PacketizeParse( void *p_private, bool *pb_ts_used, block_t *p_block )
{
    decoder_t *p_dec = p_private;

    /* Remove trailing 0 bytes */
    while( p_block->i_buffer > 5 && p_block->p_buffer[p_block->i_buffer-1] == 0x00 )
        p_block->i_buffer--;

    return ParseNALBlock( p_dec, pb_ts_used, p_block );
}
static int PacketizeValidate( void *p_private, block_t *p_au )
{
    VLC_UNUSED(p_private);
    VLC_UNUSED(p_au);
    return VLC_SUCCESS;
}

static block_t *CreateAnnexbNAL( decoder_t *p_dec, const uint8_t *p, int i_size )
{
    block_t *p_nal;

    p_nal = block_Alloc( 4 + i_size );
    if( !p_nal ) return NULL;

    /* Add start code */
    p_nal->p_buffer[0] = 0x00;
    p_nal->p_buffer[1] = 0x00;
    p_nal->p_buffer[2] = 0x00;
    p_nal->p_buffer[3] = 0x01;

    /* Copy nalu */
    memcpy( &p_nal->p_buffer[4], p, i_size );

    VLC_UNUSED(p_dec);
    return p_nal;
}

/*****************************************************************************
 * ParseNALBlock: parses annexB type NALs
 * All p_frag blocks are required to start with 0 0 0 1 4-byte startcode
 *****************************************************************************/
static block_t *ParseNALBlock( decoder_t *p_dec, bool *pb_ts_used, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_pic = NULL;

    const int i_nal_ref_idc = (p_frag->p_buffer[4] >> 5)&0x03;
    const int i_nal_type = p_frag->p_buffer[4]&0x1f;
    const mtime_t i_frag_dts = p_frag->i_dts;
    const mtime_t i_frag_pts = p_frag->i_pts;

    if( p_sys->b_slice && ( !p_sys->b_sps || !p_sys->b_pps ) )
    {
        block_ChainRelease( p_sys->p_frame );
        msg_Warn( p_dec, "waiting for SPS/PPS" );

        /* Reset context */
        p_sys->slice.i_frame_type = 0;
        p_sys->p_frame = NULL;
        p_sys->b_frame_sps = false;
        p_sys->b_frame_pps = false;
        p_sys->b_slice = false;
        cc_Flush( &p_sys->cc_next );
    }

    if( ( !p_sys->b_sps || !p_sys->b_pps ) &&
        i_nal_type >= NAL_SLICE && i_nal_type <= NAL_SLICE_IDR )
    {
        p_sys->b_slice = true;
        /* Fragment will be discarded later on */
    }
    else if( i_nal_type >= NAL_SLICE && i_nal_type <= NAL_SLICE_IDR )
    {
        slice_t slice;
        bool  b_new_picture;

        ParseSlice( p_dec, &b_new_picture, &slice, i_nal_ref_idc, i_nal_type, p_frag );

        /* */
        if( b_new_picture && p_sys->b_slice )
            p_pic = OutputPicture( p_dec );

        /* */
        p_sys->slice = slice;
        p_sys->b_slice = true;
    }
    else if( i_nal_type == NAL_SPS )
    {
        if( p_sys->b_slice )
            p_pic = OutputPicture( p_dec );
        p_sys->b_frame_sps = true;

        PutSPS( p_dec, p_frag );

        /* Do not append the SPS because we will insert it on keyframes */
        p_frag = NULL;
    }
    else if( i_nal_type == NAL_PPS )
    {
        if( p_sys->b_slice )
            p_pic = OutputPicture( p_dec );
        p_sys->b_frame_pps = true;

        PutPPS( p_dec, p_frag );

        /* Do not append the PPS because we will insert it on keyframes */
        p_frag = NULL;
    }
    else if( i_nal_type == NAL_AU_DELIMITER ||
             i_nal_type == NAL_SEI ||
             ( i_nal_type >= 13 && i_nal_type <= 18 ) )
    {
        if( p_sys->b_slice )
            p_pic = OutputPicture( p_dec );

        /* Parse SEI for CC support */
        if( i_nal_type == NAL_SEI )
        {
            ParseSei( p_dec, p_frag );
        }
        else if( i_nal_type == NAL_AU_DELIMITER )
        {
            if( p_sys->p_frame && (p_sys->p_frame->i_flags & BLOCK_FLAG_PRIVATE_AUD) )
            {
                block_Release( p_frag );
                p_frag = NULL;
            }
            else
            {
                p_frag->i_flags |= BLOCK_FLAG_PRIVATE_AUD;
            }
        }
    }

    /* Append the block */
    if( p_frag )
        block_ChainAppend( &p_sys->p_frame, p_frag );

    *pb_ts_used = false;
    if( p_sys->i_frame_dts <= VLC_TS_INVALID &&
        p_sys->i_frame_pts <= VLC_TS_INVALID )
    {
        p_sys->i_frame_dts = i_frag_dts;
        p_sys->i_frame_pts = i_frag_pts;
        *pb_ts_used = true;
    }
    return p_pic;
}

static block_t *OutputPicture( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_pic;

    if ( !p_sys->b_header && p_sys->i_recovery_frames != -1 )
    {
        if( p_sys->i_recovery_frames == 0 )
        {
            msg_Dbg( p_dec, "Recovery from SEI recovery point complete" );
            p_sys->b_header = true;
        }
        --p_sys->i_recovery_frames;
    }

    if( !p_sys->b_header && p_sys->i_recovery_frames == -1 &&
         p_sys->slice.i_frame_type != BLOCK_FLAG_TYPE_I)
        return NULL;

    const bool b_sps_pps_i = p_sys->slice.i_frame_type == BLOCK_FLAG_TYPE_I &&
                             p_sys->b_sps &&
                             p_sys->b_pps;
    if( b_sps_pps_i || p_sys->b_frame_sps || p_sys->b_frame_pps )
    {
        block_t *p_head = NULL;
        if( p_sys->p_frame->i_flags & BLOCK_FLAG_PRIVATE_AUD )
        {
            p_head = p_sys->p_frame;
            p_sys->p_frame = p_sys->p_frame->p_next;
        }

        block_t *p_list = NULL;
        for( int i = 0; i < SPS_MAX && (b_sps_pps_i || p_sys->b_frame_sps); i++ )
        {
            if( p_sys->pp_sps[i] )
                block_ChainAppend( &p_list, block_Duplicate( p_sys->pp_sps[i] ) );
        }
        for( int i = 0; i < PPS_MAX && (b_sps_pps_i || p_sys->b_frame_pps); i++ )
        {
            if( p_sys->pp_pps[i] )
                block_ChainAppend( &p_list, block_Duplicate( p_sys->pp_pps[i] ) );
        }
        if( b_sps_pps_i && p_list )
            p_sys->b_header = true;

        if( p_head )
            p_head->p_next = p_list;
        else
            p_head = p_list;
        block_ChainAppend( &p_head, p_sys->p_frame );

        p_pic = block_ChainGather( p_head );
    }
    else
    {
        p_pic = block_ChainGather( p_sys->p_frame );
    }

    unsigned i_num_clock_ts = 1;
    if( p_sys->b_frame_mbs_only == 0 && p_sys->b_pic_struct_present_flag )
    {
        if( p_sys->i_pic_struct < 9 )
        {
            const uint8_t rgi_numclock[9] = { 1, 1, 1, 2, 2, 3, 3, 2, 3 };
            i_num_clock_ts = rgi_numclock[ p_sys->i_pic_struct ];
        }
    }

    if( p_sys->i_time_scale )
    {
        p_pic->i_length = CLOCK_FREQ * i_num_clock_ts *
                          p_sys->i_num_units_in_tick / p_sys->i_time_scale;
    }

    if( p_sys->b_frame_mbs_only == 0 && p_sys->b_pic_struct_present_flag )
    {
        switch( p_sys->i_pic_struct )
        {
        case 1:
        case 2:
            if( !p_sys->b_even_frame )
            {
                p_pic->i_flags |= (p_sys->i_pic_struct == 1) ? BLOCK_FLAG_TOP_FIELD_FIRST
                                                             : BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            }
            else if( p_pic->i_pts <= VLC_TS_INVALID && p_sys->i_prev_pts > VLC_TS_INVALID )
            {
                /* interpolate from even frame */
                p_pic->i_pts = p_sys->i_prev_pts + p_pic->i_length;
            }
            p_sys->b_even_frame = !p_sys->b_even_frame;
            break;
        case 3:
            p_pic->i_flags |= BLOCK_FLAG_TOP_FIELD_FIRST;
            p_sys->b_even_frame = false;
            break;
        case 4:
            p_pic->i_flags |= BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            p_sys->b_even_frame = false;
            break;
        case 5:
            p_pic->i_flags |= BLOCK_FLAG_TOP_FIELD_FIRST;
            break;
        case 6:
            p_pic->i_flags |= BLOCK_FLAG_BOTTOM_FIELD_FIRST;
            break;
        default:
            p_sys->b_even_frame = false;
            break;
        }
    }

    if( p_sys->i_frame_dts <= VLC_TS_INVALID )
        p_sys->i_frame_dts = p_sys->i_prev_dts;

    p_pic->i_dts = p_sys->i_frame_dts;
    p_pic->i_pts = p_sys->i_frame_pts;
    p_pic->i_flags |= p_sys->slice.i_frame_type;
    p_pic->i_flags &= ~BLOCK_FLAG_PRIVATE_AUD;
    if( !p_sys->b_header )
        p_pic->i_flags |= BLOCK_FLAG_PREROLL;

    p_sys->i_prev_dts = p_sys->i_frame_dts;
    p_sys->i_prev_pts = p_sys->i_frame_pts;
    p_sys->i_frame_dts = VLC_TS_INVALID;
    p_sys->i_frame_pts = VLC_TS_INVALID;

    p_sys->slice.i_frame_type = 0;
    p_sys->p_frame = NULL;
    p_sys->b_frame_sps = false;
    p_sys->b_frame_pps = false;
    p_sys->b_slice = false;

    /* CC */
    p_sys->i_cc_pts = p_pic->i_pts;
    p_sys->i_cc_dts = p_pic->i_dts;
    p_sys->i_cc_flags = p_pic->i_flags;

    p_sys->cc = p_sys->cc_next;
    cc_Flush( &p_sys->cc_next );

    return p_pic;
}

static void PutSPS( decoder_t *p_dec, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    struct nal_sps sps;

    if( h264_parse_sps( p_frag->p_buffer, p_frag->i_buffer, &sps ) != 0 )
    {
        msg_Warn( p_dec, "invalid SPS (sps_id=%d)", sps.i_id );
        block_Release( p_frag );
        return;
    }

    p_dec->fmt_out.i_profile = sps.i_profile;
    p_dec->fmt_out.i_level = sps.i_level;
    p_dec->fmt_out.video.i_width  = sps.i_width;
    p_dec->fmt_out.video.i_height = sps.i_height;
    if( sps.vui.i_sar_num != 0 && sps.vui.i_sar_den != 0 )
    {
        p_dec->fmt_out.video.i_sar_num = sps.vui.i_sar_num;
        p_dec->fmt_out.video.i_sar_den = sps.vui.i_sar_den;
    }

    p_sys->i_log2_max_frame_num = sps.i_log2_max_frame_num;
    p_sys->b_frame_mbs_only = sps.b_frame_mbs_only;
    p_sys->i_pic_order_cnt_type = sps.i_pic_order_cnt_type;
    p_sys->i_delta_pic_order_always_zero_flag = sps.i_delta_pic_order_always_zero_flag;
    p_sys->i_log2_max_pic_order_cnt_lsb = sps.i_log2_max_pic_order_cnt_lsb;

    if( sps.vui.b_valid )
    {
        p_sys->b_timing_info_present_flag = sps.vui.b_timing_info_present_flag;
        p_sys->i_num_units_in_tick =  sps.vui.i_num_units_in_tick;
        p_sys->i_time_scale = sps.vui.i_time_scale;
        p_sys->b_fixed_frame_rate = sps.vui.b_fixed_frame_rate;
        p_sys->b_pic_struct_present_flag = sps.vui.b_pic_struct_present_flag;
        p_sys->b_cpb_dpb_delays_present_flag = sps.vui.b_cpb_dpb_delays_present_flag;
        p_sys->i_cpb_removal_delay_length_minus1 = sps.vui.i_cpb_removal_delay_length_minus1;
        p_sys->i_dpb_output_delay_length_minus1 = sps.vui.i_dpb_output_delay_length_minus1;
    }

    /* We have a new SPS */
    if( !p_sys->b_sps )
        msg_Dbg( p_dec, "found NAL_SPS (sps_id=%d)", sps.i_id );
    p_sys->b_sps = true;

    if( p_sys->pp_sps[sps.i_id] )
        block_Release( p_sys->pp_sps[sps.i_id] );
    p_sys->pp_sps[sps.i_id] = p_frag;
}

static void PutPPS( decoder_t *p_dec, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    struct nal_pps pps;

    if( h264_parse_pps( p_frag->p_buffer, p_frag->i_buffer, &pps ) != 0 )
    {
        msg_Warn( p_dec, "invalid PPS (pps_id=%d sps_id=%d)", pps.i_id, pps.i_sps_id );
        block_Release( p_frag );
        return;
    }
    p_sys->i_pic_order_present_flag = pps.i_pic_order_present_flag;

    /* We have a new PPS */
    if( !p_sys->b_pps )
        msg_Dbg( p_dec, "found NAL_PPS (pps_id=%d sps_id=%d)", pps.i_id, pps.i_sps_id );
    p_sys->b_pps = true;

    if( p_sys->pp_pps[pps.i_id] )
        block_Release( p_sys->pp_pps[pps.i_id] );
    p_sys->pp_pps[pps.i_id] = p_frag;
}

static void ParseSlice( decoder_t *p_dec, bool *pb_new_picture, slice_t *p_slice,
                        int i_nal_ref_idc, int i_nal_type, const block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    uint8_t *pb_dec;
    int i_dec;
    int i_slice_type;
    slice_t slice;
    bs_t s;

    /* do not convert the whole frame */
    CreateDecodedNAL( &pb_dec, &i_dec, &p_frag->p_buffer[5],
                     __MIN( p_frag->i_buffer - 5, 60 ) );
    bs_init( &s, pb_dec, i_dec );

    /* first_mb_in_slice */
    /* int i_first_mb = */ bs_read_ue( &s );

    /* slice_type */
    switch( (i_slice_type = bs_read_ue( &s )) )
    {
    case 0: case 5:
        slice.i_frame_type = BLOCK_FLAG_TYPE_P;
        break;
    case 1: case 6:
        slice.i_frame_type = BLOCK_FLAG_TYPE_B;
        break;
    case 2: case 7:
        slice.i_frame_type = BLOCK_FLAG_TYPE_I;
        break;
    case 3: case 8: /* SP */
        slice.i_frame_type = BLOCK_FLAG_TYPE_P;
        break;
    case 4: case 9:
        slice.i_frame_type = BLOCK_FLAG_TYPE_I;
        break;
    default:
        slice.i_frame_type = 0;
        break;
    }

    /* */
    slice.i_nal_type = i_nal_type;
    slice.i_nal_ref_idc = i_nal_ref_idc;

    slice.i_pic_parameter_set_id = bs_read_ue( &s );
    slice.i_frame_num = bs_read( &s, p_sys->i_log2_max_frame_num + 4 );

    slice.i_field_pic_flag = 0;
    slice.i_bottom_field_flag = -1;
    if( !p_sys->b_frame_mbs_only )
    {
        /* field_pic_flag */
        slice.i_field_pic_flag = bs_read( &s, 1 );
        if( slice.i_field_pic_flag )
            slice.i_bottom_field_flag = bs_read( &s, 1 );
    }

    slice.i_idr_pic_id = p_sys->slice.i_idr_pic_id;
    if( slice.i_nal_type == NAL_SLICE_IDR )
        slice.i_idr_pic_id = bs_read_ue( &s );

    slice.i_pic_order_cnt_lsb = -1;
    slice.i_delta_pic_order_cnt_bottom = -1;
    slice.i_delta_pic_order_cnt0 = 0;
    slice.i_delta_pic_order_cnt1 = 0;
    if( p_sys->i_pic_order_cnt_type == 0 )
    {
        slice.i_pic_order_cnt_lsb = bs_read( &s, p_sys->i_log2_max_pic_order_cnt_lsb + 4 );
        if( p_sys->i_pic_order_present_flag && !slice.i_field_pic_flag )
            slice.i_delta_pic_order_cnt_bottom = bs_read_se( &s );
    }
    else if( (p_sys->i_pic_order_cnt_type == 1) &&
             (!p_sys->i_delta_pic_order_always_zero_flag) )
    {
        slice.i_delta_pic_order_cnt0 = bs_read_se( &s );
        if( p_sys->i_pic_order_present_flag && !slice.i_field_pic_flag )
            slice.i_delta_pic_order_cnt1 = bs_read_se( &s );
    }
    free( pb_dec );

    /* Detection of the first VCL NAL unit of a primary coded picture
     * (cf. 7.4.1.2.4) */
    bool b_pic = false;
    if( slice.i_frame_num != p_sys->slice.i_frame_num ||
        slice.i_pic_parameter_set_id != p_sys->slice.i_pic_parameter_set_id ||
        slice.i_field_pic_flag != p_sys->slice.i_field_pic_flag ||
        !slice.i_nal_ref_idc != !p_sys->slice.i_nal_ref_idc )
        b_pic = true;
    if( (slice.i_bottom_field_flag != -1) &&
        (p_sys->slice.i_bottom_field_flag != -1) &&
        (slice.i_bottom_field_flag != p_sys->slice.i_bottom_field_flag) )
        b_pic = true;
    if( p_sys->i_pic_order_cnt_type == 0 &&
        ( slice.i_pic_order_cnt_lsb != p_sys->slice.i_pic_order_cnt_lsb ||
          slice.i_delta_pic_order_cnt_bottom != p_sys->slice.i_delta_pic_order_cnt_bottom ) )
        b_pic = true;
    else if( p_sys->i_pic_order_cnt_type == 1 &&
             ( slice.i_delta_pic_order_cnt0 != p_sys->slice.i_delta_pic_order_cnt0 ||
               slice.i_delta_pic_order_cnt1 != p_sys->slice.i_delta_pic_order_cnt1 ) )
        b_pic = true;
    if( ( slice.i_nal_type == NAL_SLICE_IDR || p_sys->slice.i_nal_type == NAL_SLICE_IDR ) &&
        ( slice.i_nal_type != p_sys->slice.i_nal_type || slice.i_idr_pic_id != p_sys->slice.i_idr_pic_id ) )
            b_pic = true;

    /* */
    *pb_new_picture = b_pic;
    *p_slice = slice;
}

static void ParseSei( decoder_t *p_dec, block_t *p_frag )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    uint8_t *pb_dec;
    int i_dec;

    /* */
    CreateDecodedNAL( &pb_dec, &i_dec, &p_frag->p_buffer[5], p_frag->i_buffer - 5 );
    if( !pb_dec )
        return;

    /* The +1 is for rbsp trailing bits */
    for( int i_used = 0; i_used+1 < i_dec; )
    {
        /* Read type */
        int i_type = 0;
        while( i_used+1 < i_dec )
        {
            const int i_byte = pb_dec[i_used++];
            i_type += i_byte;
            if( i_byte != 0xff )
                break;
        }
        /* Read size */
        int i_size = 0;
        while( i_used+1 < i_dec )
        {
            const int i_byte = pb_dec[i_used++];
            i_size += i_byte;
            if( i_byte != 0xff )
                break;
        }
        /* Check room */
        if( i_used + i_size + 1 > i_dec )
            break;

        /* Look for pic timing */
        if( i_type == SEI_PIC_TIMING )
        {
            bs_t s;
            const int      i_tim = i_size;
            const uint8_t *p_tim = &pb_dec[i_used];

            bs_init( &s, p_tim, i_tim );

            if( p_sys->b_cpb_dpb_delays_present_flag )
            {
                bs_read( &s, p_sys->i_cpb_removal_delay_length_minus1 + 1 );
                bs_read( &s, p_sys->i_dpb_output_delay_length_minus1 + 1 );
            }

            if( p_sys->b_pic_struct_present_flag )
                p_sys->i_pic_struct = bs_read( &s, 4 );
            /* + unparsed remains */
        }

        /* Look for user_data_registered_itu_t_t35 */
        if( i_type == SEI_USER_DATA_REGISTERED )
        {
            static const uint8_t p_dvb1_data_start_code[] = {
                0xb5,
                0x00, 0x31,
                0x47, 0x41, 0x39, 0x34
            };
            const int      i_t35 = i_size;
            const uint8_t *p_t35 = &pb_dec[i_used];

            /* Check for we have DVB1_data() */
            if( i_t35 >= 5 &&
                !memcmp( p_t35, p_dvb1_data_start_code, sizeof(p_dvb1_data_start_code) ) )
            {
                cc_Extract( &p_sys->cc_next, true, &p_t35[3], i_t35 - 3 );
            }
        }

        /* Look for SEI recovery point */
        if( i_type == SEI_RECOVERY_POINT )
        {
            bs_t s;
            const int      i_rec = i_size;
            const uint8_t *p_rec = &pb_dec[i_used];

            bs_init( &s, p_rec, i_rec );
            int i_recovery_frames = bs_read_ue( &s );
            //bool b_exact_match = bs_read( &s, 1 );
            //bool b_broken_link = bs_read( &s, 1 );
            //int i_changing_slice_group = bs_read( &s, 2 );
            if( !p_sys->b_header )
            {
                msg_Dbg( p_dec, "Seen SEI recovery point, %d recovery frames", i_recovery_frames );
                if ( p_sys->i_recovery_frames == -1 || i_recovery_frames < p_sys->i_recovery_frames )
                    p_sys->i_recovery_frames = i_recovery_frames;
            }
        }

        i_used += i_size;
    }

    free( pb_dec );
}

