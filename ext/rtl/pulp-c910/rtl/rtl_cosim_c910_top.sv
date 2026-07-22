// Copyright (c) 2026 The gem5 Authors.
// SPDX-License-Identifier: BSD-3-Clause

`include "axi/typedef.svh"

module rtl_cosim_c910_top (
  input  logic         clk_i,
  input  logic         rst_ni,
  input  logic         rtc_i,
  input  logic         ipi_i,
  input  logic         time_irq_i,
  input  logic [1:0]   plic_hartx_mint_req_i,
  input  logic [1:0]   plic_hartx_sint_req_i,
  input  logic         debug_req_i,
  input  logic [39:0]  ext_int_i,
  input  logic         jtag_tck_i,
  input  logic         jtag_tdi_i,
  input  logic         jtag_tms_i,
  output logic         jtag_tdo_o,
  output logic         jtag_tdo_en_o,
  input  logic         jtag_trst_ni,
  output logic [1:0]   lpmd_b_o,
  output logic         cosim_debug_mode_o,

  output logic [7:0]   axi_aw_id_o,
  output logic [39:0]  axi_aw_addr_o,
  output logic [7:0]   axi_aw_len_o,
  output logic [2:0]   axi_aw_size_o,
  output logic [1:0]   axi_aw_burst_o,
  output logic         axi_aw_lock_o,
  output logic [3:0]   axi_aw_cache_o,
  output logic [2:0]   axi_aw_prot_o,
  output logic [3:0]   axi_aw_region_o,
  output logic [3:0]   axi_aw_qos_o,
  output logic         axi_aw_user_o,
  output logic         axi_aw_valid_o,
  input  logic         axi_aw_ready_i,

  output logic [127:0] axi_w_data_o,
  output logic [15:0]  axi_w_strb_o,
  output logic         axi_w_last_o,
  output logic         axi_w_user_o,
  output logic         axi_w_valid_o,
  input  logic         axi_w_ready_i,

  input  logic [7:0]   axi_b_id_i,
  input  logic [1:0]   axi_b_resp_i,
  input  logic         axi_b_user_i,
  input  logic         axi_b_valid_i,
  output logic         axi_b_ready_o,

  output logic [7:0]   axi_ar_id_o,
  output logic [39:0]  axi_ar_addr_o,
  output logic [7:0]   axi_ar_len_o,
  output logic [2:0]   axi_ar_size_o,
  output logic [1:0]   axi_ar_burst_o,
  output logic         axi_ar_lock_o,
  output logic [3:0]   axi_ar_cache_o,
  output logic [2:0]   axi_ar_prot_o,
  output logic [3:0]   axi_ar_region_o,
  output logic [3:0]   axi_ar_qos_o,
  output logic         axi_ar_user_o,
  output logic         axi_ar_valid_o,
  input  logic         axi_ar_ready_i,

  input  logic [7:0]   axi_r_id_i,
  input  logic [127:0] axi_r_data_i,
  input  logic [1:0]   axi_r_resp_i,
  input  logic         axi_r_last_i,
  input  logic         axi_r_user_i,
  input  logic         axi_r_valid_i,
  output logic         axi_r_ready_o
);

  typedef logic [39:0]  axi_addr_t;
  typedef logic [127:0] axi_data_t;
  typedef logic [15:0]  axi_strb_t;
  typedef logic [7:0]   axi_id_t;
  typedef logic         axi_user_t;
  `AXI_TYPEDEF_ALL(c910_axi, axi_addr_t, axi_id_t, axi_data_t, axi_strb_t, axi_user_t)

  c910_axi_req_t axi_req;
  c910_axi_resp_t axi_rsp;

  assign axi_aw_id_o     = axi_req.aw.id;
  assign axi_aw_addr_o   = axi_req.aw.addr;
  assign axi_aw_len_o    = axi_req.aw.len;
  assign axi_aw_size_o   = axi_req.aw.size;
  assign axi_aw_burst_o  = axi_req.aw.burst;
  assign axi_aw_lock_o   = axi_req.aw.lock;
  assign axi_aw_cache_o  = axi_req.aw.cache;
  assign axi_aw_prot_o   = axi_req.aw.prot;
  assign axi_aw_region_o = axi_req.aw.region;
  assign axi_aw_qos_o    = axi_req.aw.qos;
  assign axi_aw_user_o   = axi_req.aw.user;
  assign axi_aw_valid_o  = axi_req.aw_valid;
  assign axi_rsp.aw_ready = axi_aw_ready_i;

  assign axi_w_data_o    = axi_req.w.data;
  assign axi_w_strb_o    = axi_req.w.strb;
  assign axi_w_last_o    = axi_req.w.last;
  assign axi_w_user_o    = axi_req.w.user;
  assign axi_w_valid_o   = axi_req.w_valid;
  assign axi_rsp.w_ready = axi_w_ready_i;

  assign axi_rsp.b.id    = axi_b_id_i;
  assign axi_rsp.b.resp  = axi_b_resp_i;
  assign axi_rsp.b.user  = axi_b_user_i;
  assign axi_rsp.b_valid = axi_b_valid_i;
  assign axi_b_ready_o   = axi_req.b_ready;

  assign axi_ar_id_o     = axi_req.ar.id;
  assign axi_ar_addr_o   = axi_req.ar.addr;
  assign axi_ar_len_o    = axi_req.ar.len;
  assign axi_ar_size_o   = axi_req.ar.size;
  assign axi_ar_burst_o  = axi_req.ar.burst;
  assign axi_ar_lock_o   = axi_req.ar.lock;
  assign axi_ar_cache_o  = axi_req.ar.cache;
  assign axi_ar_prot_o   = axi_req.ar.prot;
  assign axi_ar_region_o = axi_req.ar.region;
  assign axi_ar_qos_o    = axi_req.ar.qos;
  assign axi_ar_user_o   = axi_req.ar.user;
  assign axi_ar_valid_o  = axi_req.ar_valid;
  assign axi_rsp.ar_ready = axi_ar_ready_i;

  assign axi_rsp.r.id    = axi_r_id_i;
  assign axi_rsp.r.data  = axi_r_data_i;
  assign axi_rsp.r.resp  = axi_r_resp_i;
  assign axi_rsp.r.last  = axi_r_last_i;
  assign axi_rsp.r.user  = axi_r_user_i;
  assign axi_rsp.r_valid = axi_r_valid_i;
  assign axi_r_ready_o   = axi_req.r_ready;

  c910_axi_wrap #(
    .AddrWidth ( 40 ),
    .DataWidth ( 128 ),
    .IdWidth   ( 8 ),
    .UserWidth ( 1 ),
    .aw_chan_t ( c910_axi_aw_chan_t ),
    .w_chan_t  ( c910_axi_w_chan_t ),
    .b_chan_t  ( c910_axi_b_chan_t ),
    .ar_chan_t ( c910_axi_ar_chan_t ),
    .r_chan_t  ( c910_axi_r_chan_t ),
    .axi_req_t ( c910_axi_req_t ),
    .axi_rsp_t ( c910_axi_resp_t )
  ) i_c910_axi_wrap (
    .clk_i,
    .rst_ni,
    .rtc_i,
    .ipi_i,
    .time_irq_i,
    .plic_hartx_mint_req_i,
    .plic_hartx_sint_req_i,
    .debug_req_i,
    .ext_int_i,
    .jtag_tck_i,
    .jtag_tdi_i,
    .jtag_tms_i,
    .jtag_tdo_o,
    .jtag_tdo_en_o,
    .jtag_trst_ni,
    .axi_req_o ( axi_req ),
    .axi_rsp_i ( axi_rsp )
  );

  // The generated OpenC910 shell leaves its public core0_pad_lpmd_b monitor
  // undriven. Keep upstream pristine and surface the connected CP0 signal
  // through this integration-only probe.
  assign lpmd_b_o = i_c910_axi_wrap.cpu_sub_system_axi_i
      .x_rv_integration_platform.x_cpu_top.x_ct_top_0.cp0_biu_lpmd_b;
  assign cosim_debug_mode_o = i_c910_axi_wrap.cpu_sub_system_axi_i
      .x_rv_integration_platform.x_cpu_top.x_ct_top_0.rtu_yy_xx_dbgon;

endmodule
