// Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause

`include "axi/typedef.svh"

module axi_slave_fixture (
  input logic clk_i, input logic rst_ni,
  input logic [3:0] awid_i, input logic [31:0] awaddr_i,
  input logic [7:0] awlen_i, input logic [2:0] awsize_i,
  input logic [1:0] awburst_i, input logic awlock_i,
  input logic [3:0] awcache_i, input logic [2:0] awprot_i,
  input logic [3:0] awregion_i, input logic [3:0] awqos_i,
  input logic [1:0] awuser_i, input logic awvalid_i, output logic awready_o,
  input logic [31:0] wdata_i, input logic [3:0] wstrb_i,
  input logic wlast_i, input logic [1:0] wuser_i,
  input logic wvalid_i, output logic wready_o,
  output logic [3:0] bid_o, output logic [1:0] bresp_o,
  output logic [1:0] buser_o, output logic bvalid_o, input logic bready_i,
  input logic [3:0] arid_i, input logic [31:0] araddr_i,
  input logic [7:0] arlen_i, input logic [2:0] arsize_i,
  input logic [1:0] arburst_i, input logic arlock_i,
  input logic [3:0] arcache_i, input logic [2:0] arprot_i,
  input logic [3:0] arregion_i, input logic [3:0] arqos_i,
  input logic [1:0] aruser_i, input logic arvalid_i, output logic arready_o,
  output logic [3:0] rid_o, output logic [31:0] rdata_o,
  output logic [1:0] rresp_o, output logic rlast_o,
  output logic [1:0] ruser_o, output logic rvalid_o, input logic rready_i,
  output logic idle_o
);
  typedef logic [31:0] addr_t;
  typedef logic [3:0] id_t;
  typedef logic [31:0] data_t;
  typedef logic [3:0] strb_t;
  typedef logic [1:0] user_t;
  `AXI_TYPEDEF_ALL(fixture_axi, addr_t, id_t, data_t, strb_t, user_t)

  fixture_axi_req_t request, sim_request;
  fixture_axi_resp_t sim_response;
  fixture_axi_w_chan_t w_buffer_q;
  logic w_buffer_valid_q, w_send_valid_q;

  assign request = '{
    aw: '{id: awid_i, addr: awaddr_i, len: awlen_i, size: awsize_i,
          burst: awburst_i, lock: awlock_i, cache: awcache_i,
          prot: awprot_i, qos: awqos_i, region: awregion_i,
          atop: 6'b0, user: awuser_i, default: '0},
    aw_valid: awvalid_i,
    w: '{data: wdata_i, strb: wstrb_i, last: wlast_i, user: wuser_i},
    w_valid: wvalid_i,
    b_ready: bready_i,
    ar: '{id: arid_i, addr: araddr_i, len: arlen_i, size: arsize_i,
          burst: arburst_i, lock: arlock_i, cache: arcache_i,
          prot: arprot_i, qos: arqos_i, region: arregion_i,
          user: aruser_i, default: '0},
    ar_valid: arvalid_i,
    r_ready: rready_i
  };

  always_comb begin
    sim_request = request;
    sim_request.w = w_buffer_q;
    sim_request.w_valid = w_buffer_valid_q & w_send_valid_q;
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      w_buffer_q <= '0;
      w_buffer_valid_q <= 1'b0;
      w_send_valid_q <= 1'b0;
    end else begin
      if (w_buffer_valid_q && w_send_valid_q && sim_response.w_ready) begin
        w_buffer_valid_q <= 1'b0;
        w_send_valid_q <= 1'b0;
      end
      if (w_buffer_valid_q && !w_send_valid_q)
        w_send_valid_q <= 1'b1;
      if (!w_buffer_valid_q && request.w_valid) begin
        w_buffer_q <= request.w;
        w_buffer_valid_q <= 1'b1;
        w_send_valid_q <= 1'b0;
      end
    end
  end

  axi_sim_mem #(
    .AddrWidth         (32),
    .DataWidth         (32),
    .IdWidth           (4),
    .UserWidth         (2),
    .NumPorts          (1),
    .axi_req_t         (fixture_axi_req_t),
    .axi_rsp_t         (fixture_axi_resp_t),
    .WarnUninitialized (1'b0),
    .UninitializedData ("zeros"),
    .ApplDelay         (0ns),
    .AcqDelay          (0ns)
  ) i_pulp_axi_sim_mem (
    .clk_i,
    .rst_ni,
    .axi_req_i         (sim_request),
    .axi_rsp_o         (sim_response),
    .mon_w_valid_o     (),
    .mon_w_addr_o      (),
    .mon_w_data_o      (),
    .mon_w_id_o        (),
    .mon_w_user_o      (),
    .mon_w_beat_count_o(),
    .mon_w_last_o      (),
    .mon_r_valid_o     (),
    .mon_r_addr_o      (),
    .mon_r_data_o      (),
    .mon_r_id_o        (),
    .mon_r_user_o      (),
    .mon_r_beat_count_o(),
    .mon_r_last_o      ()
  );

  assign awready_o = sim_response.aw_ready;
  assign wready_o = ~w_buffer_valid_q;
  assign bid_o = sim_response.b.id;
  assign bresp_o = sim_response.b.resp;
  assign buser_o = sim_response.b.user;
  assign bvalid_o = sim_response.b_valid;
  assign arready_o = sim_response.ar_ready;
  assign rid_o = sim_response.r.id;
  assign rdata_o = sim_response.r.data;
  assign rresp_o = sim_response.r.resp;
  assign rlast_o = sim_response.r.last;
  assign ruser_o = sim_response.r.user;
  assign rvalid_o = sim_response.r_valid;
  assign idle_o = ~(awvalid_i | wvalid_i | arvalid_i |
                    sim_response.b_valid | sim_response.r_valid |
                    w_buffer_valid_q);
endmodule
