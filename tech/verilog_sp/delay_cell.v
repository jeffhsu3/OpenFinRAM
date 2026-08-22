module delay_cell #(
    parameter BUF_COUNT = 10
) (
    input  A,
    output Y
);

    // This is a physical timing element, not a Boolean implementation of a
    // buffer.  An even inverter chain is logically transparent, so synthesis
    // will otherwise collapse the entire chain (and the sdel mux selecting
    // between two chain lengths).  The keep attributes are understood by
    // Yosys; the synthesis flow also reads the ASAP7 cells as black boxes and
    // checks that every physical inverter survives in the mapped netlist.
    (* keep *) wire [2*BUF_COUNT-2:0] chain_wires;

    (* keep, dont_touch, delay_input *)
    INVx1_ASAP7_75t_R u_first_inv (
        .A(A),
        .Y(chain_wires[0])
    );

    genvar i;
    generate
        for (i = 1; i < 2*BUF_COUNT-1; i = i + 1) begin: inv_chain_loop
            (* keep, dont_touch *)
            INVx1_ASAP7_75t_R u_inv (
                .A(chain_wires[i-1]),
                .Y(chain_wires[i])
            );
        end
    endgenerate

    (* keep, dont_touch *)
    INVx1_ASAP7_75t_R u_last_inv (
        .A(chain_wires[2*BUF_COUNT-2]),
        .Y(Y)
    );
endmodule
