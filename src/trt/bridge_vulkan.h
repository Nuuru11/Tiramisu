// tiramisu/src/trt/bridge_vulkan.h
// Vulkan 1.0-1.3 + WSI (VK_KHR_surface/win32_surface/swapchain) -> HLSL/D3D11
// translation bridge and host hook interface.
#ifndef TIRAMISU_BRIDGE_VULKAN_H
#define TIRAMISU_BRIDGE_VULKAN_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

struct trt_vk_vert_bind {
  u32 binding;
  u32 stride;
  u32 input_rate; // 0 = VK_VERTEX_INPUT_RATE_VERTEX, 1 = INSTANCE
};

struct trt_vk_vert_attr {
  u32 location;
  u32 binding;
  u32 vk_format;
  u32 offset;
};

struct trt_vk_pipeline_desc {
  u64 pipeline_id;
  u8 is_compute;
  u8 depth_test;
  u8 depth_write;
  u8 blend_en;
  u8 color_mask;
  u8 pad[3];
  const char* vs_hlsl;
  u32 vs_len;
  const char* ps_hlsl;
  u32 ps_len;
  const char* cs_hlsl;
  u32 cs_len;
  const trt_vk_vert_bind* vtx_binds;
  u32 vtx_bind_count;
  const trt_vk_vert_attr* vtx_attrs;
  u32 vtx_attr_count;
  u32 topology;     // VkPrimitiveTopology (0..4)
  u32 polygon_mode; // 0=FILL, 1=LINE
  u32 cull_mode;    // 0=NONE, 1=FRONT, 2=BACK
  u32 front_face;   // 0=CCW, 1=CW
  u32 depth_op;     // VkCompareOp
  u32 src_rgb;
  u32 dst_rgb;
  u32 op_rgb;
  u32 src_a;
  u32 dst_a;
  u32 op_a;
};

struct trt_vk_image_desc {
  u32 img_type; // 0=1D, 1=2D, 2=3D
  u32 vk_format;
  u32 w;
  u32 h;
  u32 depth;
  u32 mips;
  u32 layers;
  u32 usage;
  u32 flags;
  u8 is_swapchain;
  u8 pad[3];
};

struct trt_vk_desc_binding {
  u32 kind;     // spirv_binding_kind (0=CBV,1=SRV_BUF,2=UAV_BUF,3=TEX,4=SMP,5=COMBINED,6=STORAGE_IMG)
  u32 stage_mask;
  u32 hlsl_reg; // bN / tN / uN / sN
  u32 smp_reg;
  u64 buf_id;
  u64 buf_off;
  u64 buf_range;
  u64 img_id;
  u32 min_filter;
  u32 mag_filter;
  u32 addr_u;
  u32 addr_v;
};

struct trt_vk_rp_begin {
  u32 color_count;
  u64 color_imgs[8];
  u32 clear_color_mask;
  float clear_rgba[8][4];
  u64 depth_img;
  u8 clear_depth;
  u8 clear_stencil;
  u8 pad[2];
  float depth_val;
  u32 stencil_val;
  int area_x;
  int area_y;
  u32 area_w;
  u32 area_h;
};

struct trt_vk_draw_cmd {
  u64 pipeline_id;
  u8 indexed;
  u8 idx_is_u32;
  u8 has_vp;
  u8 has_sc;
  u64 ib_id;
  u64 ib_off;
  u32 vtx_count;
  u32 inst_count;
  u32 first_vtx_or_idx;
  int vtx_off;
  u32 first_inst;
  u32 vb_count;
  u64 vb_ids[8];
  u64 vb_offs[8];
  float vp_x, vp_y, vp_w, vp_h, vp_min_z, vp_max_z;
  int sc_x, sc_y;
  u32 sc_w, sc_h;
  float blend_const[4];
  const void* push_data;
  u32 push_bytes;
  u32 push_reg;
  const trt_vk_desc_binding* bindings;
  u32 binding_count;
};

struct trt_vk_dispatch_cmd {
  u64 pipeline_id;
  u32 gx, gy, gz;
  const void* push_data;
  u32 push_bytes;
  u32 push_reg;
  const trt_vk_desc_binding* bindings;
  u32 binding_count;
};

struct trt_vk_hooks {
  void* user;
  u32 (*create_pipeline)(const trt_vk_pipeline_desc* desc, void* user);
  void (*destroy_pipeline)(u64 pipeline_id, void* user);
  u32 (*sync_buffer)(u64 buf_id, u32 usage, const void* data, u64 bytes, void* user);
  void (*destroy_buffer)(u64 buf_id, void* user);
  u32 (*sync_image)(u64 img_id, const trt_vk_image_desc* desc, const void* data,
                    u64 bytes, u32 mip, u32 layer, void* user);
  void (*destroy_image)(u64 img_id, void* user);
  void (*begin_render_pass)(const trt_vk_rp_begin* rp, void* user);
  void (*end_render_pass)(void* user);
  void (*draw)(const trt_vk_draw_cmd* cmd, void* user);
  void (*dispatch)(const trt_vk_dispatch_cmd* cmd, void* user);
  void (*copy_buffer)(u64 src_buf, u64 dst_buf, u64 src_off, u64 dst_off,
                      u64 size, void* user);
  void (*copy_buf_to_img)(u64 src_buf, u64 dst_img, u64 buf_off, u32 w, u32 h,
                          u32 mip, u32 layer, const void* cpu_bytes,
                          u64 byte_len, void* user);
  void (*present)(u64 swapchain_id, u32 image_index, void* user);
};

int trt_bridge_register_vulkan(void);
void trt_vk_set_hooks(const trt_vk_hooks* hooks);

#ifdef __cplusplus
}
#endif

#endif
