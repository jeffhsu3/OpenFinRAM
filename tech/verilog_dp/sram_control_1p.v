// Single-port (1RW) controller for the generated 6T macro (--bitcell 6t).
//
// The two-port controller (sram_control.v) with port B taken out: one port,
// whose IO column is at one end of the bitlines of each column's one array
// of 2*NUM_WL wordlines.  Its ports keep port A's names (ce_n_A, A_A, ...,
// sel_hi_A, ysel_A, ...) so the macro assembler and the deck name the
// single-port macro the way they name the two-port one's port A.
//
// The wordlines are driven at the array by four-wordline slices (WL<i> =
// SEL . B<i>), one strip on each side of this controller.  The controller
// emits the one-hot of the wordline index's two low bits (sel_lo, static)
// and, per bank, the one-hot of its high bits gated by the wordline phase
// (sel_hi): wordline w = {bank_sel, row}, w[1:0] picks within a slice,
// w[WL_BITS-1:2] the slice.
module ctrl_decode #(
    parameter ADDR_WIDTH = 5,
    parameter NUM_WL     = 2,
    parameter NUM_BANK   = 1,
    parameter COLUMN_MUX = 4,
    parameter WL_BUF     = 5,
    parameter SAE_BUF    = 15,
    // The address bits of a group in the slice decode (sel_hi): 2, 3 or 4.
    parameter ROW_PREDECODE = 3,
    // 6T at 16:1: the leaves decode their own select from two one-hot
    // groups, carried on ysel_A[bank][0 +: 4] (the low two column bits) and
    // ysel_A[bank][4 +: COLUMN_MUX/4] (the rest); yseln_A is then unused.
    parameter YSEL_PREDECODE = 0,
    // The select bus a bank takes: one line a column, or the two groups.
    parameter YSEL_W     = (YSEL_PREDECODE != 0) ? 4 + COLUMN_MUX / 4 : COLUMN_MUX,  // not to be overridden
    parameter SLICES     = (2 * NUM_WL) / 4  // four wordlines per driver slice; not to be overridden
)(
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  ce_n_A,
    input  logic                  we_n_A,
    input  logic                  oe_n_A,
    input  logic [ADDR_WIDTH-1:0] A_A,

    output logic [NUM_BANK-1:0][SLICES-1:0]     sel_hi_A,
    output logic [3:0]                          sel_lo_A,
    output logic [NUM_BANK-1:0]                 blprechn_A,
    output logic [NUM_BANK-1:0][YSEL_W-1:0]     ysel_A,
    output logic [NUM_BANK-1:0][YSEL_W-1:0]     yseln_A,
    output logic [NUM_BANK-1:0]                 wrena_A,
    output logic [NUM_BANK-1:0]                 wrenan_A,
    output logic [NUM_BANK-1:0]                 oeb_out_A,
    output logic [NUM_BANK-1:0]                 oe_out_A,
    output logic [NUM_BANK-1:0]                 sae_A
);

    function integer clog2;
        input integer value;
        begin
            value = value - 1;
            for (clog2 = 0; value > 0; clog2 = clog2 + 1)
                value = value >> 1;
        end
    endfunction

    localparam ROW_BITS      = clog2(NUM_WL);
    localparam WL_BITS       = ROW_BITS + 1;       // 2*NUM_WL wordlines: {bank_sel, row}
    localparam Y_BITS        = (COLUMN_MUX > 1) ? clog2(COLUMN_MUX) : 1;
    localparam SLICE_BITS    = (NUM_BANK > 1) ? clog2(NUM_BANK) : 1;
    localparam BANK_BIT_IDX  = ROW_BITS + Y_BITS;
    localparam SLICE_BIT_IDX = BANK_BIT_IDX + 1;

    localparam [COLUMN_MUX-1:0] ONEHOT_BASE = {{(COLUMN_MUX - 1){1'b0}}, 1'b1};

    wire [ROW_BITS-1:0]   row_sel_d   = A_A[ROW_BITS-1:0];
    wire [Y_BITS-1:0]     col_sel_d   = A_A[BANK_BIT_IDX-1:ROW_BITS];
    wire                  bank_sel_d  = A_A[BANK_BIT_IDX];
    wire [SLICE_BITS-1:0] slice_sel_d = (NUM_BANK > 1) ?
                                        A_A[SLICE_BIT_IDX + SLICE_BITS - 1:SLICE_BIT_IDX] :
                                        '0;

    logic [ROW_BITS-1:0]   row_sel_r;
    logic [Y_BITS-1:0]     col_sel_r;
    logic                  bank_sel_r;
    logic [SLICE_BITS-1:0] slice_sel_r;

    typedef enum logic [1:0] {
        IDLE  = 2'b00,
        READ  = 2'b01,
        WRITE = 2'b10
    } state_t;

    state_t state;
    state_t next_state;

    // The command captured at the rising edge runs the whole cycle, whatever
    // the pins do after the edge: a synchronous driver changes ce_n/we_n/oe_n
    // a flop delay after the edge it launches from, long before this cycle's
    // wordline (~120 ps) and sense enable (~210 ps).
    wire read_req  = (state == READ);
    wire write_req = (state == WRITE);

    // Precharge is released first, then WL is asserted after a short delay
    // to avoid VDD->BL->cell->VSS crowbar current.  The access phase is the
    // clock's high phase, gated by the incoming command held in a latch that
    // is transparent while the clock is low.
    //
    // Clock gating here and for the write latch below: the latch is clocked
    // from the clock tree, like the flops, but the gate takes the clock ahead
    // of the registers' latency (the P&R script keeps CTS from balancing
    // these gates to the registers).  So the gate output follows the pin by
    // a gate and the short tree, not by the register tree plus a flop (as
    // (state != IDLE) && clk would), and cannot glitch: it rises before the
    // latch closes, which the command pins' hold to the flops already
    // covers, and falls before the latch reopens.  (An ICG would put the
    // register tree in front of the gate.)
    wire access_next = rst_n && (next_state != IDLE);
    wire access_l;
    wire prech_off;
    (* keep, dont_touch *)
    DLLx1_ASAP7_75t_R u_access_en (.CLK(clk), .D(access_next), .Q(access_l));
    (* keep, dont_touch *)
    AND2x2_ASAP7_75t_R u_access_gate (.A(clk), .B(access_l), .Y(prech_off));

    wire wl_any_fire;
    delay_cell #(.BUF_COUNT(WL_BUF)) u_delay_wl (
        .A(prech_off),
        .Y(wl_any_fire)
    );

    wire wl_read_fire = wl_any_fire && read_req;
    // Write enable follows the delayed clock, as the wordline does (see the
    // two-port controller: gating the raw clock let it pulse on the edge
    // after a write).
    wire wl_write_fire = wl_any_fire && write_req;

    // Predecode: the low two bits of the wordline index pick a wordline
    // within a slice (static, shared by every slice and bank); the high bits
    // pick the slice, gated by the wordline phase and the bank.
    wire [WL_BITS-1:0] wl_index = {bank_sel_r, row_sel_r};
    wire [3:0] lo;
    wire [SLICES-1:0] hi;
    if (2 * NUM_WL < 4 || (2 * NUM_WL) % 4 != 0) begin : g_bad_wordlines
        invalid_sram_wordline_count u_invalid ();   // a strip is whole slices
    end
    sram_row_decode #(.NUM_WL(4), .ADDR_BITS(2)) u_lo
        (.A(wl_index[1:0]), .SEL(lo));
    if (SLICES > 1) begin : g_hi
        sram_row_decode #(.NUM_WL(SLICES), .ADDR_BITS(WL_BITS - 2),
                          .PREDECODE_BITS(ROW_PREDECODE)) u_hi
            (.A(wl_index[WL_BITS-1:2]), .SEL(hi));
    end else begin : g_one_slice
        assign hi = 1'b1;
    end
    assign sel_lo_A = lo;
    for (genvar bank = 0; bank < NUM_BANK; bank = bank + 1) begin : g_wordlines
        wire enable = rst_n && (read_req || write_req) && wl_any_fire
                      && (slice_sel_r == bank);
        assign sel_hi_A[bank] = hi & {SLICES{enable}};
    end

    // SAE is asserted after wl_read_fire with an additional SAE_BUF delay.
    wire sae_raw;
    delay_cell #(.BUF_COUNT(SAE_BUF)) u_delay_sae (
        .A(wl_read_fire),
        .Y(sae_raw)
    );

    // Write data.  The IO block's write driver latch follows D while wrenan
    // is high, so D is taken when wrenan falls: D's hold is clk -> wrenan
    // falling at the farthest column.  Per bank, the incoming write command,
    // latched as above, closes it one gate after the clock pin rises
    // (wrenan = !(clk & write_next | wl_write_fire)); it stays closed until
    // the wordline is off (wl_write_fire falls a WL_BUF delay after the
    // falling edge) and reopens for the next cycle's D.
    for (genvar bank = 0; bank < NUM_BANK; bank = bank + 1) begin : g_write_latch
        wire wr_next = rst_n && !ce_n_A && !we_n_A && oe_n_A && (slice_sel_d == bank);
        wire wr_next_l;
        wire wr_fire = wl_write_fire && (slice_sel_r == bank);
        (* keep, dont_touch *)
        DLLx1_ASAP7_75t_R u_wr_en (.CLK(clk), .D(wr_next), .Q(wr_next_l));
        (* keep, dont_touch *)
        AOI21x1_ASAP7_75t_R u_wr_close (
            .A1(clk), .A2(wr_next_l), .B(wr_fire), .Y(wrenan_A[bank])
        );
    end

    // Output enable.  Q is the selected bank's tri-state output latch; the
    // banks share Q.  Enable the bank read last, from a flop, from the read's
    // capture edge until the next read's, so Q holds the last word read (and
    // is driven) through writes and idle cycles, and a synchronous reader
    // sampling at the next edge sees a driven Q.  Bank 0 after reset.
    logic [NUM_BANK-1:0] oe_r;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state       <= IDLE;
            row_sel_r   <= '0;
            col_sel_r   <= '0;
            bank_sel_r  <= 1'b0;
            slice_sel_r <= '0;
            oe_r        <= NUM_BANK'(1);
        end else begin
            state <= next_state;
            if (next_state == READ)
                oe_r <= NUM_BANK'(1) << slice_sel_d;
            if (!ce_n_A) begin
                row_sel_r   <= row_sel_d;
                col_sel_r   <= col_sel_d;
                bank_sel_r  <= bank_sel_d;
                slice_sel_r <= slice_sel_d;
            end
        end
    end

    always_comb begin
        next_state = IDLE;
        if (!ce_n_A) begin
            if (!we_n_A && oe_n_A)      next_state = WRITE;
            else if (we_n_A && !oe_n_A) next_state = READ;
            else                        next_state = IDLE;
        end
    end

    always_comb begin
        blprechn_A = '0;
        sae_A      = '0;
        for (int i = 0; i < NUM_BANK; i = i + 1) begin
            ysel_A[i]   = '0;
            yseln_A[i]  = '1;
            wrena_A[i]  = 1'b0;
        end

        // prech_off is high only in an access cycle, so it needs no gating
        // by the (later) state flop.
        blprechn_A[slice_sel_r] = prech_off;
        if (read_req || write_req) begin
            if (YSEL_PREDECODE != 0) begin
                ysel_A[slice_sel_r][col_sel_r[1:0]]     = 1'b1;
                ysel_A[slice_sel_r][4 + (col_sel_r >> 2)] = 1'b1;
            end else begin
                ysel_A[slice_sel_r] = ONEHOT_BASE << col_sel_r;
            end
            // (Unused with the groups, but each its own driven net: tied
            // constants would share one tie cell's output across pins.)
            yseln_A[slice_sel_r] = ~ysel_A[slice_sel_r];

            if (read_req)
                sae_A[slice_sel_r] = sae_raw;

            if (write_req)
                wrena_A[slice_sel_r] = wl_write_fire;
        end
    end

    assign oe_out_A  = oe_r;
    assign oeb_out_A = ~oe_r;

endmodule
