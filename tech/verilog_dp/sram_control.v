module ctrl_decode #(
    parameter ADDR_WIDTH = 5,
    parameter DATA_WIDTH = 1,
    parameter NUM_WL     = 2,
    parameter NUM_BANK  = 1,
    parameter COLUMN_MUX = 4
)(
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  ce_n_A,
    input  logic                  ce_n_B,
    input  logic                  we_n_A,
    input  logic                  oe_n_A,
    input  logic                  oe_n_B,
    input  logic [ADDR_WIDTH-1:0] A_A,
    input  logic [ADDR_WIDTH-1:0] A_B,

    // Per-port replica wordline controls for SA timing path.
    // RWLT_A/RWLB_A fire when Port A reads top/bottom bank.
    // RWLT_B/RWLB_B fire when Port B reads top/bottom bank.
    // Kept separate so RBL_A only discharges on Port A reads and RBL_B on Port B reads.
    output logic [NUM_BANK-1:0]   RWLT_A,
    output logic [NUM_BANK-1:0]   RWLB_A,
    output logic [NUM_BANK-1:0]   RWLT_B,
    output logic [NUM_BANK-1:0]   RWLB_B,

    output logic [NUM_BANK-1:0]   blprechn_rbl_A,
    output logic [NUM_BANK-1:0]   blprechn_rbl_B,

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

    output logic [NUM_BANK-1:0][NUM_WL-1:0]     wlt_B,
    output logic [NUM_BANK-1:0][NUM_WL-1:0]     wlb_B,
    output logic [NUM_BANK-1:0]                 blprechtn_B,
    output logic [NUM_BANK-1:0]                 blprechbn_B,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselt_B,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yseltn_B,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselb_B,
    output logic [NUM_BANK-1:0][COLUMN_MUX-1:0] yselbn_B,
    output logic [NUM_BANK-1:0]                 oeb_out_B,
    output logic [NUM_BANK-1:0]                 oe_out_B
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

    // Port B is read-only: single-bit state (0=IDLE, 1=READ)
    logic state_B;
    logic next_state_B;

    wire read_req_A  = (state_A == READ)  && !ce_n_A;
    wire write_req_A = (state_A == WRITE) && !ce_n_A;
    wire read_req_B  = state_B && !ce_n_B;

    // Precharge is released first, then WL is asserted after a short delay
    // to avoid VDD->BL->cell->VSS crowbar current.
    wire prech_off_A = (read_req_A || write_req_A) && clk;
    wire prech_off_B = read_req_B && clk;

    wire wl_any_fire_A;
    wire wl_any_fire_B;

    delay_cell #(.BUF_COUNT(2)) u_delay_wl_A (
        .A(prech_off_A),
        .Y(wl_any_fire_A)
    );

    delay_cell #(.BUF_COUNT(2)) u_delay_wl_B (
        .A(prech_off_B),
        .Y(wl_any_fire_B)
    );

    wire wl_read_fire_A  = wl_any_fire_A && read_req_A;
    wire wl_write_fire_A = wl_any_fire_A && write_req_A;

    wire wl_read_fire_B  = wl_any_fire_B && read_req_B;

    // Replica WL activity is driven only by read operations.
    wire replica_wl_t_fire = (wl_read_fire_A && bank_sel_r_A) ||
                             (wl_read_fire_B && bank_sel_r_B);
    wire replica_wl_b_fire = (wl_read_fire_A && !bank_sel_r_A) ||
                             (wl_read_fire_B && !bank_sel_r_B);

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
            state_B      <= 1'b0;
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
        next_state_B = 1'b0;
        if (!ce_n_B && !oe_n_B) next_state_B = 1'b1;
    end

    always_comb begin
        RWLT_A  = 1'b0;
        RWLB_A  = 1'b0;
        RWLT_B  = 1'b0;
        RWLB_B  = 1'b0;

        wlt_A       = '0;
        wlb_A       = '0;
        blprechtn_A = '0;
        blprechbn_A = '0;
        oeb_out_A   = '1;

        wlt_B       = '0;
        wlb_B       = '0;
        blprechtn_B = '0;
        blprechbn_B = '0;
        oeb_out_B   = '1;

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
        end

        if (read_req_A || write_req_A) begin
            if (bank_sel_r_A) begin
                blprechtn_A[slice_sel_r_A] = prech_off_A;
                blprechbn_A[slice_sel_r_A] = 1'b0;
            end else begin
                blprechtn_A[slice_sel_r_A] = 1'b0;
                blprechbn_A[slice_sel_r_A] = prech_off_A;
            end

            if (wl_any_fire_A) begin
                if (bank_sel_r_A) wlt_A[slice_sel_r_A][row_sel_r_A] = 1'b1;
                else              wlb_A[slice_sel_r_A][row_sel_r_A] = 1'b1;
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
            end

            if (write_req_A) begin
                wrena_A[slice_sel_r_A]  = clk;
                wrenan_A[slice_sel_r_A] = ~clk;
            end
        end

        if (read_req_B) begin
            if (bank_sel_r_B) begin
                blprechtn_B[slice_sel_r_B] = prech_off_B;
                blprechbn_B[slice_sel_r_B] = 1'b0;
            end else begin
                blprechtn_B[slice_sel_r_B] = 1'b0;
                blprechbn_B[slice_sel_r_B] = prech_off_B;
            end

            if (wl_any_fire_B) begin
                if (bank_sel_r_B) wlt_B[slice_sel_r_B][row_sel_r_B] = 1'b1;
                else              wlb_B[slice_sel_r_B][row_sel_r_B] = 1'b1;
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
            end
        end

        RWLT_A = wl_read_fire_A &&  bank_sel_r_A;
        RWLB_A = wl_read_fire_A && !bank_sel_r_A;
        RWLT_B = wl_read_fire_B &&  bank_sel_r_B;
        RWLB_B = wl_read_fire_B && !bank_sel_r_B;

        oe_out_A = ~oeb_out_A;
        oe_out_B = ~oeb_out_B;
    end

    // Replica BL precharge: asserted whenever any bank releases precharge on Port A/B.
    assign blprechn_rbl_A = |blprechtn_A | |blprechbn_A;
    assign blprechn_rbl_B = |blprechtn_B | |blprechbn_B;

endmodule