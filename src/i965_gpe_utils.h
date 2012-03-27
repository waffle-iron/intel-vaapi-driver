/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Xiang Haihao <haihao.xiang@intel.com>
 */

#ifndef _I965_GPE_UTILS_H_
#define _I965_GPE_UTILS_H_

#include <i915_drm.h>
#include <intel_bufmgr.h>

#include "i965_defines.h"
#include "i965_drv_video.h"
#include "i965_structs.h"

#define MAX_GPE_KERNELS    32

struct i965_buffer_surface
{
    dri_bo *bo;
    unsigned int num_blocks;
    unsigned int size_block;
    unsigned int pitch;
};

struct i965_gpe_context
{
    struct {
        dri_bo *bo;
        unsigned int length;            /* in bytes */
    } surface_state_binding_table;

    struct {
        dri_bo *bo;
        unsigned int max_entries;
        unsigned int entry_size;        /* in bytes */
    } idrt;

    struct {
        dri_bo *bo;
        unsigned int length;            /* in bytes */
    } curbe;

    struct {
        unsigned int gpgpu_mode : 1;
        unsigned int pad0 : 7;
        unsigned int max_num_threads : 16;
        unsigned int num_urb_entries : 8;
        unsigned int urb_entry_size : 16;
        unsigned int curbe_allocation_size : 16;
    } vfe_state;

    unsigned int num_kernels;
    struct i965_kernel kernels[MAX_GPE_KERNELS];
};

void i965_gpe_context_destroy(struct i965_gpe_context *gpe_context);
void i965_gpe_context_init(VADriverContextP ctx,
                           struct i965_gpe_context *gpe_context);
void i965_gpe_load_kernels(VADriverContextP ctx,
                           struct i965_gpe_context *gpe_context,
                           struct i965_kernel *kernel_list,
                           unsigned int num_kernels);
void gen6_gpe_pipeline_setup(VADriverContextP ctx,
                             struct i965_gpe_context *gpe_context,
                             struct intel_batchbuffer *batch);
void i965_gpe_surface2_setup(VADriverContextP ctx,
                             struct i965_gpe_context *gpe_context,
                             struct object_surface *obj_surface,
                             unsigned long binding_table_offset,
                             unsigned long surface_state_offset);
void i965_gpe_media_rw_surface_setup(VADriverContextP ctx,
                                     struct i965_gpe_context *gpe_context,
                                     struct object_surface *obj_surface,
                                     unsigned long binding_table_offset,
                                     unsigned long surface_state_offset);
void i965_gpe_buffer_suface_setup(VADriverContextP ctx,
                                  struct i965_gpe_context *gpe_context,
                                  struct i965_buffer_surface *buffer_surface,
                                  unsigned long binding_table_offset,
                                  unsigned long surface_state_offset);
void gen7_gpe_surface2_setup(VADriverContextP ctx,
                             struct i965_gpe_context *gpe_context,
                             struct object_surface *obj_surface,
                             unsigned long binding_table_offset,
                             unsigned long surface_state_offset);
void gen7_gpe_media_rw_surface_setup(VADriverContextP ctx,
                                     struct i965_gpe_context *gpe_context,
                                     struct object_surface *obj_surface,
                                     unsigned long binding_table_offset,
                                     unsigned long surface_state_offset);
void gen7_gpe_buffer_suface_setup(VADriverContextP ctx,
                                  struct i965_gpe_context *gpe_context,
                                  struct i965_buffer_surface *buffer_surface,
                                  unsigned long binding_table_offset,
                                  unsigned long surface_state_offset);
#endif /* _I965_GPE_UTILS_H_ */