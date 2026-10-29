// SPDX-License-Identifier: GPL-3.0-or-later
module top (
    input  wire         clk25,       // 25 MHz on the module
    /* the slot address bus, (with parens) */
    input  wire [15:0]  slot_a,
    inout  wire [7:0]   slot_d,
    output wire         uart_tx
);
    wire unrelated;
endmodule
