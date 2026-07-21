// Copyright (c) 2026 The gem5 Authors. SPDX-License-Identifier: BSD-3-Clause

`include "apb/typedef.svh"

module apb_slave_fixture (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic [31:0] paddr_i,
  input  logic        psel_i,
  input  logic        penable_i,
  input  logic        pwrite_i,
  input  logic [31:0] pwdata_i,
  input  logic [3:0]  pstrb_i,
  input  logic [2:0]  pprot_i,
  output logic        pready_o,
  output logic [31:0] prdata_o,
  output logic        pslverr_o,
  output logic        idle_o
);
  typedef logic [31:0] addr_t;
  typedef logic [31:0] data_t;
  typedef logic [3:0]  strb_t;
  `APB_TYPEDEF_ALL(fixture_apb, addr_t, data_t, strb_t)

  fixture_apb_req_t slave_req;
  fixture_apb_req_t [0:0] target_req;
  fixture_apb_resp_t slave_resp;
  fixture_apb_resp_t [0:0] target_resp;
  logic [31:0] register_q;

  apb_demux #(
    .NoMstPorts (1),
    .req_t       (fixture_apb_req_t),
    .resp_t      (fixture_apb_resp_t)
  ) i_pulp_apb_demux (
    .slv_req_i  (slave_req),
    .slv_resp_o (slave_resp),
    .mst_req_o  (target_req),
    .mst_resp_i (target_resp),
    .select_i   (1'b0)
  );

  assign slave_req = '{
    paddr: paddr_i,
    pprot: pprot_i,
    psel: psel_i,
    penable: penable_i,
    pwrite: pwrite_i,
    pwdata: pwdata_i,
    pstrb: pstrb_i
  };
  assign target_resp[0] = '{
    pready: target_req[0].psel & target_req[0].penable,
    prdata: register_q,
    pslverr: target_req[0].paddr != 32'h0000_0010
  };

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      register_q <= 32'h0;
    end else if (target_req[0].psel && target_req[0].penable &&
                 target_req[0].pwrite) begin
      for (int unsigned lane = 0; lane < 4; lane++) begin
        if (target_req[0].pstrb[lane])
          register_q[lane*8 +: 8] <= target_req[0].pwdata[lane*8 +: 8];
      end
    end
  end

  assign pready_o = slave_resp.pready;
  assign prdata_o = slave_resp.prdata;
  assign pslverr_o = slave_resp.pslverr;
  assign idle_o = ~psel_i;
endmodule
