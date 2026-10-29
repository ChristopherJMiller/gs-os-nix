// SPDX-License-Identifier: GPL-3.0-or-later
//
// carrier_top: the FPGA's top level on the carrier board (docs/HARDWARE.md). The port
// list is the contract with hw/carrier/pins.toml (checked by hw-pinmap); the bus
// section's translators, buffers and open-drain drivers sit between these ports and the
// slot. This stub drives nothing: every output is off or released, so a board loaded with
// it cannot disturb the slot bus. The gateware (plan 3) fills the body.
module carrier_top (
    input  wire        clk25,
    input  wire [15:0] slot_a,      input  wire       slot_rw,
    inout  wire [7:0]  slot_d,      input  wire [7:0] slot_dl,
    input  wire        slot_ph0, slot_ph1, slot_q3, slot_7m,
    input  wire        slot_devsel_n, slot_iosel_n, slot_iostrb_n,
    input  wire        slot_m2sel_n, slot_m2b0, slot_reset_n, slot_sync_n,
    output wire        slot_d_dir, slot_d_oe_n, slot_a_dir, slot_a_oe_n,
    output wire        slot_irq_n_o, slot_nmi_n_o, slot_inh_n_o,
    output wire        slot_dma_n_o, slot_rdy_o, slot_reset_n_o,
    input  wire        safe_n, uart_rx,
    output wire        uart_tx
);
    /* verilator lint_off UNUSEDSIGNAL */
    // Inputs the stub does not read yet.
    wire unused = &{1'b0, clk25, slot_a, slot_rw, slot_dl, slot_ph0, slot_ph1, slot_q3,
                    slot_7m, slot_devsel_n, slot_iosel_n, slot_iostrb_n, slot_m2sel_n,
                    slot_m2b0, slot_reset_n, slot_sync_n, safe_n, uart_rx};
    /* verilator lint_on UNUSEDSIGNAL */

    // Translators off (/OE high), slot -> FPGA (DIR low).
    assign slot_d_oe_n = 1'b1;
    assign slot_d_dir  = 1'b0;
    assign slot_a_oe_n = 1'b1;
    assign slot_a_dir  = 1'b0;
    // Every open-drain output released; /RESET released.
    assign slot_irq_n_o   = 1'b1;
    assign slot_nmi_n_o   = 1'b1;
    assign slot_inh_n_o   = 1'b1;
    assign slot_dma_n_o   = 1'b1;
    assign slot_rdy_o     = 1'b1;
    assign slot_reset_n_o = 1'b1;
    assign uart_tx        = 1'b1;
    // The data bus is not driven.
    assign slot_d = 8'bz;
endmodule
