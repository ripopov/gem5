// Copyright (c) 2026 The gem5 Project
// SPDX-License-Identifier: BSD-3-Clause

// Convert the OpenC910 legacy AXI barrier transactions into local completion
// responses. The standard AXI4 interface downstream has no AxBAR signals, so
// a barrier is completed only after every previously forwarded transaction
// has received its final response. New transactions remain blocked until the
// synthetic barrier response is accepted by the core.
module c910_axi_barrier_filter #(
  parameter int unsigned IdWidth = 32'd0,
  parameter logic [IdWidth-1:0] BarrierId = IdWidth'(5'd31),
  parameter type axi_req_t = logic,
  parameter type axi_rsp_t = logic
) (
  input  logic     clk_i,
  input  logic     rst_ni,
  input  axi_req_t slv_req_i,
  output axi_rsp_t slv_rsp_o,
  output axi_req_t mst_req_o,
  input  axi_rsp_t mst_rsp_i
);

  typedef enum logic [1:0] {
    Idle,
    BarrierWriteData,
    BarrierWriteResponse,
    BarrierReadResponse
  } state_t;

  state_t state_q;
  logic [IdWidth-1:0] barrier_id_q;
  logic [8:0] reads_outstanding_q;
  logic [8:0] writes_outstanding_q;
  logic [8:0] write_data_pending_q;

  logic barrier_aw;
  logic barrier_ar;
  logic drained;
  logic normal_ar_fire;
  logic normal_aw_fire;
  logic normal_w_last_fire;
  logic normal_r_last_fire;
  logic normal_b_fire;
  logic barrier_aw_fire;
  logic barrier_ar_fire;
  logic barrier_w_fire;

  assign barrier_aw = slv_req_i.aw_valid &&
                      (slv_req_i.aw.id == BarrierId);
  assign barrier_ar = slv_req_i.ar_valid &&
                      (slv_req_i.ar.id == BarrierId);
  assign drained = (reads_outstanding_q == 0) &&
                   (writes_outstanding_q == 0) &&
                   (write_data_pending_q == 0);

  always_comb begin
    mst_req_o = slv_req_i;
    slv_rsp_o = mst_rsp_i;

    if (state_q == Idle && (barrier_aw || barrier_ar)) begin
      // Do not allow a request on the other address channel to pass the
      // barrier. Write data belonging to an older accepted AW may still drain.
      mst_req_o.aw_valid = 1'b0;
      mst_req_o.ar_valid = 1'b0;
      slv_rsp_o.aw_ready = 1'b0;
      slv_rsp_o.ar_ready = 1'b0;

      if (write_data_pending_q == 0) begin
        mst_req_o.w_valid = 1'b0;
        slv_rsp_o.w_ready = 1'b0;
      end

      if (barrier_aw) begin
        slv_rsp_o.aw_ready = drained;
        // OpenC910 presents the single barrier W beat with, or after, AW.
        slv_rsp_o.w_ready = drained;
      end else begin
        slv_rsp_o.ar_ready = drained;
      end
    end else if (state_q != Idle) begin
      mst_req_o.aw_valid = 1'b0;
      mst_req_o.w_valid = 1'b0;
      mst_req_o.ar_valid = 1'b0;
      mst_req_o.b_ready = 1'b0;
      mst_req_o.r_ready = 1'b0;
      slv_rsp_o.aw_ready = 1'b0;
      slv_rsp_o.w_ready = 1'b0;
      slv_rsp_o.ar_ready = 1'b0;
      slv_rsp_o.b_valid = 1'b0;
      slv_rsp_o.r_valid = 1'b0;

      case (state_q)
        BarrierWriteData: begin
          slv_rsp_o.w_ready = 1'b1;
        end
        BarrierWriteResponse: begin
          slv_rsp_o.b = '0;
          slv_rsp_o.b.id = barrier_id_q;
          slv_rsp_o.b.resp = axi_pkg::RESP_OKAY;
          slv_rsp_o.b_valid = 1'b1;
        end
        BarrierReadResponse: begin
          slv_rsp_o.r = '0;
          slv_rsp_o.r.id = barrier_id_q;
          slv_rsp_o.r.resp = axi_pkg::RESP_OKAY;
          slv_rsp_o.r.last = 1'b1;
          slv_rsp_o.r_valid = 1'b1;
        end
        default: begin
        end
      endcase
    end
  end

  assign normal_ar_fire = (state_q == Idle) && !barrier_aw && !barrier_ar &&
                          mst_req_o.ar_valid && mst_rsp_i.ar_ready;
  assign normal_aw_fire = (state_q == Idle) && !barrier_aw && !barrier_ar &&
                          mst_req_o.aw_valid && mst_rsp_i.aw_ready;
  assign normal_w_last_fire = mst_req_o.w_valid && mst_rsp_i.w_ready &&
                              mst_req_o.w.last;
  assign normal_r_last_fire = mst_rsp_i.r_valid && mst_req_o.r_ready &&
                              mst_rsp_i.r.last;
  assign normal_b_fire = mst_rsp_i.b_valid && mst_req_o.b_ready;
  assign barrier_aw_fire = (state_q == Idle) && barrier_aw &&
                           slv_rsp_o.aw_ready;
  assign barrier_ar_fire = (state_q == Idle) && !barrier_aw && barrier_ar &&
                           slv_rsp_o.ar_ready;
  assign barrier_w_fire = slv_req_i.w_valid && slv_rsp_o.w_ready;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q <= Idle;
      barrier_id_q <= '0;
      reads_outstanding_q <= '0;
      writes_outstanding_q <= '0;
      write_data_pending_q <= '0;
    end else begin
      case ({normal_ar_fire, normal_r_last_fire})
        2'b10: reads_outstanding_q <= reads_outstanding_q + 1'b1;
        2'b01: reads_outstanding_q <= reads_outstanding_q - 1'b1;
        default: reads_outstanding_q <= reads_outstanding_q;
      endcase
      case ({normal_aw_fire, normal_b_fire})
        2'b10: writes_outstanding_q <= writes_outstanding_q + 1'b1;
        2'b01: writes_outstanding_q <= writes_outstanding_q - 1'b1;
        default: writes_outstanding_q <= writes_outstanding_q;
      endcase
      case ({normal_aw_fire, normal_w_last_fire})
        2'b10: write_data_pending_q <= write_data_pending_q + 1'b1;
        2'b01: write_data_pending_q <= write_data_pending_q - 1'b1;
        default: write_data_pending_q <= write_data_pending_q;
      endcase

      case (state_q)
        Idle: begin
          if (barrier_aw_fire) begin
            barrier_id_q <= slv_req_i.aw.id;
            state_q <= barrier_w_fire ? BarrierWriteResponse :
                                        BarrierWriteData;
          end else if (barrier_ar_fire) begin
            barrier_id_q <= slv_req_i.ar.id;
            state_q <= BarrierReadResponse;
          end
        end
        BarrierWriteData: begin
          if (barrier_w_fire)
            state_q <= BarrierWriteResponse;
        end
        BarrierWriteResponse: begin
          if (slv_rsp_o.b_valid && slv_req_i.b_ready)
            state_q <= Idle;
        end
        BarrierReadResponse: begin
          if (slv_rsp_o.r_valid && slv_req_i.r_ready)
            state_q <= Idle;
        end
        default: state_q <= Idle;
      endcase
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (rst_ni && barrier_aw_fire) begin
      assert (slv_req_i.aw.len == 0)
        else $error("OpenC910 emitted a multi-beat write barrier");
      if (barrier_w_fire)
        assert (slv_req_i.w.last)
          else $error("OpenC910 write barrier beat is not last");
    end
    if (rst_ni && state_q == BarrierWriteData && barrier_w_fire)
      assert (slv_req_i.w.last)
        else $error("OpenC910 write barrier beat is not last");
    if (rst_ni && barrier_ar_fire)
      assert (slv_req_i.ar.len == 0)
        else $error("OpenC910 emitted a multi-beat read barrier");
  end
`endif

endmodule
