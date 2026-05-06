module ctrl_decode #(
    parameter ADDR_WIDTH = 10,
    parameter NUM_WL     = 32,
    parameter NUM_BANK  = 2,    // MUX 分組數量 
    // 時序參數 
    parameter WL_BUF     = 5,
    parameter SAE_BUF    = 15
)(
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  ce_n,
    input  logic                  we_n,
    input  logic [ADDR_WIDTH-1:0] A,
    input  logic                  oe_n,       // 系統輸出致能 
    
    // 使用二維陣列定義輸出接腳 (SystemVerilog 特性)
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
    output logic [NUM_BANK-1:0]             oeb_out,   // 輸出至 NOR2 閘 
    output logic [NUM_BANK-1:0]             oe_out     // 直接控制輸出 OE 
);

    // --- 1. 地址解碼 (Address Decoding) --- 
    function integer clog2;
        input integer value;
        begin
            value = value - 1; 
            for (clog2 = 0; value > 0; clog2 = clog2 + 1) 
                value = value >> 1;
        end
    endfunction

    localparam ROW_BITS      = clog2(NUM_WL); 
    localparam Y_BITS        = 2; 
    localparam SLICE_BITS = (NUM_BANK > 1) ? clog2(NUM_BANK) : 1;
    localparam BANK_BIT_IDX  = ROW_BITS + Y_BITS; 
    localparam SLICE_BIT_IDX = BANK_BIT_IDX + 1; 

    wire [ROW_BITS-1:0]   row_sel_d   = A[ROW_BITS-1 : 0]; 
    wire [1:0]            col_sel_d   = A[BANK_BIT_IDX-1 : ROW_BITS]; 
    wire                  bank_sel_d  = A[BANK_BIT_IDX]; 
    wire [SLICE_BITS-1:0] slice_sel_d = (NUM_BANK > 1) ? A[SLICE_BIT_IDX + SLICE_BITS - 1 : SLICE_BIT_IDX] : 1'b0;

    logic [ROW_BITS-1:0]   row_sel_r;
    logic [1:0]            col_sel_r;
    logic                  bank_sel_r;
    logic [SLICE_BITS-1:0] slice_sel_r;

    // --- 2. 時序控制邏輯 --- 
    localparam IDLE  = 2'b11; 
    localparam READ  = 2'b01; 
    localparam WRITE = 2'b10; 
    
    logic [1:0] state; 
    logic [1:0] next_state; // 確保這行存在於模組內部的頂層
    wire start_op = !ce_n;

    wire read_req  = (state == READ)  && !ce_n;
    wire write_req = (state == WRITE) && !ce_n;

    wire prech_off = (read_req || write_req) && clk;

    wire wl_any_fire;
    delay_cell #(.BUF_COUNT(WL_BUF)) u_delay_wl (
        .A(prech_off),
        .Y(wl_any_fire)      // 讀寫共用真實的物理 WL 延遲
    );

    wire wl_read_fire = wl_any_fire && read_req;
    
    wire sae_raw;
    delay_cell #(.BUF_COUNT(SAE_BUF)) u_delay_sae (
        .A(wl_read_fire),
        .Y(sae_raw)          // SAE 嚴格跟隨 Read WL 之後觸發
    );

    // Latch decoded address at transaction start to keep outputs stable across a cycle.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            row_sel_r   <= '0;
            col_sel_r   <= '0;
            bank_sel_r  <= 1'b0;
            slice_sel_r <= '0;
        end else if (start_op) begin
            row_sel_r   <= row_sel_d;
            col_sel_r   <= col_sel_d;
            bank_sel_r  <= bank_sel_d;
            slice_sel_r <= slice_sel_d;
        end
    end

    // --- 3. 組合邏輯 (二維陣列處理) --- 
    always_comb begin
        // 預設狀態
        wlt        = '0;
        wlb        = '0;
        blprechtn  = '0; // 0 為 Precharge ON 
        blprechbn  = '0;
        sae        = '0;
        saprechn   = '0;
        oeb_out    = '1; 
        
        for (int i = 0; i < NUM_BANK; i++) begin
            yselt[i]  = 4'b0000;
            yselb[i]  = 4'b0000;
            yseltn[i] = 4'b1111;
            yselbn[i] = 4'b1111;
            wrena[i]  = 1'b0;
            wrenan[i] = 1'b1;
        end

        // 整合 Read 與 Write 共用的 Precharge 與 WL 邏輯
        if (read_req || write_req) begin
            // 直接以 prech_off 驅動，避免 glitch
            if (bank_sel_r) begin 
                blprechtn[slice_sel_r] = prech_off;
                blprechbn[slice_sel_r] = 1'b0; 
            end else begin 
                blprechtn[slice_sel_r] = 1'b0; 
                blprechbn[slice_sel_r] = prech_off; 
            end
            
            // WL 共用同一條 Delay 路徑
            if (wl_any_fire) begin 
                if (bank_sel_r) wlt[slice_sel_r][row_sel_r] = 1'b1; 
                else            wlb[slice_sel_r][row_sel_r] = 1'b1; 
            end

            // Y-Mux 共用邏輯
            if (bank_sel_r) begin 
                yselt[slice_sel_r]  = (4'b0001 << col_sel_r); 
                yseltn[slice_sel_r] = ~yselt[slice_sel_r]; 
            end else begin 
                yselb[slice_sel_r]  = (4'b0001 << col_sel_r); 
                yselbn[slice_sel_r] = ~yselb[slice_sel_r]; 
            end

            // 處理 Read 專屬訊號
            if (read_req) begin
                oeb_out[slice_sel_r]  = oe_n;
                sae[slice_sel_r]      = sae_raw; 
                saprechn[slice_sel_r] = sae_raw; 
            end

            // 處理 Write 專屬訊號
            if (write_req) begin
                wrena[slice_sel_r]  = clk; 
                wrenan[slice_sel_r] = ~clk;
            end
        end

        oe_out = ~oeb_out;
    end

    // --- 4. 狀態機切換 (FSM) --- 
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) state <= IDLE; 
        else state <= next_state; 
    end
    
    always_comb begin
        next_state = IDLE; 
        if (!ce_n) begin
            if (!we_n && oe_n)      next_state = WRITE;
            else if (we_n && !oe_n) next_state = READ;
            else                    next_state = IDLE;
        end
    end

endmodule