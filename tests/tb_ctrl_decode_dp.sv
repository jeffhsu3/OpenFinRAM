// Exhaustive functional check for the true-dual-port ctrl_decode.  Each port
// is independently exercised for reads and writes over the complete address
// space, followed by a simultaneous write to different addresses.
`timescale 1ns/1ps
module tb_ctrl_decode_dp #(
    parameter int NUM_WL     = 8,
    parameter int NUM_BANK   = 1,
    parameter int COLUMN_MUX = 4
);
    localparam int ROW_BITS   = $clog2(NUM_WL);
    localparam int COL_BITS   = (COLUMN_MUX > 1) ? $clog2(COLUMN_MUX) : 1;
    localparam int BANK_BITS  = (NUM_BANK > 1) ? $clog2(NUM_BANK) : 0;
    localparam int ADDR_WIDTH = ROW_BITS + BANK_BITS + 1 + COL_BITS;
    localparam longint unsigned NUM_ADDR = 64'd1 << ADDR_WIDTH;

    logic clk = 0, rst_n = 0;
    logic ce_n_A = 1, we_n_A = 1, oe_n_A = 1;
    logic ce_n_B = 1, we_n_B = 1, oe_n_B = 1;
    logic [ADDR_WIDTH-1:0] A_A = 0, A_B = 0;

    logic [NUM_BANK-1:0][NUM_WL-1:0] wlt_A, wlb_A, wlt_B, wlb_B;
    logic [NUM_BANK-1:0][COLUMN_MUX-1:0]
        yselt_A, yseltn_A, yselb_A, yselbn_A,
        yselt_B, yseltn_B, yselb_B, yselbn_B;
    logic [NUM_BANK-1:0]
        blprechtn_A, blprechbn_A, wrena_A, wrenan_A,
        oeb_out_A, oe_out_A, sae_A,
        blprechtn_B, blprechbn_B, wrena_B, wrenan_B,
        oeb_out_B, oe_out_B, sae_B;

    ctrl_decode #(
        .ADDR_WIDTH(ADDR_WIDTH), .NUM_WL(NUM_WL), .NUM_BANK(NUM_BANK),
        .COLUMN_MUX(COLUMN_MUX), .WL_BUF(2), .SAE_BUF(2)
    ) dut (
        .clk(clk), .rst_n(rst_n),
        .ce_n_A(ce_n_A), .ce_n_B(ce_n_B),
        .we_n_A(we_n_A), .we_n_B(we_n_B),
        .oe_n_A(oe_n_A), .oe_n_B(oe_n_B), .A_A(A_A), .A_B(A_B),
        .wlt_A(wlt_A), .wlb_A(wlb_A),
        .blprechtn_A(blprechtn_A), .blprechbn_A(blprechbn_A),
        .yselt_A(yselt_A), .yseltn_A(yseltn_A),
        .yselb_A(yselb_A), .yselbn_A(yselbn_A),
        .wrena_A(wrena_A), .wrenan_A(wrenan_A),
        .oeb_out_A(oeb_out_A), .oe_out_A(oe_out_A), .sae_A(sae_A),
        .wlt_B(wlt_B), .wlb_B(wlb_B),
        .blprechtn_B(blprechtn_B), .blprechbn_B(blprechbn_B),
        .yselt_B(yselt_B), .yseltn_B(yseltn_B),
        .yselb_B(yselb_B), .yselbn_B(yselbn_B),
        .wrena_B(wrena_B), .wrenan_B(wrenan_B),
        .oeb_out_B(oeb_out_B), .oe_out_B(oe_out_B), .sae_B(sae_B)
    );

    always #5 clk = ~clk;
    int errors = 0;

    function automatic bit is_power_of_two(input int value);
        return value > 0 && (value & (value - 1)) == 0;
    endfunction

    task automatic check_selected_port(
        input bit port_b,
        input logic [ADDR_WIDTH-1:0] address,
        input bit write_access
    );
        int unsigned row, col, bank;
        logic topbot;
        logic [NUM_BANK*NUM_WL-1:0] expected_wlt, expected_wlb;
        logic [NUM_BANK*COLUMN_MUX-1:0]
            expected_yselt, expected_yseltn, expected_yselb, expected_yselbn;
        logic [NUM_BANK-1:0] expected_precht, expected_prechb;
        logic [NUM_BANK-1:0] expected_wrena, expected_wrenan;
        logic [NUM_BANK-1:0] expected_oeb, expected_oe, expected_sae;

        row    = address & (NUM_WL - 1);
        col    = (address >> ROW_BITS) & (COLUMN_MUX - 1);
        topbot = address[ROW_BITS+COL_BITS];
        bank   = address >> (ROW_BITS + COL_BITS + 1);

        expected_wlt = '0; expected_wlb = '0;
        expected_yselt = '0; expected_yseltn = '1;
        expected_yselb = '0; expected_yselbn = '1;
        expected_precht = '0; expected_prechb = '0;
        expected_wrena = '0; expected_wrenan = '1;
        expected_oeb = '1; expected_oe = '0; expected_sae = '0;
        if (topbot) begin
            expected_wlt[bank*NUM_WL + row] = 1'b1;
            expected_yselt[bank*COLUMN_MUX + col] = 1'b1;
            expected_yseltn[bank*COLUMN_MUX + col] = 1'b0;
            expected_precht[bank] = 1'b1;
        end else begin
            expected_wlb[bank*NUM_WL + row] = 1'b1;
            expected_yselb[bank*COLUMN_MUX + col] = 1'b1;
            expected_yselbn[bank*COLUMN_MUX + col] = 1'b0;
            expected_prechb[bank] = 1'b1;
        end
        if (write_access) begin
            expected_wrena[bank] = 1'b1;
            expected_wrenan[bank] = 1'b0;
        end else begin
            expected_oeb[bank] = 1'b0;
            expected_oe[bank] = 1'b1;
            expected_sae[bank] = 1'b1;
        end

        ce_n_A = 1; we_n_A = 1; oe_n_A = 1;
        ce_n_B = 1; we_n_B = 1; oe_n_B = 1;
        if (port_b) begin
            ce_n_B = 0; we_n_B = !write_access; oe_n_B = write_access;
            A_B = address;
        end else begin
            ce_n_A = 0; we_n_A = !write_access; oe_n_A = write_access;
            A_A = address;
        end
        @(posedge clk); #2;

        if (!port_b) begin
            if (wlt_A !== expected_wlt || wlb_A !== expected_wlb ||
                yselt_A !== expected_yselt || yseltn_A !== expected_yseltn ||
                yselb_A !== expected_yselb || yselbn_A !== expected_yselbn ||
                blprechtn_A !== expected_precht || blprechbn_A !== expected_prechb ||
                wrena_A !== expected_wrena || wrenan_A !== expected_wrenan ||
                oeb_out_A !== expected_oeb || oe_out_A !== expected_oe ||
                sae_A !== expected_sae) begin
                errors++;
                $display("FAIL A %s address=%0d", write_access ? "write" : "read", address);
            end
            if (wlt_B !== '0 || wlb_B !== '0 || wrena_B !== '0 ||
                wrenan_B !== '1 || sae_B !== '0) begin
                errors++;
                $display("FAIL A access activated idle port B at address=%0d", address);
            end
        end else begin
            if (wlt_B !== expected_wlt || wlb_B !== expected_wlb ||
                yselt_B !== expected_yselt || yseltn_B !== expected_yseltn ||
                yselb_B !== expected_yselb || yselbn_B !== expected_yselbn ||
                blprechtn_B !== expected_precht || blprechbn_B !== expected_prechb ||
                wrena_B !== expected_wrena || wrenan_B !== expected_wrenan ||
                oeb_out_B !== expected_oeb || oe_out_B !== expected_oe ||
                sae_B !== expected_sae) begin
                errors++;
                $display("FAIL B %s address=%0d", write_access ? "write" : "read", address);
            end
            if (wlt_A !== '0 || wlb_A !== '0 || wrena_A !== '0 ||
                wrenan_A !== '1 || sae_A !== '0) begin
                errors++;
                $display("FAIL B access activated idle port A at address=%0d", address);
            end
        end
    endtask

    initial begin
        if (!is_power_of_two(NUM_WL) || NUM_WL < 2 ||
            !is_power_of_two(NUM_BANK) ||
            !is_power_of_two(COLUMN_MUX) || COLUMN_MUX < 2)
            $fatal(1, "unsupported test geometry");

        repeat (2) @(posedge clk);
        rst_n = 1;
        for (longint unsigned address = 0; address < NUM_ADDR; address++) begin
            check_selected_port(0, address[ADDR_WIDTH-1:0], 0);
            check_selected_port(0, address[ADDR_WIDTH-1:0], 1);
            check_selected_port(1, address[ADDR_WIDTH-1:0], 0);
            check_selected_port(1, address[ADDR_WIDTH-1:0], 1);
        end

        // Both write-state machines and write-enable buses must be live in the
        // same cycle; different addresses avoid an intentional cell collision.
        ce_n_A = 0; we_n_A = 0; oe_n_A = 1; A_A = '0;
        ce_n_B = 0; we_n_B = 0; oe_n_B = 1; A_B = NUM_ADDR - 1;
        @(posedge clk); #2;
        if ($countones(wrena_A) != 1 || $countones(wrena_B) != 1 ||
            $countones(wlt_A) + $countones(wlb_A) != 1 ||
            $countones(wlt_B) + $countones(wlb_B) != 1) begin
            errors++;
            $display("FAIL simultaneous A/B write activation");
        end

        if (errors == 0)
            $display("PASS: 2RW decode correct over all %0d addresses (NUM_WL=%0d, BANKS=%0d, MUX=%0d)",
                     NUM_ADDR, NUM_WL, NUM_BANK, COLUMN_MUX);
        else
            $fatal(1, "FAILED: %0d error(s)", errors);
        $finish;
    end
endmodule
