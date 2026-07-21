// Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause

`include "axi/typedef.svh"

module axi_master_fixture (
  input logic clk_i, input logic rst_ni,
  output logic [3:0] awid_o, output logic [31:0] awaddr_o,
  output logic [7:0] awlen_o, output logic [2:0] awsize_o,
  output logic [1:0] awburst_o, output logic awlock_o,
  output logic [3:0] awcache_o, output logic [2:0] awprot_o,
  output logic [3:0] awregion_o, output logic [3:0] awqos_o,
  output logic [1:0] awuser_o, output logic awvalid_o, input logic awready_i,
  output logic [31:0] wdata_o, output logic [3:0] wstrb_o,
  output logic wlast_o, output logic [1:0] wuser_o,
  output logic wvalid_o, input logic wready_i,
  input logic [3:0] bid_i, input logic [1:0] bresp_i,
  input logic [1:0] buser_i, input logic bvalid_i, output logic bready_o,
  output logic [3:0] arid_o, output logic [31:0] araddr_o,
  output logic [7:0] arlen_o, output logic [2:0] arsize_o,
  output logic [1:0] arburst_o, output logic arlock_o,
  output logic [3:0] arcache_o, output logic [2:0] arprot_o,
  output logic [3:0] arregion_o, output logic [3:0] arqos_o,
  output logic [1:0] aruser_o, output logic arvalid_o, input logic arready_i,
  input logic [3:0] rid_i, input logic [31:0] rdata_i,
  input logic [1:0] rresp_i, input logic rlast_i,
  input logic [1:0] ruser_i, input logic rvalid_i, output logic rready_o,
  output logic idle_o
);
  typedef logic [31:0] addr_t;
  typedef logic [3:0] id_t;
  typedef logic [31:0] data_t;
  typedef logic [3:0] strb_t;
  typedef logic [1:0] user_t;
  `AXI_TYPEDEF_ALL(fixture_axi, addr_t, id_t, data_t, strb_t, user_t)

  fixture_axi_req_t source_req, master_req;
  fixture_axi_resp_t source_resp, master_resp;
  logic [2:0] state_q;
  logic aw_done_q;
  logic [1:0] write_beat_q;

  axi_modify_address #(
    .slv_req_t  (fixture_axi_req_t),
    .mst_addr_t (addr_t),
    .mst_req_t  (fixture_axi_req_t),
    .axi_resp_t (fixture_axi_resp_t)
  ) i_pulp_axi_modify_address (
    .slv_req_i     (source_req),
    .slv_resp_o    (source_resp),
    .mst_aw_addr_i (source_req.aw.addr),
    .mst_ar_addr_i (source_req.ar.addr),
    .mst_req_o     (master_req),
    .mst_resp_i    (master_resp)
  );

  always_comb begin
    source_req = '0;
    source_req.aw = '{
      id: 4'd3, addr: 32'h0000_0040, len: 8'd1, size: 3'd2,
      burst: axi_pkg::BURST_INCR, lock: 1'b0, cache: 4'b0,
      prot: 3'b0, qos: 4'b0, region: 4'b0, atop: 6'b0,
      user: 2'b0, default: '0
    };
    source_req.w = '{
      data: write_beat_q == 0 ? 32'h0403_0201 : 32'h0807_0605,
      strb: 4'hf, last: write_beat_q == 1, user: 2'b0
    };
    source_req.ar = '{
      id: 4'd3, addr: 32'h0000_0040, len: 8'd1, size: 3'd2,
      burst: axi_pkg::BURST_INCR, lock: 1'b0, cache: 4'b0,
      prot: 3'b0, qos: 4'b0, region: 4'b0, user: 2'b0,
      default: '0
    };
    unique case (state_q)
      3'd0: begin
        source_req.aw_valid = ~aw_done_q;
        source_req.w_valid = write_beat_q < 2;
      end
      3'd1: source_req.b_ready = 1'b1;
      3'd2: source_req.ar_valid = 1'b1;
      3'd3: source_req.r_ready = 1'b1;
      default: source_req = '0;
    endcase
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q <= 3'd0;
      aw_done_q <= 1'b0;
      write_beat_q <= 2'd0;
    end else begin
      unique case (state_q)
        3'd0: begin
          if (source_req.aw_valid && source_resp.aw_ready)
            aw_done_q <= 1'b1;
          if (source_req.w_valid && source_resp.w_ready)
            write_beat_q <= write_beat_q + 1'b1;
          if ((aw_done_q || (source_req.aw_valid && source_resp.aw_ready)) &&
              (write_beat_q == 2 ||
               (write_beat_q == 1 && source_req.w_valid && source_resp.w_ready)))
            state_q <= 3'd1;
        end
        3'd1: if (source_resp.b_valid) state_q <= 3'd2;
        3'd2: if (source_resp.ar_ready) state_q <= 3'd3;
        3'd3: if (source_resp.r_valid && source_resp.r.last) state_q <= 3'd4;
        default: state_q <= state_q;
      endcase
    end
  end

  assign master_resp = '{
    aw_ready: awready_i, ar_ready: arready_i, w_ready: wready_i,
    b_valid: bvalid_i, b: '{id: bid_i, resp: bresp_i, user: buser_i},
    r_valid: rvalid_i,
    r: '{id: rid_i, data: rdata_i, resp: rresp_i, last: rlast_i, user: ruser_i}
  };
  assign awid_o = master_req.aw.id;
  assign awaddr_o = master_req.aw.addr;
  assign awlen_o = master_req.aw.len;
  assign awsize_o = master_req.aw.size;
  assign awburst_o = master_req.aw.burst;
  assign awlock_o = master_req.aw.lock;
  assign awcache_o = master_req.aw.cache;
  assign awprot_o = master_req.aw.prot;
  assign awregion_o = master_req.aw.region;
  assign awqos_o = master_req.aw.qos;
  assign awuser_o = master_req.aw.user;
  assign awvalid_o = master_req.aw_valid;
  assign wdata_o = master_req.w.data;
  assign wstrb_o = master_req.w.strb;
  assign wlast_o = master_req.w.last;
  assign wuser_o = master_req.w.user;
  assign wvalid_o = master_req.w_valid;
  assign bready_o = master_req.b_ready;
  assign arid_o = master_req.ar.id;
  assign araddr_o = master_req.ar.addr;
  assign arlen_o = master_req.ar.len;
  assign arsize_o = master_req.ar.size;
  assign arburst_o = master_req.ar.burst;
  assign arlock_o = master_req.ar.lock;
  assign arcache_o = master_req.ar.cache;
  assign arprot_o = master_req.ar.prot;
  assign arregion_o = master_req.ar.region;
  assign arqos_o = master_req.ar.qos;
  assign aruser_o = master_req.ar.user;
  assign arvalid_o = master_req.ar_valid;
  assign rready_o = master_req.r_ready;
  assign idle_o = state_q == 3'd4;
endmodule
