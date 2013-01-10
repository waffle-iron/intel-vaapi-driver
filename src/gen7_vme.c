/*
 * Copyright © 2010-2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Zhao Yakui <yakui.zhao@intel.com>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "intel_batchbuffer.h"
#include "intel_driver.h"

#include "i965_defines.h"
#include "i965_drv_video.h"
#include "i965_encoder.h"
#include "gen6_vme.h"
#include "gen6_mfc.h"
#ifdef SURFACE_STATE_PADDED_SIZE
#undef SURFACE_STATE_PADDED_SIZE
#endif

#define VME_MSG_LENGTH		32
#define SURFACE_STATE_PADDED_SIZE_0_GEN7        ALIGN(sizeof(struct gen7_surface_state), 32)
#define SURFACE_STATE_PADDED_SIZE_1_GEN7        ALIGN(sizeof(struct gen7_surface_state2), 32)
#define SURFACE_STATE_PADDED_SIZE_GEN7          MAX(SURFACE_STATE_PADDED_SIZE_0_GEN7, SURFACE_STATE_PADDED_SIZE_1_GEN7)

#define SURFACE_STATE_PADDED_SIZE               SURFACE_STATE_PADDED_SIZE_GEN7
#define SURFACE_STATE_OFFSET(index)             (SURFACE_STATE_PADDED_SIZE * index)
#define BINDING_TABLE_OFFSET(index)             (SURFACE_STATE_OFFSET(MAX_MEDIA_SURFACES_GEN6) + sizeof(unsigned int) * index)

#define CURBE_ALLOCATION_SIZE   37              /* in 256-bit */
#define CURBE_TOTAL_DATA_LENGTH (4 * 32)        /* in byte, it should be less than or equal to CURBE_ALLOCATION_SIZE * 32 */
#define CURBE_URB_ENTRY_LENGTH  4               /* in 256-bit, it should be less than or equal to CURBE_TOTAL_DATA_LENGTH / 32 */

enum VIDEO_CODING_TYPE{
    VIDEO_CODING_AVC = 0,
    VIDEO_CODING_MPEG2,
    VIDEO_CODING_SUM
};

enum AVC_VME_KERNEL_TYPE{ 
    AVC_VME_INTRA_SHADER = 0,
    AVC_VME_INTER_SHADER,
    AVC_VME_BATCHBUFFER,
    AVC_VME_KERNEL_SUM
};

enum MPEG2_VME_KERNEL_TYPE{
    MPEG2_VME_INTER_SHADER = 0,
    MPEG2_VME_BATCHBUFFER,
    MPEG2_VME_KERNEL_SUM
};
 
static const uint32_t gen7_vme_intra_frame[][4] = {
#include "shaders/vme/intra_frame_ivb.g7b"
};

static const uint32_t gen7_vme_inter_frame[][4] = {
#include "shaders/vme/inter_frame_ivb.g7b"
};

static const uint32_t gen7_vme_batchbuffer[][4] = {
#include "shaders/vme/batchbuffer.g7b"
};

static struct i965_kernel gen7_vme_kernels[] = {
    {
        "AVC VME Intra Frame",
        AVC_VME_INTRA_SHADER,			/*index*/
        gen7_vme_intra_frame, 			
        sizeof(gen7_vme_intra_frame),		
        NULL
    },
    {
        "AVC VME inter Frame",
        AVC_VME_INTER_SHADER,
        gen7_vme_inter_frame,
        sizeof(gen7_vme_inter_frame),
        NULL
    },
    {
        "AVC VME BATCHBUFFER",
        AVC_VME_BATCHBUFFER,
        gen7_vme_batchbuffer,
        sizeof(gen7_vme_batchbuffer),
        NULL
    },
};

static const uint32_t gen7_vme_mpeg2_inter_frame[][4] = {
#include "shaders/vme/mpeg2_inter_frame.g7b"
};

static const uint32_t gen7_vme_mpeg2_batchbuffer[][4] = {
#include "shaders/vme/batchbuffer.g7b"
};

static struct i965_kernel gen7_vme_mpeg2_kernels[] = {
    {
        "MPEG2 VME inter Frame",
        MPEG2_VME_INTER_SHADER,
        gen7_vme_mpeg2_inter_frame,
        sizeof(gen7_vme_mpeg2_inter_frame),
        NULL
    },
    {
        "MPEG2 VME BATCHBUFFER",
        MPEG2_VME_BATCHBUFFER,
        gen7_vme_mpeg2_batchbuffer,
        sizeof(gen7_vme_mpeg2_batchbuffer),
        NULL
    },
};

/* only used for VME source surface state */
static void 
gen7_vme_source_surface_state(VADriverContextP ctx,
                              int index,
                              struct object_surface *obj_surface,
                              struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;

    vme_context->vme_surface2_setup(ctx,
                                    &vme_context->gpe_context,
                                    obj_surface,
                                    BINDING_TABLE_OFFSET(index),
                                    SURFACE_STATE_OFFSET(index));
}

static void
gen7_vme_media_source_surface_state(VADriverContextP ctx,
                                    int index,
                                    struct object_surface *obj_surface,
                                    struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;

    vme_context->vme_media_rw_surface_setup(ctx,
                                            &vme_context->gpe_context,
                                            obj_surface,
                                            BINDING_TABLE_OFFSET(index),
                                            SURFACE_STATE_OFFSET(index));
}

static void
gen7_vme_output_buffer_setup(VADriverContextP ctx,
                             struct encode_state *encode_state,
                             int index,
                             struct intel_encoder_context *encoder_context)

{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    VAEncSliceParameterBufferH264 *pSliceParameter = (VAEncSliceParameterBufferH264 *)encode_state->slice_params_ext[0]->buffer;
    int is_intra = pSliceParameter->slice_type == SLICE_TYPE_I;
    int width_in_mbs = pSequenceParameter->picture_width_in_mbs;
    int height_in_mbs = pSequenceParameter->picture_height_in_mbs;

    vme_context->vme_output.num_blocks = width_in_mbs * height_in_mbs;
    vme_context->vme_output.pitch = 16; /* in bytes, always 16 */

    if (is_intra)
        vme_context->vme_output.size_block = INTRA_VME_OUTPUT_IN_BYTES;
    else
        vme_context->vme_output.size_block = INTER_VME_OUTPUT_IN_BYTES;

    vme_context->vme_output.bo = dri_bo_alloc(i965->intel.bufmgr, 
                                              "VME output buffer",
                                              vme_context->vme_output.num_blocks * vme_context->vme_output.size_block,
                                              0x1000);
    assert(vme_context->vme_output.bo);
    vme_context->vme_buffer_suface_setup(ctx,
                                         &vme_context->gpe_context,
                                         &vme_context->vme_output,
                                         BINDING_TABLE_OFFSET(index),
                                         SURFACE_STATE_OFFSET(index));
}

static void
gen7_vme_output_vme_batchbuffer_setup(VADriverContextP ctx,
                                      struct encode_state *encode_state,
                                      int index,
                                      struct intel_encoder_context *encoder_context)

{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    int width_in_mbs = pSequenceParameter->picture_width_in_mbs;
    int height_in_mbs = pSequenceParameter->picture_height_in_mbs;

    vme_context->vme_batchbuffer.num_blocks = width_in_mbs * height_in_mbs + 1;
    vme_context->vme_batchbuffer.size_block = 32; /* 2 OWORDs */
    vme_context->vme_batchbuffer.pitch = 16;
    vme_context->vme_batchbuffer.bo = dri_bo_alloc(i965->intel.bufmgr, 
                                                   "VME batchbuffer",
                                                   vme_context->vme_batchbuffer.num_blocks * vme_context->vme_batchbuffer.size_block,
                                                   0x1000);
    vme_context->vme_buffer_suface_setup(ctx,
                                         &vme_context->gpe_context,
                                         &vme_context->vme_batchbuffer,
                                         BINDING_TABLE_OFFSET(index),
                                         SURFACE_STATE_OFFSET(index));
}

static VAStatus
gen7_vme_surface_setup(VADriverContextP ctx, 
                       struct encode_state *encode_state,
                       int is_intra,
                       struct intel_encoder_context *encoder_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface;
    VAEncPictureParameterBufferH264 *pPicParameter = (VAEncPictureParameterBufferH264 *)encode_state->pic_param_ext->buffer;

    /*Setup surfaces state*/
    /* current picture for encoding */
    obj_surface = SURFACE(encoder_context->input_yuv_surface);
    assert(obj_surface);
    gen7_vme_source_surface_state(ctx, 0, obj_surface, encoder_context);
    gen7_vme_media_source_surface_state(ctx, 4, obj_surface, encoder_context);

    if (!is_intra) {
        /* reference 0 */
        obj_surface = SURFACE(pPicParameter->ReferenceFrames[0].picture_id);
        assert(obj_surface);
        if ( obj_surface->bo != NULL)
            gen7_vme_source_surface_state(ctx, 1, obj_surface, encoder_context);

        /* reference 1 */
        obj_surface = SURFACE(pPicParameter->ReferenceFrames[1].picture_id);
        assert(obj_surface);
        if ( obj_surface->bo != NULL ) 
            gen7_vme_source_surface_state(ctx, 2, obj_surface, encoder_context);
    }

    /* VME output */
    gen7_vme_output_buffer_setup(ctx, encode_state, 3, encoder_context);
    gen7_vme_output_vme_batchbuffer_setup(ctx, encode_state, 5, encoder_context);

    return VA_STATUS_SUCCESS;
}

static VAStatus gen7_vme_interface_setup(VADriverContextP ctx, 
                                         struct encode_state *encode_state,
                                         struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    struct gen6_interface_descriptor_data *desc;   
    int i;
    dri_bo *bo;

    bo = vme_context->gpe_context.idrt.bo;
    dri_bo_map(bo, 1);
    assert(bo->virtual);
    desc = bo->virtual;

    for (i = 0; i < vme_context->vme_kernel_sum; i++) {
        struct i965_kernel *kernel;
        kernel = &vme_context->gpe_context.kernels[i];
        assert(sizeof(*desc) == 32);
        /*Setup the descritor table*/
        memset(desc, 0, sizeof(*desc));
        desc->desc0.kernel_start_pointer = (kernel->bo->offset >> 6);
        desc->desc2.sampler_count = 1; /* FIXME: */
        desc->desc2.sampler_state_pointer = (vme_context->vme_state.bo->offset >> 5);
        desc->desc3.binding_table_entry_count = 1; /* FIXME: */
        desc->desc3.binding_table_pointer = (BINDING_TABLE_OFFSET(0) >> 5);
        desc->desc4.constant_urb_entry_read_offset = 0;
        desc->desc4.constant_urb_entry_read_length = CURBE_URB_ENTRY_LENGTH;
 		
        /*kernel start*/
        dri_bo_emit_reloc(bo,	
                          I915_GEM_DOMAIN_INSTRUCTION, 0,
                          0,
                          i * sizeof(*desc) + offsetof(struct gen6_interface_descriptor_data, desc0),
                          kernel->bo);
        /*Sampler State(VME state pointer)*/
        dri_bo_emit_reloc(bo,
                          I915_GEM_DOMAIN_INSTRUCTION, 0,
                          (1 << 2),									//
                          i * sizeof(*desc) + offsetof(struct gen6_interface_descriptor_data, desc2),
                          vme_context->vme_state.bo);
        desc++;
    }
    dri_bo_unmap(bo);

    return VA_STATUS_SUCCESS;
}

static VAStatus gen7_vme_constant_setup(VADriverContextP ctx, 
                                        struct encode_state *encode_state,
                                        struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    // unsigned char *constant_buffer;
    unsigned int *vme_state_message;
    int mv_num = 32;
    if (vme_context->h264_level >= 30) {
	mv_num = 16;
	if (vme_context->h264_level >= 31)
		mv_num = 8;
    } 

    dri_bo_map(vme_context->gpe_context.curbe.bo, 1);
    assert(vme_context->gpe_context.curbe.bo->virtual);
    // constant_buffer = vme_context->curbe.bo->virtual;
    vme_state_message = (unsigned int *)vme_context->gpe_context.curbe.bo->virtual;
    vme_state_message[31] = mv_num;
	
    /*TODO copy buffer into CURB*/

    dri_bo_unmap( vme_context->gpe_context.curbe.bo);

    return VA_STATUS_SUCCESS;
}

static const unsigned int intra_mb_mode_cost_table[] = {
    0x31110001, // for qp0
    0x09110001, // for qp1
    0x15030001, // for qp2
    0x0b030001, // for qp3
    0x0d030011, // for qp4
    0x17210011, // for qp5
    0x41210011, // for qp6
    0x19210011, // for qp7
    0x25050003, // for qp8
    0x1b130003, // for qp9
    0x1d130003, // for qp10
    0x27070021, // for qp11
    0x51310021, // for qp12
    0x29090021, // for qp13
    0x35150005, // for qp14
    0x2b0b0013, // for qp15
    0x2d0d0013, // for qp16
    0x37170007, // for qp17
    0x61410031, // for qp18
    0x39190009, // for qp19
    0x45250015, // for qp20
    0x3b1b000b, // for qp21
    0x3d1d000d, // for qp22
    0x47270017, // for qp23
    0x71510041, // for qp24 ! center for qp=0..30
    0x49290019, // for qp25
    0x55350025, // for qp26
    0x4b2b001b, // for qp27
    0x4d2d001d, // for qp28
    0x57370027, // for qp29
    0x81610051, // for qp30
    0x57270017, // for qp31
    0x81510041, // for qp32 ! center for qp=31..51
    0x59290019, // for qp33
    0x65350025, // for qp34
    0x5b2b001b, // for qp35
    0x5d2d001d, // for qp36
    0x67370027, // for qp37
    0x91610051, // for qp38
    0x69390029, // for qp39
    0x75450035, // for qp40
    0x6b3b002b, // for qp41
    0x6d3d002d, // for qp42
    0x77470037, // for qp43
    0xa1710061, // for qp44
    0x79490039, // for qp45
    0x85550045, // for qp46
    0x7b4b003b, // for qp47
    0x7d4d003d, // for qp48
    0x87570047, // for qp49
    0xb1810071, // for qp50
    0x89590049  // for qp51
};

static void gen7_vme_state_setup_fixup(VADriverContextP ctx,
                                       struct encode_state *encode_state,
                                       struct intel_encoder_context *encoder_context,
                                       unsigned int *vme_state_message)
{
    struct gen6_mfc_context *mfc_context = encoder_context->mfc_context;
    VAEncPictureParameterBufferH264 *pic_param = (VAEncPictureParameterBufferH264 *)encode_state->pic_param_ext->buffer;
    VAEncSliceParameterBufferH264 *slice_param = (VAEncSliceParameterBufferH264 *)encode_state->slice_params_ext[0]->buffer;

    if (slice_param->slice_type != SLICE_TYPE_I &&
        slice_param->slice_type != SLICE_TYPE_SI)
        return;
    if (encoder_context->rate_control_mode == VA_RC_CQP)
        vme_state_message[16] = intra_mb_mode_cost_table[pic_param->pic_init_qp + slice_param->slice_qp_delta];
    else
        vme_state_message[16] = intra_mb_mode_cost_table[mfc_context->bit_rate_control_context[slice_param->slice_type].QpPrimeY];
}

static VAStatus gen7_vme_avc_state_setup(VADriverContextP ctx,
                                         struct encode_state *encode_state,
                                         int is_intra,
                                         struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    unsigned int *vme_state_message;
	unsigned int *mb_cost_table;
    int i;

	mb_cost_table = (unsigned int *)vme_context->vme_state_message;
    //building VME state message
    dri_bo_map(vme_context->vme_state.bo, 1);
    assert(vme_context->vme_state.bo->virtual);
    vme_state_message = (unsigned int *)vme_context->vme_state.bo->virtual;

    vme_state_message[0] = 0x01010101;
    vme_state_message[1] = 0x10010101;
    vme_state_message[2] = 0x0F0F0F0F;
    vme_state_message[3] = 0x100F0F0F;
    vme_state_message[4] = 0x01010101;
    vme_state_message[5] = 0x10010101;
    vme_state_message[6] = 0x0F0F0F0F;
    vme_state_message[7] = 0x100F0F0F;
    vme_state_message[8] = 0x01010101;
    vme_state_message[9] = 0x10010101;
    vme_state_message[10] = 0x0F0F0F0F;
    vme_state_message[11] = 0x000F0F0F;
    vme_state_message[12] = 0x00;
    vme_state_message[13] = 0x00;

    vme_state_message[14] = (mb_cost_table[2] & 0xFFFF);
    vme_state_message[15] = 0;
    vme_state_message[16] = mb_cost_table[0];
    vme_state_message[17] = mb_cost_table[1];
    vme_state_message[18] = mb_cost_table[3];
    vme_state_message[19] = mb_cost_table[4];

    for(i = 20; i < 32; i++) {
        vme_state_message[i] = 0;
    }

    dri_bo_unmap( vme_context->vme_state.bo);
    return VA_STATUS_SUCCESS;
}

static VAStatus gen7_vme_vme_state_setup(VADriverContextP ctx,
                                         struct encode_state *encode_state,
                                         int is_intra,
                                         struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    unsigned int *vme_state_message;
    int i;
	
    //building VME state message
    dri_bo_map(vme_context->vme_state.bo, 1);
    assert(vme_context->vme_state.bo->virtual);
    vme_state_message = (unsigned int *)vme_context->vme_state.bo->virtual;

    vme_state_message[0] = 0x01010101;
    vme_state_message[1] = 0x10010101;
    vme_state_message[2] = 0x0F0F0F0F;
    vme_state_message[3] = 0x100F0F0F;
    vme_state_message[4] = 0x01010101;
    vme_state_message[5] = 0x10010101;
    vme_state_message[6] = 0x0F0F0F0F;
    vme_state_message[7] = 0x100F0F0F;
    vme_state_message[8] = 0x01010101;
    vme_state_message[9] = 0x10010101;
    vme_state_message[10] = 0x0F0F0F0F;
    vme_state_message[11] = 0x000F0F0F;
    vme_state_message[12] = 0x00;
    vme_state_message[13] = 0x00;

    vme_state_message[14] = 0x4a4a;
    vme_state_message[15] = 0x0;
    vme_state_message[16] = 0x4a4a4a4a;
    vme_state_message[17] = 0x4a4a4a4a;
    vme_state_message[18] = 0x21110100;
    vme_state_message[19] = 0x61514131;

    for(i = 20; i < 32; i++) {
        vme_state_message[i] = 0;
    }
    //vme_state_message[16] = 0x42424242;			//cost function LUT set 0 for Intra

    gen7_vme_state_setup_fixup(ctx, encode_state, encoder_context, vme_state_message);

    dri_bo_unmap( vme_context->vme_state.bo);
    return VA_STATUS_SUCCESS;
}

#define		INTRA_PRED_AVAIL_FLAG_AE	0x60
#define		INTRA_PRED_AVAIL_FLAG_B		0x10
#define		INTRA_PRED_AVAIL_FLAG_C       	0x8
#define		INTRA_PRED_AVAIL_FLAG_D		0x4
#define		INTRA_PRED_AVAIL_FLAG_BCD_MASK	0x1C

static void
gen7_vme_fill_vme_batchbuffer(VADriverContextP ctx, 
                              struct encode_state *encode_state,
                              int mb_width, int mb_height,
                              int kernel,
                              int transform_8x8_mode_flag,
                              struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    int mb_x = 0, mb_y = 0;
    int i, s, j;
    unsigned int *command_ptr;


    dri_bo_map(vme_context->vme_batchbuffer.bo, 1);
    command_ptr = vme_context->vme_batchbuffer.bo->virtual;

    for (s = 0; s < encode_state->num_slice_params_ext; s++) {
        VAEncSliceParameterBufferMPEG2 *slice_param = (VAEncSliceParameterBufferMPEG2 *)encode_state->slice_params_ext[s]->buffer;

        for (j = 0; j < encode_state->slice_params_ext[s]->num_elements; j++) {
            int slice_mb_begin = slice_param->macroblock_address;
            int slice_mb_number = slice_param->num_macroblocks;
            unsigned int mb_intra_ub;
            int slice_mb_x = slice_param->macroblock_address % mb_width;

            for (i = 0; i < slice_mb_number;) {
                int mb_count = i + slice_mb_begin;    

                mb_x = mb_count % mb_width;
                mb_y = mb_count / mb_width;
                mb_intra_ub = 0;

                if (mb_x != 0) {
                    mb_intra_ub |= INTRA_PRED_AVAIL_FLAG_AE;
                }

                if (mb_y != 0) {
                    mb_intra_ub |= INTRA_PRED_AVAIL_FLAG_B;

                    if (mb_x != 0)
                        mb_intra_ub |= INTRA_PRED_AVAIL_FLAG_D;

                    if (mb_x != (mb_width -1))
                        mb_intra_ub |= INTRA_PRED_AVAIL_FLAG_C;
                }

                if (i < mb_width) {
                    if (i == 0)
                        mb_intra_ub &= ~(INTRA_PRED_AVAIL_FLAG_AE);

                    mb_intra_ub &= ~(INTRA_PRED_AVAIL_FLAG_BCD_MASK);

                    if ((i == (mb_width - 1)) && slice_mb_x) {
                        mb_intra_ub |= INTRA_PRED_AVAIL_FLAG_C;
                    }
                }
		
                if ((i == mb_width) && slice_mb_x) {
                    mb_intra_ub &= ~(INTRA_PRED_AVAIL_FLAG_D);
                }

                *command_ptr++ = (CMD_MEDIA_OBJECT | (8 - 2));
                *command_ptr++ = kernel;
                *command_ptr++ = 0;
                *command_ptr++ = 0;
                *command_ptr++ = 0;
                *command_ptr++ = 0;
   
                /*inline data */
                *command_ptr++ = (mb_width << 16 | mb_y << 8 | mb_x);
                *command_ptr++ = ( (1 << 16) | transform_8x8_mode_flag | (mb_intra_ub << 8));

                i += 1;
            }

            slice_param++;
        }
    }

    *command_ptr++ = 0;
    *command_ptr++ = MI_BATCH_BUFFER_END;

    dri_bo_unmap(vme_context->vme_batchbuffer.bo);
}

static void gen7_vme_media_init(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    dri_bo *bo;

    i965_gpe_context_init(ctx, &vme_context->gpe_context);

    /* VME output buffer */
    dri_bo_unreference(vme_context->vme_output.bo);
    vme_context->vme_output.bo = NULL;

    dri_bo_unreference(vme_context->vme_batchbuffer.bo);
    vme_context->vme_batchbuffer.bo = NULL;

    /* VME state */
    dri_bo_unreference(vme_context->vme_state.bo);
    bo = dri_bo_alloc(i965->intel.bufmgr,
                      "Buffer",
                      1024*16, 64);
    assert(bo);
    vme_context->vme_state.bo = bo;
}

static void gen7_vme_pipeline_programing(VADriverContextP ctx, 
                                         struct encode_state *encode_state,
                                         struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    VAEncPictureParameterBufferH264 *pPicParameter = (VAEncPictureParameterBufferH264 *)encode_state->pic_param_ext->buffer;
    VAEncSliceParameterBufferH264 *pSliceParameter = (VAEncSliceParameterBufferH264 *)encode_state->slice_params_ext[0]->buffer;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    int is_intra = pSliceParameter->slice_type == SLICE_TYPE_I;
    int width_in_mbs = pSequenceParameter->picture_width_in_mbs;
    int height_in_mbs = pSequenceParameter->picture_height_in_mbs;

    gen7_vme_fill_vme_batchbuffer(ctx, 
                                  encode_state,
                                  width_in_mbs, height_in_mbs,
                                  is_intra ? AVC_VME_INTRA_SHADER : AVC_VME_INTER_SHADER, 
                                  pPicParameter->pic_fields.bits.transform_8x8_mode_flag,
                                  encoder_context);

    intel_batchbuffer_start_atomic(batch, 0x1000);
    gen6_gpe_pipeline_setup(ctx, &vme_context->gpe_context, batch);
    BEGIN_BATCH(batch, 2);
    OUT_BATCH(batch, MI_BATCH_BUFFER_START | (2 << 6));
    OUT_RELOC(batch,
              vme_context->vme_batchbuffer.bo,
              I915_GEM_DOMAIN_COMMAND, 0, 
              0);
    ADVANCE_BATCH(batch);

    intel_batchbuffer_end_atomic(batch);	
}

static VAStatus gen7_vme_prepare(VADriverContextP ctx, 
                                 struct encode_state *encode_state,
                                 struct intel_encoder_context *encoder_context)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;
    VAEncSliceParameterBufferH264 *pSliceParameter = (VAEncSliceParameterBufferH264 *)encode_state->slice_params_ext[0]->buffer;
    int is_intra = pSliceParameter->slice_type == SLICE_TYPE_I;
    VAEncSequenceParameterBufferH264 *pSequenceParameter = (VAEncSequenceParameterBufferH264 *)encode_state->seq_param_ext->buffer;
    struct gen6_vme_context *vme_context = encoder_context->vme_context;

    if (!vme_context->h264_level ||
		(vme_context->h264_level != pSequenceParameter->level_idc)) {
	vme_context->h264_level = pSequenceParameter->level_idc;	
    }
	
    intel_vme_update_mbmv_cost(ctx, encode_state, encoder_context);
    /*Setup all the memory object*/
    gen7_vme_surface_setup(ctx, encode_state, is_intra, encoder_context);
    gen7_vme_interface_setup(ctx, encode_state, encoder_context);
    gen7_vme_constant_setup(ctx, encode_state, encoder_context);
    gen7_vme_avc_state_setup(ctx, encode_state, is_intra, encoder_context);

    /*Programing media pipeline*/
    gen7_vme_pipeline_programing(ctx, encode_state, encoder_context);

    return vaStatus;
}

static VAStatus gen7_vme_run(VADriverContextP ctx, 
                             struct encode_state *encode_state,
                             struct intel_encoder_context *encoder_context)
{
    struct intel_batchbuffer *batch = encoder_context->base.batch;

    intel_batchbuffer_flush(batch);

    return VA_STATUS_SUCCESS;
}

static VAStatus gen7_vme_stop(VADriverContextP ctx, 
                              struct encode_state *encode_state,
                              struct intel_encoder_context *encoder_context)
{
    return VA_STATUS_SUCCESS;
}

static VAStatus
gen7_vme_pipeline(VADriverContextP ctx,
                  VAProfile profile,
                  struct encode_state *encode_state,
                  struct intel_encoder_context *encoder_context)
{
    gen7_vme_media_init(ctx, encoder_context);
    gen7_vme_prepare(ctx, encode_state, encoder_context);
    gen7_vme_run(ctx, encode_state, encoder_context);
    gen7_vme_stop(ctx, encode_state, encoder_context);

    return VA_STATUS_SUCCESS;
}

static void
gen7_vme_mpeg2_output_buffer_setup(VADriverContextP ctx,
                                    struct encode_state *encode_state,
                                    int index,
                                    int is_intra,
                                    struct intel_encoder_context *encoder_context)

{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    VAEncSequenceParameterBufferMPEG2 *seq_param = (VAEncSequenceParameterBufferMPEG2 *)encode_state->seq_param_ext->buffer;
    int width_in_mbs = ALIGN(seq_param->picture_width, 16) / 16;
    int height_in_mbs = ALIGN(seq_param->picture_height, 16) / 16;

    vme_context->vme_output.num_blocks = width_in_mbs * height_in_mbs;
    vme_context->vme_output.pitch = 16; /* in bytes, always 16 */

    if (is_intra)
        vme_context->vme_output.size_block = INTRA_VME_OUTPUT_IN_BYTES;
    else
        vme_context->vme_output.size_block = INTER_VME_OUTPUT_IN_BYTES;

    vme_context->vme_output.bo = dri_bo_alloc(i965->intel.bufmgr,
                                              "VME output buffer",
                                              vme_context->vme_output.num_blocks * vme_context->vme_output.size_block,
                                              0x1000);
    assert(vme_context->vme_output.bo);
    vme_context->vme_buffer_suface_setup(ctx,
                                         &vme_context->gpe_context,
                                         &vme_context->vme_output,
                                         BINDING_TABLE_OFFSET(index),
                                         SURFACE_STATE_OFFSET(index));
}

static void
gen7_vme_mpeg2_output_vme_batchbuffer_setup(VADriverContextP ctx,
                                             struct encode_state *encode_state,
                                             int index,
                                             struct intel_encoder_context *encoder_context)

{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    VAEncSequenceParameterBufferMPEG2 *seq_param = (VAEncSequenceParameterBufferMPEG2 *)encode_state->seq_param_ext->buffer;
    int width_in_mbs = ALIGN(seq_param->picture_width, 16) / 16;
    int height_in_mbs = ALIGN(seq_param->picture_height, 16) / 16;

    vme_context->vme_batchbuffer.num_blocks = width_in_mbs * height_in_mbs + 1;
    vme_context->vme_batchbuffer.size_block = 32; /* 4 OWORDs */
    vme_context->vme_batchbuffer.pitch = 16;
    vme_context->vme_batchbuffer.bo = dri_bo_alloc(i965->intel.bufmgr, 
                                                   "VME batchbuffer",
                                                   vme_context->vme_batchbuffer.num_blocks * vme_context->vme_batchbuffer.size_block,
                                                   0x1000);
    vme_context->vme_buffer_suface_setup(ctx,
                                         &vme_context->gpe_context,
                                         &vme_context->vme_batchbuffer,
                                         BINDING_TABLE_OFFSET(index),
                                         SURFACE_STATE_OFFSET(index));
}

static VAStatus
gen7_vme_mpeg2_surface_setup(VADriverContextP ctx, 
                              struct encode_state *encode_state,
                              int is_intra,
                              struct intel_encoder_context *encoder_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface;
    VAEncPictureParameterBufferMPEG2 *pic_param = (VAEncPictureParameterBufferMPEG2 *)encode_state->pic_param_ext->buffer;

    /*Setup surfaces state*/
    /* current picture for encoding */
    obj_surface = SURFACE(encoder_context->input_yuv_surface);
    assert(obj_surface);
    gen7_vme_source_surface_state(ctx, 0, obj_surface, encoder_context);
    gen7_vme_media_source_surface_state(ctx, 4, obj_surface, encoder_context);

    if (!is_intra) {
        /* reference 0 */
        obj_surface = SURFACE(pic_param->forward_reference_picture);
        assert(obj_surface);
        if ( obj_surface->bo != NULL)
            gen7_vme_source_surface_state(ctx, 1, obj_surface, encoder_context);

        /* reference 1 */
        obj_surface = SURFACE(pic_param->backward_reference_picture);
        if (obj_surface && obj_surface->bo != NULL) 
            gen7_vme_source_surface_state(ctx, 2, obj_surface, encoder_context);
    }

    /* VME output */
    gen7_vme_mpeg2_output_buffer_setup(ctx, encode_state, 3, is_intra, encoder_context);
    gen7_vme_mpeg2_output_vme_batchbuffer_setup(ctx, encode_state, 5, encoder_context);

    return VA_STATUS_SUCCESS;
}

static void
gen7_vme_mpeg2_fill_vme_batchbuffer(VADriverContextP ctx,
                                     struct encode_state *encode_state,
                                     int mb_width, int mb_height,
                                     int kernel,
                                     int transform_8x8_mode_flag,
                                     struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    int number_mb_cmds;
    int mb_x = 0, mb_y = 0;
    int i, s, j;
    unsigned int *command_ptr;

    dri_bo_map(vme_context->vme_batchbuffer.bo, 1);
    command_ptr = vme_context->vme_batchbuffer.bo->virtual;

    for (s = 0; s < encode_state->num_slice_params_ext; s++) {
        VAEncSliceParameterBufferMPEG2 *slice_param = (VAEncSliceParameterBufferMPEG2 *)encode_state->slice_params_ext[s]->buffer;

        for (j = 0; j < encode_state->slice_params_ext[s]->num_elements; j++) {
            int slice_mb_begin = slice_param->macroblock_address;
            int slice_mb_number = slice_param->num_macroblocks;

            for (i = 0; i < slice_mb_number;) {
                int mb_count = i + slice_mb_begin;

                mb_x = mb_count % mb_width;
                mb_y = mb_count / mb_width;

                if( i == 0) {
                    number_mb_cmds = mb_width;
                } else if ((i + 128) <= slice_mb_number) {
                    number_mb_cmds = 128;
                } else {
                    number_mb_cmds = slice_mb_number - i;
                }

                *command_ptr++ = (CMD_MEDIA_OBJECT | (8 - 2));
                *command_ptr++ = kernel;
                *command_ptr++ = 0;
                *command_ptr++ = 0;
                *command_ptr++ = 0;
                *command_ptr++ = 0;
 
                /*inline data */
                *command_ptr++ = (mb_width << 16 | mb_y << 8 | mb_x);
                *command_ptr++ = ( (number_mb_cmds << 16) | transform_8x8_mode_flag | ((i == 0) << 1));

                i += number_mb_cmds;
            }

            slice_param++;
        }
    }

    *command_ptr++ = 0;
    *command_ptr++ = MI_BATCH_BUFFER_END;

    dri_bo_unmap(vme_context->vme_batchbuffer.bo);
}

static void
gen7_vme_mpeg2_pipeline_programing(VADriverContextP ctx, 
                                    struct encode_state *encode_state,
                                    int is_intra,
                                    struct intel_encoder_context *encoder_context)
{
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    struct intel_batchbuffer *batch = encoder_context->base.batch;
    VAEncSequenceParameterBufferMPEG2 *seq_param = (VAEncSequenceParameterBufferMPEG2 *)encode_state->seq_param_ext->buffer;
    int width_in_mbs = ALIGN(seq_param->picture_width, 16) / 16;
    int height_in_mbs = ALIGN(seq_param->picture_height, 16) / 16;

    gen7_vme_mpeg2_fill_vme_batchbuffer(ctx, 
                                         encode_state,
                                         width_in_mbs, height_in_mbs,
                                         MPEG2_VME_INTER_SHADER,
                                         0,
                                         encoder_context);

    intel_batchbuffer_start_atomic(batch, 0x1000);
    gen6_gpe_pipeline_setup(ctx, &vme_context->gpe_context, batch);
    BEGIN_BATCH(batch, 2);
    OUT_BATCH(batch, MI_BATCH_BUFFER_START | (2 << 6));
    OUT_RELOC(batch,
              vme_context->vme_batchbuffer.bo,
              I915_GEM_DOMAIN_COMMAND, 0, 
              0);
    ADVANCE_BATCH(batch);

    intel_batchbuffer_end_atomic(batch);
}

static VAStatus
gen7_vme_mpeg2_prepare(VADriverContextP ctx, 
                        struct encode_state *encode_state,
                        struct intel_encoder_context *encoder_context)
{
    VAStatus vaStatus = VA_STATUS_SUCCESS;

   /*Setup all the memory object*/
    gen7_vme_mpeg2_surface_setup(ctx, encode_state, 0, encoder_context);
    gen7_vme_interface_setup(ctx, encode_state, encoder_context);
    gen7_vme_vme_state_setup(ctx, encode_state, 0, encoder_context);
    gen7_vme_constant_setup(ctx, encode_state, encoder_context);

    /*Programing media pipeline*/
    gen7_vme_mpeg2_pipeline_programing(ctx, encode_state, 0, encoder_context);

    return vaStatus;
}

static VAStatus
gen7_vme_mpeg2_pipeline(VADriverContextP ctx,
                         VAProfile profile,
                         struct encode_state *encode_state,
                         struct intel_encoder_context *encoder_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_vme_context *vme_context = encoder_context->vme_context;
    VAEncSliceParameterBufferMPEG2 *slice_param = 
        (VAEncSliceParameterBufferMPEG2 *)encode_state->slice_params_ext[0]->buffer;
    VAEncSequenceParameterBufferMPEG2 *seq_param = 
       (VAEncSequenceParameterBufferMPEG2 *)encode_state->seq_param_ext->buffer;
 
    /*No need of to exec VME for Intra slice */
    if (slice_param->is_intra_slice) {
         if(!vme_context->vme_output.bo) {
             int w_in_mbs = ALIGN(seq_param->picture_width, 16) / 16;
             int h_in_mbs = ALIGN(seq_param->picture_height, 16) / 16;

             vme_context->vme_output.num_blocks = w_in_mbs * h_in_mbs;
             vme_context->vme_output.pitch = 16; /* in bytes, always 16 */
             vme_context->vme_output.size_block = INTRA_VME_OUTPUT_IN_BYTES;
             vme_context->vme_output.bo = dri_bo_alloc(i965->intel.bufmgr,
                                                       "MPEG2 VME output buffer",
                                                       vme_context->vme_output.num_blocks
                                                           * vme_context->vme_output.size_block,
                                                       0x1000);
         }

         return VA_STATUS_SUCCESS;
    }

    gen7_vme_media_init(ctx, encoder_context);
    gen7_vme_mpeg2_prepare(ctx, encode_state, encoder_context);
    gen7_vme_run(ctx, encode_state, encoder_context);
    gen7_vme_stop(ctx, encode_state, encoder_context);

    return VA_STATUS_SUCCESS;
}

static void
gen7_vme_context_destroy(void *context)
{
    struct gen6_vme_context *vme_context = context;

    i965_gpe_context_destroy(&vme_context->gpe_context);

    dri_bo_unreference(vme_context->vme_output.bo);
    vme_context->vme_output.bo = NULL;

    dri_bo_unreference(vme_context->vme_state.bo);
    vme_context->vme_state.bo = NULL;

    dri_bo_unreference(vme_context->vme_batchbuffer.bo);
    vme_context->vme_batchbuffer.bo = NULL;

    if (vme_context->vme_state_message) {
	free(vme_context->vme_state_message);
	vme_context->vme_state_message = NULL;
    }

    free(vme_context);
}

Bool gen7_vme_context_init(VADriverContextP ctx, struct intel_encoder_context *encoder_context)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct gen6_vme_context *vme_context = calloc(1, sizeof(struct gen6_vme_context));

    vme_context->gpe_context.surface_state_binding_table.length =
              (SURFACE_STATE_PADDED_SIZE + sizeof(unsigned int)) * MAX_MEDIA_SURFACES_GEN6;

    vme_context->gpe_context.idrt.max_entries = MAX_INTERFACE_DESC_GEN6;
    vme_context->gpe_context.idrt.entry_size = sizeof(struct gen6_interface_descriptor_data);
    vme_context->gpe_context.curbe.length = CURBE_TOTAL_DATA_LENGTH;

    vme_context->gpe_context.vfe_state.max_num_threads = 60 - 1;
    vme_context->gpe_context.vfe_state.num_urb_entries = 16;
    vme_context->gpe_context.vfe_state.gpgpu_mode = 0;
    vme_context->gpe_context.vfe_state.urb_entry_size = 59 - 1;
    vme_context->gpe_context.vfe_state.curbe_allocation_size = CURBE_ALLOCATION_SIZE - 1;

    if(encoder_context->profile == VAProfileH264Baseline ||
       encoder_context->profile == VAProfileH264Main     ||
       encoder_context->profile == VAProfileH264High ){
       vme_context->video_coding_type = VIDEO_CODING_AVC;
       vme_context->vme_kernel_sum = AVC_VME_KERNEL_SUM; 
 
    } else if (encoder_context->profile == VAProfileMPEG2Simple ||
               encoder_context->profile == VAProfileMPEG2Main ){
       vme_context->video_coding_type = VIDEO_CODING_MPEG2;
       vme_context->vme_kernel_sum = MPEG2_VME_KERNEL_SUM; 
    } else {
        /* Unsupported encoding profile */
        assert(0);
    }

    if (IS_GEN7(i965->intel.device_id)) {
        if (vme_context->video_coding_type == VIDEO_CODING_AVC) {
              i965_gpe_load_kernels(ctx,
                                    &vme_context->gpe_context,
                                    gen7_vme_kernels,
                                    vme_context->vme_kernel_sum);
              encoder_context->vme_pipeline = gen7_vme_pipeline;
 
        } else {
              i965_gpe_load_kernels(ctx,
                                    &vme_context->gpe_context,
                                    gen7_vme_mpeg2_kernels,
                                    vme_context->vme_kernel_sum);
              encoder_context->vme_pipeline = gen7_vme_mpeg2_pipeline;
 
        }

        vme_context->vme_surface2_setup = gen7_gpe_surface2_setup;
        vme_context->vme_media_rw_surface_setup = gen7_gpe_media_rw_surface_setup;
        vme_context->vme_buffer_suface_setup = gen7_gpe_buffer_suface_setup;
    }

    encoder_context->vme_context = vme_context;
    encoder_context->vme_context_destroy = gen7_vme_context_destroy;
    vme_context->vme_state_message = malloc(VME_MSG_LENGTH * sizeof(int));

    return True;
}