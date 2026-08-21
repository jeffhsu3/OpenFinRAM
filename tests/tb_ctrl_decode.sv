// Exhaustive decode-correctness check for ctrl_decode (single-port).
// An independent oracle over every legal address checks reads and writes:
//   row = A[ROW_BITS-1:0], col-sel = next COL_BITS, top/bottom = next bit,
//   bank = the remaining upper bits. Exactly one wordline and one Y-select must
//   be active in the selected bank/half; every inactive output must remain at
//   its safe default. Chip deselect and the enabled no-op state are also checked.
`timescale 1ns/1ps
module tb_ctrl_decode #(
    parameter int NUM_WL     = 8,
    parameter int NUM_BANK   = 1,
    parameter int COLUMN_MUX = 4
);
    localparam int ROW_BITS   = $clog2(NUM_WL);
    localparam int COL_BITS   = (COLUMN_MUX > 1) ? $clog2(COLUMN_MUX) : 1;
    localparam int BANK_BITS  = (NUM_BANK > 1) ? $clog2(NUM_BANK) : 0;
    // For supported geometries, this matches get_addr_width:
    // clog2(NUM_WL)+clog2(NUM_BANK)+1(top/bot)+clog2(mux).
    localparam int ADDR_WIDTH = ROW_BITS + BANK_BITS + 1 + COL_BITS;
    localparam longint unsigned NUM_ADDR = 64'd1 << ADDR_WIDTH;

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

    function automatic bit is_power_of_two(input int value);
        return value > 0 && (value & (value - 1)) == 0;
    endfunction

    task automatic check_addr(
        input logic [ADDR_WIDTH-1:0] a,
        input logic                  write_access
    );
        int unsigned row, col, bank;
        logic topbot;
        string operation;
        // Flat oracle vectors avoid Icarus's restriction on variable indexing
        // through the first dimension of a multidimensional packed array.
        logic [NUM_BANK*NUM_WL-1:0] expected_wlt, expected_wlb;
        logic [NUM_BANK*COLUMN_MUX-1:0] expected_yselt, expected_yseltn;
        logic [NUM_BANK*COLUMN_MUX-1:0] expected_yselb, expected_yselbn;

        row    = a & (NUM_WL - 1);
        col    = (a >> ROW_BITS) & (COLUMN_MUX - 1);
        topbot = a[ROW_BITS+COL_BITS];
        bank   = a >> (ROW_BITS + COL_BITS + 1);
        operation = write_access ? "write" : "read";

        expected_wlt    = '0;
        expected_wlb    = '0;
        expected_yselt  = '0;
        expected_yseltn = '1;
        expected_yselb  = '0;
        expected_yselbn = '1;

        if (topbot) begin
            expected_wlt[bank*NUM_WL + row] = 1'b1;
            expected_yselt[bank*COLUMN_MUX + col] = 1'b1;
            expected_yseltn[bank*COLUMN_MUX + col] = 1'b0;
        end else begin
            expected_wlb[bank*NUM_WL + row] = 1'b1;
            expected_yselb[bank*COLUMN_MUX + col] = 1'b1;
            expected_yselbn[bank*COLUMN_MUX + col] = 1'b0;
        end

        ce_n = 0;
        we_n = write_access ? 1'b0 : 1'b1;
        oe_n = write_access ? 1'b1 : 1'b0;
        A = a;
        @(posedge clk); #2;                        // A_q latched, phases (=clk) high

        // Case inequality makes any X/Z in a decoder output a hard failure.
        if (wlt !== expected_wlt || wlb !== expected_wlb) begin
            errors++;
            $display("FAIL %s a=%0d: wlt=%b expected=%b, wlb=%b expected=%b",
                     operation, a, wlt, expected_wlt, wlb, expected_wlb);
        end

        if (yselt !== expected_yselt || yseltn !== expected_yseltn ||
            yselb !== expected_yselb || yselbn !== expected_yselbn) begin
            errors++;
            $display("FAIL %s a=%0d: Y-select mismatch", operation, a);
            $display("  top: ys=%b expected=%b, ysn=%b expected=%b",
                     yselt, expected_yselt, yseltn, expected_yseltn);
            $display("  bot : ys=%b expected=%b, ysn=%b expected=%b",
                     yselb, expected_yselb, yselbn, expected_yselbn);
        end
    endtask

    task automatic check_inactive(
        input logic ce_value,
        input logic we_value,
        input logic oe_value
    );
        ce_n = ce_value;
        we_n = we_value;
        oe_n = oe_value;
        @(posedge clk); #2;

        if (wlt !== '0 || wlb !== '0) begin
            errors++;
            $display("FAIL inactive ce_n=%b we_n=%b oe_n=%b: wlt=%b wlb=%b",
                     ce_value, we_value, oe_value, wlt, wlb);
        end
        if (yselt !== '0 || yseltn !== '1 || yselb !== '0 || yselbn !== '1) begin
            errors++;
            $display("FAIL inactive ce_n=%b we_n=%b oe_n=%b: invalid Y-select defaults",
                     ce_value, we_value, oe_value);
        end
    endtask

    initial begin
        if (!is_power_of_two(NUM_WL) || NUM_WL < 2)
            $fatal(1, "NUM_WL must be a power of two >= 2 (got %0d)", NUM_WL);
        if (!is_power_of_two(NUM_BANK))
            $fatal(1, "NUM_BANK must be a power of two >= 1 (got %0d)", NUM_BANK);
        if (!is_power_of_two(COLUMN_MUX) || COLUMN_MUX < 2)
            $fatal(1, "COLUMN_MUX must be a power of two >= 2 (got %0d)", COLUMN_MUX);
        if (ADDR_WIDTH >= 63)
            $fatal(1, "ADDR_WIDTH=%0d is too large for exhaustive simulation", ADDR_WIDTH);

        ce_n = 1; repeat (3) @(posedge clk);       // warm up (flush X)
        for (longint unsigned a = 0; a < NUM_ADDR; a++) begin
            check_addr(a[ADDR_WIDTH-1:0], 1'b0);
            check_addr(a[ADDR_WIDTH-1:0], 1'b1);
        end

        check_inactive(1'b1, 1'b1, 1'b0);          // deselected read
        check_inactive(1'b1, 1'b0, 1'b1);          // deselected write
        check_inactive(1'b0, 1'b1, 1'b1);          // enabled, but no read or write

        if (errors == 0)
            $display("PASS: read/write decode correct over all %0d addresses (NUM_WL=%0d, BANKS=%0d, MUX=%0d)",
                     NUM_ADDR, NUM_WL, NUM_BANK, COLUMN_MUX);
        else
            $fatal(1, "FAILED: %0d error(s)", errors);
        $finish;
    end
endmodule
