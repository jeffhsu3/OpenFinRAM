// Behavioral stand-ins for the ASAP7 structural cells instantiated in
// sram_control.v / delay_cell.v, so the decoder RTL can be simulated.
// The delay chains become logically transparent (even inverter count -> Y==A),
// which is exactly what we want for a *functional* decode-correctness check.
module INVx1_ASAP7_75t_R (input A, output Y);
    assign Y = ~A;
endmodule

module BUFx4_ASAP7_75t_R (input A, output Y);
    assign Y = A;
endmodule
