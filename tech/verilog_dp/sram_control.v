module ctrl_decode #(
    parameter ADDR_WIDTH = 5,
    parameter NUM_WL     = 2,
    parameter NUM_BANK   = 1,
    parameter COLUMN_MUX = 4,
    parameter WL_BUF     = 5,
    parameter SAE_BUF    = 15
)(
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  ce_n_A,
    input  logic                  ce_n_B,
    input  logic                  we_n_A,
    input  logic                  we_n_B,
    input  logic                  oe_n_A,
    input  logic                  oe_n_B,
    input  logic [ADDR_WIDTH-1:0] A_A,
    input  logic [ADDR_WIDTH-1:0] A_B,

    output logic [NUM_BANK-1:0][NUM_WL-1:0]     wlt_A,
    output logic [NUM_BANK-1:0][NUM_WL-1:0]     wlb_A,
    output logic [NUM_BANK-1:0]                 blprechtn_A,
    output logic [NUM_BANK-1:0]                 blprechbn_A,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselt_A,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yseltn_A,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselb_A,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselbn_A,
    output logic [NUM_BANK-1:0]                 wrena_A,
    output logic [NUM_BANK-1:0]                 wrenan_A,
    output logic [NUM_BANK-1:0]                 oeb_out_A,
    output logic [NUM_BANK-1:0]                 oe_out_A,
    output logic [NUM_BANK-1:0]                 sae_A,

    output logic [NUM_BANK-1:0][NUM_WL-1:0]     wlt_B,
    output logic [NUM_BANK-1:0][NUM_WL-1:0]     wlb_B,
    output logic [NUM_BANK-1:0]                 blprechtn_B,
    output logic [NUM_BANK-1:0]                 blprechbn_B,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselt_B,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yseltn_B,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselb_B,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselbn_B,
    output logic [NUM_BANK-1:0]                 wrena_B,
    output logic [NUM_BANK-1:0]                 wrenan_B,
    output logic [NUM_BANK-1:0]                 oeb_out_B,
    output logic [NUM_BANK-1:0]                 oe_out_B,
    output logic [NUM_BANK-1:0]                 sae_B
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
    localparam Y_BITS        = (COLUMN_MUX > 1) ? clog2(COLUMN_MUX) : 1;
    localparam SLICE_BITS    = (NUM_BANK > 1) ? clog2(NUM_BANK) : 1;
    localparam BANK_BIT_IDX  = ROW_BITS + Y_BITS;
    localparam SLICE_BIT_IDX = BANK_BIT_IDX + 1;
    localparam ADDR_USED_BITS = SLICE_BIT_IDX + SLICE_BITS;

    localparam [COLUMN_MUX-1:0] ONEHOT_BASE = {{(COLUMN_MUX - 1){1'b0}}, 1'b1};

    wire [ROW_BITS-1:0]   row_sel_d_A = A_A[ROW_BITS-1:0];
    wire [Y_BITS-1:0]     col_sel_d_A = A_A[BANK_BIT_IDX-1:ROW_BITS];
    wire                  bank_sel_d_A = A_A[BANK_BIT_IDX];
    wire [SLICE_BITS-1:0] slice_sel_d_A = (NUM_BANK > 1) ?
                                          A_A[SLICE_BIT_IDX + SLICE_BITS - 1:SLICE_BIT_IDX] :
                                          '0;

    wire [ROW_BITS-1:0]   row_sel_d_B = A_B[ROW_BITS-1:0];
    wire [Y_BITS-1:0]     col_sel_d_B = A_B[BANK_BIT_IDX-1:ROW_BITS];
    wire                  bank_sel_d_B = A_B[BANK_BIT_IDX];
    wire [SLICE_BITS-1:0] slice_sel_d_B = (NUM_BANK > 1) ?
                                          A_B[SLICE_BIT_IDX + SLICE_BITS - 1:SLICE_BIT_IDX] :
                                          '0;

    logic [ROW_BITS-1:0]   row_sel_r_A;
    logic [Y_BITS-1:0]     col_sel_r_A;
    logic                  bank_sel_r_A;
    logic [SLICE_BITS-1:0] slice_sel_r_A;

    logic [ROW_BITS-1:0]   row_sel_r_B;
    logic [Y_BITS-1:0]     col_sel_r_B;
    logic                  bank_sel_r_B;
    logic [SLICE_BITS-1:0] slice_sel_r_B;

    typedef enum logic [1:0] {
        IDLE  = 2'b00,
        READ  = 2'b01,
        WRITE = 2'b10
    } state_t;

    state_t state_A;
    state_t next_state_A;

    state_t state_B;
    state_t next_state_B;

    wire read_req_A  = (state_A == READ)  && !ce_n_A;
    wire write_req_A = (state_A == WRITE) && !ce_n_A;
    wire read_req_B  = (state_B == READ)  && !ce_n_B;
    wire write_req_B = (state_B == WRITE) && !ce_n_B;

    // Precharge is released first, then WL is asserted after a short delay
    // to avoid VDD->BL->cell->VSS crowbar current.
    wire prech_off_A = (read_req_A || write_req_A) && clk;
    wire prech_off_B = (read_req_B || write_req_B) && clk;

    wire wl_any_fire_A;
    wire wl_any_fire_B;

    delay_cell #(.BUF_COUNT(WL_BUF)) u_delay_wl_A (
        .A(prech_off_A),
        .Y(wl_any_fire_A)
    );

    delay_cell #(.BUF_COUNT(WL_BUF)) u_delay_wl_B (
        .A(prech_off_B),
        .Y(wl_any_fire_B)
    );

    wire wl_read_fire_A  = wl_any_fire_A && read_req_A;

    wire wl_read_fire_B  = wl_any_fire_B && read_req_B;

    // Predecode once per port and share across all bank/half enables.
    wire [NUM_WL-1:0] row_decode_A, row_decode_B;
    sram_row_decode #(.NUM_WL(NUM_WL)) u_row_decode_A
        (.A(row_sel_r_A), .SEL(row_decode_A));
    sram_row_decode #(.NUM_WL(NUM_WL)) u_row_decode_B
        (.A(row_sel_r_B), .SEL(row_decode_B));
    for (genvar bank = 0; bank < NUM_BANK; bank = bank + 1) begin : g_wordlines
        wire enable_A = rst_n && (read_req_A || write_req_A) && wl_any_fire_A
                        && (slice_sel_r_A == bank);
        wire enable_B = rst_n && (read_req_B || write_req_B) && wl_any_fire_B
                        && (slice_sel_r_B == bank);
        sram_wordline_driver_array #(.NUM_WL(NUM_WL)) u_top_A
            (.SEL(row_decode_A), .EN(enable_A && bank_sel_r_A), .WL(wlt_A[bank]));
        sram_wordline_driver_array #(.NUM_WL(NUM_WL)) u_bottom_A
            (.SEL(row_decode_A), .EN(enable_A && !bank_sel_r_A), .WL(wlb_A[bank]));
        sram_wordline_driver_array #(.NUM_WL(NUM_WL)) u_top_B
            (.SEL(row_decode_B), .EN(enable_B && bank_sel_r_B), .WL(wlt_B[bank]));
        sram_wordline_driver_array #(.NUM_WL(NUM_WL)) u_bottom_B
            (.SEL(row_decode_B), .EN(enable_B && !bank_sel_r_B), .WL(wlb_B[bank]));
    end

    // SAE is asserted after wl_read_fire with an additional SAE_BUF delay
    wire sae_raw_A;
    wire sae_raw_B;

    delay_cell #(.BUF_COUNT(SAE_BUF)) u_delay_sae_A (
        .A(wl_read_fire_A),
        .Y(sae_raw_A)
    );

    delay_cell #(.BUF_COUNT(SAE_BUF)) u_delay_sae_B (
        .A(wl_read_fire_B),
        .Y(sae_raw_B)
    );

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state_A      <= IDLE;
            row_sel_r_A  <= '0;
            col_sel_r_A  <= '0;
            bank_sel_r_A <= 1'b0;
            slice_sel_r_A <= '0;
        end else begin
            state_A <= next_state_A;
            if (!ce_n_A) begin
                row_sel_r_A   <= row_sel_d_A;
                col_sel_r_A   <= col_sel_d_A;
                bank_sel_r_A  <= bank_sel_d_A;
                slice_sel_r_A <= slice_sel_d_A;
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state_B      <= IDLE;
            row_sel_r_B  <= '0;
            col_sel_r_B  <= '0;
            bank_sel_r_B <= 1'b0;
            slice_sel_r_B <= '0;
        end else begin
            state_B <= next_state_B;
            if (!ce_n_B) begin
                row_sel_r_B   <= row_sel_d_B;
                col_sel_r_B   <= col_sel_d_B;
                bank_sel_r_B  <= bank_sel_d_B;
                slice_sel_r_B <= slice_sel_d_B;
            end
        end
    end

    always_comb begin
        next_state_A = IDLE;
        if (!ce_n_A) begin
            if (!we_n_A && oe_n_A)      next_state_A = WRITE;
            else if (we_n_A && !oe_n_A) next_state_A = READ;
            else                        next_state_A = IDLE;
        end
    end

    always_comb begin
        next_state_B = IDLE;
        if (!ce_n_B) begin
            if (!we_n_B && oe_n_B)      next_state_B = WRITE;
            else if (we_n_B && !oe_n_B) next_state_B = READ;
            else                        next_state_B = IDLE;
        end
    end

    always_comb begin
        blprechtn_A = '0;
        blprechbn_A = '0;
        oeb_out_A   = '1;
        sae_A       = '0;

        blprechtn_B = '0;
        blprechbn_B = '0;
        oeb_out_B   = '1;
        sae_B       = '0;

        for (int i = 0; i < NUM_BANK; i = i + 1) begin
            yselt_A[i]  = '0;
            yseltn_A[i] = '1;
            yselb_A[i]  = '0;
            yselbn_A[i] = '1;
            wrena_A[i]  = 1'b0;
            wrenan_A[i] = 1'b1;

            yselt_B[i]  = '0;
            yseltn_B[i] = '1;
            yselb_B[i]  = '0;
            yselbn_B[i] = '1;
            wrena_B[i]  = 1'b0;
            wrenan_B[i] = 1'b1;
        end

        if (read_req_A || write_req_A) begin
            if (bank_sel_r_A) begin
                blprechtn_A[slice_sel_r_A] = prech_off_A;
                blprechbn_A[slice_sel_r_A] = 1'b0;
            end else begin
                blprechtn_A[slice_sel_r_A] = 1'b0;
                blprechbn_A[slice_sel_r_A] = prech_off_A;
            end

            if (bank_sel_r_A) begin
                yselt_A[slice_sel_r_A]  = ONEHOT_BASE << col_sel_r_A;
                yseltn_A[slice_sel_r_A] = ~yselt_A[slice_sel_r_A];
            end else begin
                yselb_A[slice_sel_r_A]  = ONEHOT_BASE << col_sel_r_A;
                yselbn_A[slice_sel_r_A] = ~yselb_A[slice_sel_r_A];
            end

            if (read_req_A) begin
                oeb_out_A[slice_sel_r_A] = oe_n_A;
                sae_A[slice_sel_r_A]     = sae_raw_A;
            end

            if (write_req_A) begin
                wrena_A[slice_sel_r_A]  = clk;
                wrenan_A[slice_sel_r_A] = ~clk;
            end
        end

        if (read_req_B || write_req_B) begin
            if (bank_sel_r_B) begin
                blprechtn_B[slice_sel_r_B] = prech_off_B;
                blprechbn_B[slice_sel_r_B] = 1'b0;
            end else begin
                blprechtn_B[slice_sel_r_B] = 1'b0;
                blprechbn_B[slice_sel_r_B] = prech_off_B;
            end

            if (bank_sel_r_B) begin
                yselt_B[slice_sel_r_B]  = ONEHOT_BASE << col_sel_r_B;
                yseltn_B[slice_sel_r_B] = ~yselt_B[slice_sel_r_B];
            end else begin
                yselb_B[slice_sel_r_B]  = ONEHOT_BASE << col_sel_r_B;
                yselbn_B[slice_sel_r_B] = ~yselb_B[slice_sel_r_B];
            end

            if (read_req_B) begin
                oeb_out_B[slice_sel_r_B] = oe_n_B;
                sae_B[slice_sel_r_B]     = sae_raw_B;
            end

            if (write_req_B) begin
                wrena_B[slice_sel_r_B]  = clk;
                wrenan_B[slice_sel_r_B] = ~clk;
            end
        end

        oe_out_A = ~oeb_out_A;
        oe_out_B = ~oeb_out_B;
    end

endmodule
