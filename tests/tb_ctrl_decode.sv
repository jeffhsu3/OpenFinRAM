// Exhaustive decode-correctness check for ctrl_decode (single-port).
// Independent oracle over ALL addresses asserts the address->wordline mapping:
//   row = A[ROW_BITS-1:0], col-sel = next COL_BITS, top/bottom = next bit;
//   exactly one wlt|wlb bit high on a valid access, at `row`, on the selected
//   half; ysel one-hot at `col` with the complement bus inverted; and no
//   wordline fires when de-selected. This guards every future decoder change.
`timescale 1ns/1ps
module tb_ctrl_decode #(
    parameter int NUM_WL     = 8,
    parameter int NUM_BANK   = 1,
    parameter int COLUMN_MUX = 4
);
    localparam int ROW_BITS   = $clog2(NUM_WL);      // 3
    localparam int COL_BITS   = $clog2(COLUMN_MUX);  // 2
    // matches get_addr_width: clog2(NUM_WL)+clog2(NUM_BANK)+1(top/bot)+clog2(mux)
    localparam int ADDR_WIDTH = ROW_BITS + 0 + 1 + COL_BITS;  // 6

    logic clk = 0, ce_n = 1, we_n = 1, oe_n = 1;
    logic [3:0] sdel = 4'b0;
    logic [ADDR_WIDTH-1:0] A = 0;

    logic [NUM_BANK-1:0][NUM_WL-1:0]     wlt, wlb;
    logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselt, yseltn, yselb, yselbn;
    logic [NUM_BANK-1:0] blprechtn, blprechbn, sae, saprechn, wrena, wrenan, oeb_out, oe_out;

    ctrl_decode #(.ADDR_WIDTH(ADDR_WIDTH), .NUM_WL(NUM_WL),
                  .NUM_BANK(NUM_BANK), .COLUMN_MUX(COLUMN_MUX)) dut (
        .clk(clk), .ce_n(ce_n), .we_n(we_n), .oe_n(oe_n), .sdel(sdel), .A(A),
        .wlt(wlt), .wlb(wlb), .blprechtn(blprechtn), .blprechbn(blprechbn),
        .yselt(yselt), .yseltn(yseltn), .yselb(yselb), .yselbn(yselbn),
        .sae(sae), .saprechn(saprechn), .wrena(wrena), .wrenan(wrenan),
        .oeb_out(oeb_out), .oe_out(oe_out));

    always #5 clk = ~clk;

    int errors = 0;

    function automatic int popcount(input logic [NUM_WL-1:0] v);
        popcount = 0;
        for (int k = 0; k < NUM_WL; k++) popcount += v[k];
    endfunction

    task automatic check_addr(input logic [ADDR_WIDTH-1:0] a);
        logic [ROW_BITS-1:0] row;
        logic [COL_BITS-1:0] col;
        logic topbot;
        int nt, nb;
        logic [COLUMN_MUX-1:0] ys, ysn;
        row    = a[ROW_BITS-1:0];
        col    = a[ROW_BITS+COL_BITS-1 -: COL_BITS];
        topbot = a[ROW_BITS+COL_BITS];            // BANK_ADDR_BIT (NUM_BANK==1)

        ce_n = 0; we_n = 1; oe_n = 0; A = a;       // read access
        @(posedge clk); #2;                        // A_q latched, phases (=clk) high

        nt = popcount(wlt[0]); nb = popcount(wlb[0]);
        if (nt + nb != 1) begin
            errors++; $display("FAIL a=%0d: %0d wlt + %0d wlb active (want exactly 1)", a, nt, nb);
        end else if (topbot) begin
            if (!wlt[0][row] || nb != 0) begin
                errors++; $display("FAIL a=%0d top: wlt=%b (want one-hot @ row %0d), wlb=%b", a, wlt[0], row, wlb[0]);
            end
        end else begin
            if (!wlb[0][row] || nt != 0) begin
                errors++; $display("FAIL a=%0d bot: wlb=%b (want one-hot @ row %0d), wlt=%b", a, wlb[0], row, wlt[0]);
            end
        end

        ys  = topbot ? yselt[0]  : yselb[0];
        ysn = topbot ? yseltn[0] : yselbn[0];
        if (ys !== (COLUMN_MUX'(1) << col)) begin
            errors++; $display("FAIL a=%0d: ysel=%b (want one-hot @ col %0d)", a, ys, col);
        end
        if (ysn !== ~ys) begin
            errors++; $display("FAIL a=%0d: yseln=%b != ~ysel=%b", a, ysn, ~ys);
        end
    endtask

    initial begin
        ce_n = 1; repeat (3) @(posedge clk);       // warm up (flush X)
        for (int a = 0; a < (1 << ADDR_WIDTH); a++)
            check_addr(a[ADDR_WIDTH-1:0]);

        // de-selected: no wordline may fire
        ce_n = 1; @(posedge clk); #2;
        if (popcount(wlt[0]) + popcount(wlb[0]) != 0) begin
            errors++; $display("FAIL idle: a wordline is active with ce_n de-asserted");
        end

        if (errors == 0)
            $display("PASS: decode correct over all %0d addresses (NUM_WL=%0d, MUX=%0d)",
                     (1 << ADDR_WIDTH), NUM_WL, COLUMN_MUX);
        else
            $display("FAILED: %0d error(s)", errors);
        $finish;
    end
endmodule
