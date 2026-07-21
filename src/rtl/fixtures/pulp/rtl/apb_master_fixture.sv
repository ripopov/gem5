// Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause

`include "apb/typedef.svh"

module apb_master_fixture (
  input  logic        clk_i,
  input  logic        rst_ni,
  output logic [31:0] paddr_o,
  output logic        psel_o,
  output logic        penable_o,
  output logic        pwrite_o,
  output logic [31:0] pwdata_o,
  output logic [3:0]  pstrb_o,
  output logic [2:0]  pprot_o,
  input  logic        pready_i,
  input  logic [31:0] prdata_i,
  input  logic        pslverr_i,
  output logic        idle_o
);
  typedef logic [31:0] addr_t;
  typedef logic [31:0] data_t;
  typedef logic [3:0]  strb_t;
  `APB_TYPEDEF_ALL(fixture_apb, addr_t, data_t, strb_t)

  fixture_apb_req_t source_req;
  fixture_apb_req_t [0:0] master_req;
  fixture_apb_resp_t source_resp;
  fixture_apb_resp_t [0:0] master_resp;
  logic [2:0] state_q;

  apb_demux #(
    .NoMstPorts (1),
    .req_t       (fixture_apb_req_t),
    .resp_t      (fixture_apb_resp_t)
  ) i_pulp_apb_demux (
    .slv_req_i  (source_req),
    .slv_resp_o (source_resp),
    .mst_req_o  (master_req),
    .mst_resp_i (master_resp),
    .select_i   (1'b0)
  );

  always_comb begin
    source_req = '0;
    source_req.paddr = 32'h0000_0010;
    source_req.pprot = 3'b000;
    source_req.pstrb = 4'b0101;
    unique case (state_q)
      3'd0: begin
        source_req.psel = 1'b1;
        source_req.pwrite = 1'b1;
        source_req.pwdata = 32'h4433_2211;
      end
      3'd1: begin
        source_req.psel = 1'b1;
        source_req.penable = 1'b1;
        source_req.pwrite = 1'b1;
        source_req.pwdata = 32'h4433_2211;
      end
      3'd2: source_req.psel = 1'b1;
      3'd3: begin
        source_req.psel = 1'b1;
        source_req.penable = 1'b1;
      end
      default: source_req = '0;
    endcase
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q <= 3'd0;
    end else begin
      unique case (state_q)
        3'd0: state_q <= 3'd1;
        3'd1: if (source_resp.pready) state_q <= 3'd2;
        3'd2: state_q <= 3'd3;
        3'd3: if (source_resp.pready) state_q <= 3'd4;
        default: state_q <= state_q;
      endcase
    end
  end

  assign master_resp[0] = '{
    pready: pready_i,
    prdata: prdata_i,
    pslverr: pslverr_i
  };
  assign paddr_o = master_req[0].paddr;
  assign psel_o = master_req[0].psel;
  assign penable_o = master_req[0].penable;
  assign pwrite_o = master_req[0].pwrite;
  assign pwdata_o = master_req[0].pwdata;
  assign pstrb_o = master_req[0].pstrb;
  assign pprot_o = master_req[0].pprot;
  assign idle_o = state_q == 3'd4;

  logic unused;
  assign unused = ^{source_resp.prdata, source_resp.pslverr};
endmodule
