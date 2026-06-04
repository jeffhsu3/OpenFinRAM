module delay_cell #(
    parameter BUF_COUNT = 10
) (
    input  A,
    output Y
);

    wire [2*BUF_COUNT:0] chain_wires;
    assign chain_wires[0] = A;

    genvar i;
    generate
        for (i = 0; i < 2*BUF_COUNT; i = i + 1) begin: inv_chain_loop
            INVx1_ASAP7_75t_R u_inv (
                .A(chain_wires[i]),
                .Y(chain_wires[i+1])
            );
        end
    endgenerate

    assign Y = chain_wires[2*BUF_COUNT];
endmodule