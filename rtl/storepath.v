// Store-path tail: compress a SHA-256 digest to 160 bits and nixbase32-encode it.
//
// This is the second half of the STOREPATH verb (docs/CARD-ISA.md $14). The first
// half -- hashing the fingerprint string "type:sha256:inner:store:name" -- is done by
// the sha256 module; this takes that digest and produces the 32-character path
// component.
//
// Purely combinational: all 32 output characters are produced in parallel in one
// cycle. This is the "SIMD pays here" case from docs/DECISIONS.md D8 -- the XOR fold
// is literally wires, and the encode is 32 independent 5-bit table lookups. Doing it
// on the 65816 would mean 32 iterations of cross-byte bit extraction on a CPU with no
// barrel shifter.
//
// Verified behaviour (docs/REFERENCES.md):
//   fold:     result[i % 20] ^= h[i]   over the 32 digest bytes
//   alphabet: "0123456789abcdfghijklmnpqrsvwxyz" (no e, o, u, t)
//   encode:   emitted FROM THE END -- output char k takes 5-bit group (31-k),
//             where group n is bits [5n +: 5] of the folded value read little-endian
`default_nettype none

module storepath (
    input  wire [255:0] digest,   // SHA-256, byte 0 in the MSBs (canonical order)
    output wire [159:0] folded,   // 160-bit compressed hash, byte 0 in the LSBs
    output wire [255:0] path      // 32 ASCII chars, first char in the MSBs
);

    // --- Digest bytes: sha[i] is byte i of the canonical digest ---
    wire [7:0] sha [0:31];
    genvar i;
    generate
        for (i = 0; i < 32; i = i + 1) begin : g_bytes
            assign sha[i] = digest[255 - 8*i -: 8];
        end
    endgenerate

    // --- XOR fold to 20 bytes: folded[j] = sha[j] ^ sha[j+20] for j < 12 ---
    wire [7:0] fold_b [0:19];
    generate
        for (i = 0; i < 20; i = i + 1) begin : g_fold
            if (i < 12) begin : g_wrapped
                assign fold_b[i] = sha[i] ^ sha[i + 20];
            end else begin : g_plain
                assign fold_b[i] = sha[i];
            end
        end
    endgenerate

    // Little-endian packing: byte 0 occupies the low bits, which is what makes the
    // 5-bit group extraction below a plain bit slice.
    generate
        for (i = 0; i < 20; i = i + 1) begin : g_pack
            assign folded[8*i +: 8] = fold_b[i];
        end
    endgenerate

    // --- nixbase32 ---
    function [7:0] b32char(input [4:0] v);
        case (v)
            5'd0:  b32char = "0"; 5'd1:  b32char = "1"; 5'd2:  b32char = "2";
            5'd3:  b32char = "3"; 5'd4:  b32char = "4"; 5'd5:  b32char = "5";
            5'd6:  b32char = "6"; 5'd7:  b32char = "7"; 5'd8:  b32char = "8";
            5'd9:  b32char = "9"; 5'd10: b32char = "a"; 5'd11: b32char = "b";
            5'd12: b32char = "c"; 5'd13: b32char = "d"; 5'd14: b32char = "f";
            5'd15: b32char = "g"; 5'd16: b32char = "h"; 5'd17: b32char = "i";
            5'd18: b32char = "j"; 5'd19: b32char = "k"; 5'd20: b32char = "l";
            5'd21: b32char = "m"; 5'd22: b32char = "n"; 5'd23: b32char = "p";
            5'd24: b32char = "q"; 5'd25: b32char = "r"; 5'd26: b32char = "s";
            5'd27: b32char = "v"; 5'd28: b32char = "w"; 5'd29: b32char = "x";
            5'd30: b32char = "y"; 5'd31: b32char = "z";
        endcase
    endfunction

    // Output char k comes from group (31-k): "processes the hash from the end".
    generate
        for (i = 0; i < 32; i = i + 1) begin : g_enc
            assign path[255 - 8*i -: 8] = b32char(folded[5*(31 - i) +: 5]);
        end
    endgenerate

endmodule

`default_nettype wire
