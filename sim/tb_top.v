// Verilation top for the L1 hashing tests: sha256 feeding storepath.
//
// This is the same composition the card's STOREPATH verb performs -- hash the
// fingerprint string, then fold and encode -- so testing it here tests the real path.
`default_nettype none

module tb_top (
    input  wire          clk,
    input  wire          rst_n,
    input  wire          init,
    input  wire          in_valid,
    input  wire [7:0]    in_byte,
    output wire          in_ready,
    input  wire          finish,
    output wire          done,
    output wire [255:0]  digest,
    output wire [159:0]  folded,
    output wire [255:0]  path
);

    sha256 u_hash (
        .clk      (clk),
        .rst_n    (rst_n),
        .init     (init),
        .in_valid (in_valid),
        .in_byte  (in_byte),
        .in_ready (in_ready),
        .finish   (finish),
        .done     (done),
        .digest   (digest)
    );

    storepath u_sp (
        .digest (digest),
        .folded (folded),
        .path   (path)
    );

endmodule

`default_nettype wire
