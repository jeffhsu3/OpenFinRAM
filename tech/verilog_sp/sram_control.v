module ctrl_decode #(
    parameter ADDR_WIDTH = 10,
    parameter NUM_WL     = 32,
    parameter NUM_BANK  = 2,    // MUX 分組數量 
    // 時序參數 
    parameter DLY_PRECH_CNT = 30,
    parameter DLY_WL_CNT    = 5,
    parameter DLY_SENSE_CNT = 30,
    parameter DLY_WRITE_CNT = 10
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
    wire prech_end, wl_enable, sense_enable, write_wl_enable;
    wire read_active  = (state == READ)  && clk; 
    wire write_active = (state == WRITE) && clk; 

    delay_cell #(.BUF_COUNT(DLY_PRECH_CNT)) delay_prech (.A(read_active), .Y(prech_end)); 
    delay_cell #(.BUF_COUNT(DLY_WL_CNT))    delay_wl    (.A(prech_end),    .Y(wl_enable)); 
    delay_cell #(.BUF_COUNT(DLY_SENSE_CNT)) delay_sense (.A(wl_enable),   .Y(sense_enable)); 
    delay_cell #(.BUF_COUNT(DLY_WRITE_CNT)) delay_write_wl (.A(write_active), .Y(write_wl_enable)); 

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
        // --- 預設狀態 (Performance Mode): 預設 Precharge 開啟 (0) --- 
        wlt        = '0;
        wlb        = '0;
        blprechtn  = '0; // 0 為 Precharge ON 
        blprechbn  = '0;
        sae        = '0;
        saprechn   = '0;
        oeb_out    = '1; // 預設 NOR2 遮罩開啟 (1)，禁止輸出
        
        // Y-Select 預設值 
        for (int i = 0; i < NUM_BANK; i++) begin
            yselt[i]  = 4'b0000;
            yselb[i]  = 4'b0000;
            yseltn[i] = 4'b1111;
            yselbn[i] = 4'b1111;
            wrena[i]  = 1'b0;
            wrenan[i] = 1'b1;
        end

        case (state)
            READ: begin 
                oeb_out[slice_sel_r] = oe_n; // 被選中的 Slice 遵循外部 OE 
                
                //if (clk) begin 
                    if (bank_sel_r) begin 
                        blprechtn[slice_sel_r] = clk | wl_enable; // 延遲 Precharge 開啟直到 wl_enable 關閉
                        blprechbn[slice_sel_r] = 1'b0; // 讀取時關閉非選中 Bank Precharge 
                    end else begin 
                        blprechtn[slice_sel_r] = 1'b0; 
                        blprechbn[slice_sel_r] = clk | wl_enable; // 延遲 Precharge 開啟直到 wl_enable 關閉
                    end
                //end
                
                if (wl_enable) begin 
                    if (bank_sel_r) wlt[slice_sel_r][row_sel_r] = 1'b1; 
                    else            wlb[slice_sel_r][row_sel_r] = 1'b1; 
                end

                if (bank_sel_r) begin 
                    yselt[slice_sel_r]  = (4'b0001 << col_sel_r); 
                    yseltn[slice_sel_r] = ~yselt[slice_sel_r]; 
                end else begin 
                    yselb[slice_sel_r]  = (4'b0001 << col_sel_r); 
                    yselbn[slice_sel_r] = ~yselb[slice_sel_r]; 
                end
                
                sae[slice_sel_r]      = sense_enable; 
                saprechn[slice_sel_r] = sense_enable; 
            end
            
            WRITE: begin 
                if (bank_sel_r) begin 
                    blprechtn[slice_sel_r] = clk | write_wl_enable;  // 延遲 Precharge 開啟直到 write_wl_enable 關閉
                    blprechbn[slice_sel_r] = 1'b0; // 非選中側: 維持 Precharge ON 
                end else begin
                    blprechtn[slice_sel_r] = 1'b0; 
                    blprechbn[slice_sel_r] = clk | write_wl_enable;  // 延遲 Precharge 開啟直到 write_wl_enable 關閉
                end
                
                if (write_wl_enable) begin 
                    if (bank_sel_r) wlt[slice_sel_r][row_sel_r] = 1'b1; 
                    else            wlb[slice_sel_r][row_sel_r] = 1'b1; 
                end

                if (bank_sel_r) begin 
                    yselt[slice_sel_r]  = (4'b0001 << col_sel_r); 
                    yseltn[slice_sel_r] = ~yselt[slice_sel_r]; 
                end else begin 
                    yselb[slice_sel_r]  = (4'b0001 << col_sel_r); 
                    yselbn[slice_sel_r] = ~yselb[slice_sel_r]; 
                end

                wrena[slice_sel_r]  = clk; 
                wrenan[slice_sel_r] = ~clk;
            end
        endcase

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