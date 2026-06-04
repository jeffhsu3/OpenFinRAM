module delay_tap_select #(
    parameter integer SHORT_BUF_COUNT = 4,
    parameter integer LONG_BUF_COUNT  = 12
) (
    input  A,
    input  S,
    output Y
);

    wire short_tap;
    wire long_tap;

    delay_cell #(
        .BUF_COUNT(SHORT_BUF_COUNT)
    ) u_short_delay (
        .A(A),
        .Y(short_tap)
    );

    delay_cell #(
        .BUF_COUNT(LONG_BUF_COUNT)
    ) u_long_delay (
        .A(A),
        .Y(long_tap)
    );

    assign Y = S ? long_tap : short_tap;

endmodule

module ctrl_decode #(
    parameter integer ADDR_WIDTH = 8,
    parameter integer NUM_WL     = 32,
    parameter integer NUM_BANK   = 4
) (
    input  wire        clk,
    input  wire        ce_n,
    input  wire        we_n,
    input  wire        oe_n,
    input  wire [3:0]  sdel,
    input  wire [ADDR_WIDTH-1:0] A,
    output logic [NUM_BANK-1:0][NUM_WL-1:0] wlt,
    output logic [NUM_BANK-1:0][NUM_WL-1:0] wlb,
    output logic [NUM_BANK-1:0]             blprechtn,
    output logic [NUM_BANK-1:0]             blprechbn,
    output logic [NUM_BANK-1:0][3:0]        yselt,
    output logic [NUM_BANK-1:0][3:0]        yseltn,
    output logic [NUM_BANK-1:0][3:0]        yselb,
    output logic [NUM_BANK-1:0][3:0]        yselbn,
    output logic [NUM_BANK-1:0]             sae,
    output logic [NUM_BANK-1:0]             saprechn,
    output logic [NUM_BANK-1:0]             wrena,
    output logic [NUM_BANK-1:0]             wrenan,
    output logic [NUM_BANK-1:0]             oeb_out,
    output logic [NUM_BANK-1:0]             oe_out
);

    localparam integer COL_SEL_BITS  = 2;
    localparam integer ROW_ADDR_WIDTH = $clog2(NUM_WL);

    localparam integer ROW_ADDR_LSB = 0;
    localparam integer ROW_ADDR_MSB = ROW_ADDR_WIDTH - 1;
    localparam integer COL_ADDR_LSB = ROW_ADDR_WIDTH;
    localparam integer COL_ADDR_MSB = ROW_ADDR_WIDTH + COL_SEL_BITS - 1;

    localparam integer BANK_SEL_BITS = (NUM_BANK > 1) ? $clog2(NUM_BANK) : 0;
    localparam integer BANK_ADDR_BIT = ROW_ADDR_WIDTH + COL_SEL_BITS;

    reg [ADDR_WIDTH-1:0] A_q;
    reg [3:0] sdel_q;
    reg       CEN_q;
    reg       WEN_q;
    reg       OEN_q;

    wire access_q;
    wire read_q;
    wire write_q;

    wire phase_blprech;
    wire phase_wl;
    wire phase_sae;
    wire phase_saprech;

    reg [3:0] ysel_onehot;
    wire [ROW_ADDR_WIDTH-1:0] row_addr_q;

    assign row_addr_q = A_q[ROW_ADDR_MSB:ROW_ADDR_LSB];

    assign access_q = ~CEN_q & (~WEN_q | ~OEN_q);
    assign read_q   = ~CEN_q &  WEN_q & ~OEN_q;
    assign write_q  = ~CEN_q & ~WEN_q;

    always @(posedge clk) begin
        // if (!clk) begin
            A_q    <= A;
            sdel_q <= sdel;
            CEN_q  <= ce_n;
            WEN_q  <= we_n;
            OEN_q  <= oe_n;
        // end
    end

    delay_tap_select #(
        .SHORT_BUF_COUNT(4),
        .LONG_BUF_COUNT(12)
    ) u_phase_blprech (
        .A(clk),
        .S(sdel_q[0]),
        .Y(phase_blprech)
    );

    delay_tap_select #(
        .SHORT_BUF_COUNT(4),
        .LONG_BUF_COUNT(12)
    ) u_phase_wl (
        .A(phase_blprech),
        .S(sdel_q[1]),
        .Y(phase_wl)
    );

    delay_tap_select #(
        .SHORT_BUF_COUNT(4),
        .LONG_BUF_COUNT(12)
    ) u_phase_sae (
        .A(phase_wl),
        .S(sdel_q[2]),
        .Y(phase_sae)
    );

    delay_tap_select #(
        .SHORT_BUF_COUNT(2),
        .LONG_BUF_COUNT(4)
    ) u_phase_saprech (
        .A(phase_sae),
        .S(sdel_q[3]),
        .Y(phase_saprech)
    );

    wire [NUM_BANK-1:0] bank_sel;
    generate
        if (NUM_BANK > 1) begin : g_bank_decode
            wire [BANK_SEL_BITS-1:0] bank_addr_q = A_q[BANK_ADDR_BIT + BANK_SEL_BITS : BANK_ADDR_BIT + 1];
            assign bank_sel = (1'b1 << bank_addr_q);
        end else begin : g_single_bank
            assign bank_sel = 1'b1;
        end
    endgenerate

    always @(*) begin
        ysel_onehot = 4'b0000;
        ysel_onehot[A_q[COL_ADDR_MSB:COL_ADDR_LSB]] = 1'b1;
    end

    genvar i;
    generate
        for (i = 0; i < NUM_BANK; i = i + 1) begin : g_bank_logic
            wire bank_active = bank_sel[i];

            wire top_selected    = A_q[BANK_ADDR_BIT];
            wire bottom_selected = ~A_q[BANK_ADDR_BIT];

            wire wlenat_int = access_q & phase_wl & bank_active &  top_selected;
            wire wlenab_int = access_q & phase_wl & bank_active & bottom_selected;

            assign wlt[i] = wlenat_int ? ({{(NUM_WL-1){1'b0}}, 1'b1} << row_addr_q) : {NUM_WL{1'b0}};
            assign wlb[i] = wlenab_int ? ({{(NUM_WL-1){1'b0}}, 1'b1} << row_addr_q) : {NUM_WL{1'b0}};

            assign blprechtn[i] = (access_q & bank_active & top_selected) ? phase_blprech : 1'b0;
            assign blprechbn[i] = (access_q & bank_active & bottom_selected) ? phase_blprech : 1'b0;

            assign wrena[i]  = write_q & phase_wl & bank_active;
            assign wrenan[i] = ~wrena[i];

            assign oe_out[i]  = read_q & bank_active;
            assign oeb_out[i] = ~oe_out[i];

            assign sae[i]       = read_q & phase_sae & bank_active;
            assign saprechn[i]  = read_q & phase_saprech & bank_active;

            always @(*) begin
                yselt[i]  = 4'b0000;
                yseltn[i] = 4'b1111;
                yselb[i]  = 4'b0000;
                yselbn[i] = 4'b1111;

                if (access_q & bank_active) begin
                    if (A_q[BANK_ADDR_BIT]) begin
                        yselt[i]  = ysel_onehot;
                        yseltn[i] = ~ysel_onehot;
                    end else begin
                        yselb[i]  = ysel_onehot;
                        yselbn[i] = ~ysel_onehot;
                    end
                end
            end
        end
    endgenerate

endmodule
