// SHA-256 block compression function.
//
// One round per cycle: 64 cycles per 512-bit block. See docs/DECISIONS.md D8 for why
// this is deliberately a SINGLE core and not multi-buffer SIMD -- one core at 50 MHz
// already runs ~150x faster than the Apple II slot bus can feed it.
//
// Usage:
//   init         -> load the SHA-256 IV into the running digest
//   start+block  -> compress one padded 512-bit block (big-endian word order)
//   busy falls   -> `digest` holds the updated chaining value
`default_nettype none

module sha256_core (
    input  wire          clk,
    input  wire          rst_n,
    input  wire          init,      // load IV
    input  wire          start,     // compress `block`
    input  wire [511:0]  block,     // word 0 in the MSBs
    output reg           busy,
    output wire [255:0]  digest
);

    // Running chaining value H0..H7.
    reg [31:0] h0, h1, h2, h3, h4, h5, h6, h7;
    // Working registers a..h.
    reg [31:0] a, b, c, d, e, f, g, h;
    // Message schedule window: w[i] == W[t+i] at the start of round t.
    reg [31:0] w [0:15];
    reg [6:0]  round;

    assign digest = {h0, h1, h2, h3, h4, h5, h6, h7};

    // --- Round constants (fractional parts of cube roots of the first 64 primes) ---
    function [31:0] kconst(input [5:0] t);
        case (t)
            6'd00: kconst = 32'h428a2f98;  6'd01: kconst = 32'h71374491;
            6'd02: kconst = 32'hb5c0fbcf;  6'd03: kconst = 32'he9b5dba5;
            6'd04: kconst = 32'h3956c25b;  6'd05: kconst = 32'h59f111f1;
            6'd06: kconst = 32'h923f82a4;  6'd07: kconst = 32'hab1c5ed5;
            6'd08: kconst = 32'hd807aa98;  6'd09: kconst = 32'h12835b01;
            6'd10: kconst = 32'h243185be;  6'd11: kconst = 32'h550c7dc3;
            6'd12: kconst = 32'h72be5d74;  6'd13: kconst = 32'h80deb1fe;
            6'd14: kconst = 32'h9bdc06a7;  6'd15: kconst = 32'hc19bf174;
            6'd16: kconst = 32'he49b69c1;  6'd17: kconst = 32'hefbe4786;
            6'd18: kconst = 32'h0fc19dc6;  6'd19: kconst = 32'h240ca1cc;
            6'd20: kconst = 32'h2de92c6f;  6'd21: kconst = 32'h4a7484aa;
            6'd22: kconst = 32'h5cb0a9dc;  6'd23: kconst = 32'h76f988da;
            6'd24: kconst = 32'h983e5152;  6'd25: kconst = 32'ha831c66d;
            6'd26: kconst = 32'hb00327c8;  6'd27: kconst = 32'hbf597fc7;
            6'd28: kconst = 32'hc6e00bf3;  6'd29: kconst = 32'hd5a79147;
            6'd30: kconst = 32'h06ca6351;  6'd31: kconst = 32'h14292967;
            6'd32: kconst = 32'h27b70a85;  6'd33: kconst = 32'h2e1b2138;
            6'd34: kconst = 32'h4d2c6dfc;  6'd35: kconst = 32'h53380d13;
            6'd36: kconst = 32'h650a7354;  6'd37: kconst = 32'h766a0abb;
            6'd38: kconst = 32'h81c2c92e;  6'd39: kconst = 32'h92722c85;
            6'd40: kconst = 32'ha2bfe8a1;  6'd41: kconst = 32'ha81a664b;
            6'd42: kconst = 32'hc24b8b70;  6'd43: kconst = 32'hc76c51a3;
            6'd44: kconst = 32'hd192e819;  6'd45: kconst = 32'hd6990624;
            6'd46: kconst = 32'hf40e3585;  6'd47: kconst = 32'h106aa070;
            6'd48: kconst = 32'h19a4c116;  6'd49: kconst = 32'h1e376c08;
            6'd50: kconst = 32'h2748774c;  6'd51: kconst = 32'h34b0bcb5;
            6'd52: kconst = 32'h391c0cb3;  6'd53: kconst = 32'h4ed8aa4a;
            6'd54: kconst = 32'h5b9cca4f;  6'd55: kconst = 32'h682e6ff3;
            6'd56: kconst = 32'h748f82ee;  6'd57: kconst = 32'h78a5636f;
            6'd58: kconst = 32'h84c87814;  6'd59: kconst = 32'h8cc70208;
            6'd60: kconst = 32'h90befffa;  6'd61: kconst = 32'ha4506ceb;
            6'd62: kconst = 32'hbef9a3f7;  6'd63: kconst = 32'hc67178f2;
        endcase
    endfunction

    // --- Round functions. ROTR(x,n) == {x[n-1:0], x[31:n]} ---
    wire [31:0] big_s0 = {a[ 1:0], a[31: 2]} ^ {a[12:0], a[31:13]} ^ {a[21:0], a[31:22]};
    wire [31:0] big_s1 = {e[ 5:0], e[31: 6]} ^ {e[10:0], e[31:11]} ^ {e[24:0], e[31:25]};
    wire [31:0] ch     = (e & f) ^ (~e & g);
    wire [31:0] maj    = (a & b) ^ (a & c) ^ (b & c);

    wire [31:0] t1 = h + big_s1 + ch + kconst(round[5:0]) + w[0];
    wire [31:0] t2 = big_s0 + maj;

    // Next scheduled word: W[t+16] = s1(W[t+14]) + W[t+9] + s0(W[t+1]) + W[t]
    wire [31:0] sml_s0 = {w[1][ 6:0], w[1][31: 7]} ^ {w[1][17:0], w[1][31:18]} ^ (w[1] >>  3);
    wire [31:0] sml_s1 = {w[14][16:0], w[14][31:17]} ^ {w[14][18:0], w[14][31:19]} ^ (w[14] >> 10);
    wire [31:0] w_next = sml_s1 + w[9] + sml_s0 + w[0];

    integer i;
    always @(posedge clk) begin
        if (!rst_n) begin
            busy  <= 1'b0;
            round <= 7'd0;
            h0 <= 32'h6a09e667; h1 <= 32'hbb67ae85;
            h2 <= 32'h3c6ef372; h3 <= 32'ha54ff53a;
            h4 <= 32'h510e527f; h5 <= 32'h9b05688c;
            h6 <= 32'h1f83d9ab; h7 <= 32'h5be0cd19;
        end else if (init) begin
            busy  <= 1'b0;
            round <= 7'd0;
            h0 <= 32'h6a09e667; h1 <= 32'hbb67ae85;
            h2 <= 32'h3c6ef372; h3 <= 32'ha54ff53a;
            h4 <= 32'h510e527f; h5 <= 32'h9b05688c;
            h6 <= 32'h1f83d9ab; h7 <= 32'h5be0cd19;
        end else if (start && !busy) begin
            a <= h0; b <= h1; c <= h2; d <= h3;
            e <= h4; f <= h5; g <= h6; h <= h7;
            for (i = 0; i < 16; i = i + 1)
                w[i] <= block[511 - 32*i -: 32];
            round <= 7'd0;
            busy  <= 1'b1;
        end else if (busy) begin
            // Compress one round.
            h <= g; g <= f; f <= e;
            e <= d + t1;
            d <= c; c <= b; b <= a;
            a <= t1 + t2;

            for (i = 0; i < 15; i = i + 1)
                w[i] <= w[i+1];
            w[15] <= w_next;

            if (round == 7'd63) begin
                busy <= 1'b0;
                // Feed-forward: H += working registers.
                h0 <= h0 + (t1 + t2);
                h1 <= h1 + a;
                h2 <= h2 + b;
                h3 <= h3 + c;
                h4 <= h4 + (d + t1);
                h5 <= h5 + e;
                h6 <= h6 + f;
                h7 <= h7 + g;
            end else begin
                round <= round + 7'd1;
            end
        end
    end

endmodule

`default_nettype wire
