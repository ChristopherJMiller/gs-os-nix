// keycam -- a 32-wide content-addressable match over attribute-set key hashes.
//
// Attribute lookup is the hottest operation in Nix evaluation (docs/ARCHITECTURE.md),
// and this is the one place in the design where SIMD genuinely pays: comparing 32
// stored hashes against a search hash is 32 independent 32-bit comparators, all in one
// cycle, instead of a chain of pointer-chasing probes.
//
// docs/HEAP.md stores ATTRS as three parallel columns rather than interleaved records
// precisely so this array can be filled by a straight sequential read.
//
// Purely combinational. The caller loads `entries`, presents `key`, and reads `hit`
// and `index` in the same cycle.
//
// A hash match is a CANDIDATE, not an answer: 32-bit hashes collide. The caller must
// still compare the attribute's name. Treating a match as authoritative would silently
// return the wrong attribute -- a value of the right shape from the wrong key.
`default_nettype none

module keycam #(
    parameter integer WIDTH = 32          // lanes compared per pass
) (
    input  wire [31:0]              key,
    input  wire [32*WIDTH-1:0]      entries,   // WIDTH packed 32-bit hashes, lane 0 low
    input  wire [WIDTH-1:0]         lane_valid,
    output wire                     hit,
    output wire [$clog2(WIDTH)-1:0] index      // lowest matching lane
);

    // --- 32 parallel comparators ---
    wire [WIDTH-1:0] match;
    genvar i;
    generate
        for (i = 0; i < WIDTH; i = i + 1) begin : g_cmp
            assign match[i] = lane_valid[i] && (entries[32*i +: 32] == key);
        end
    endgenerate

    assign hit = |match;

    // --- Priority encoder: lowest set lane ---
    // Attributes are stored sorted by name, so the lowest matching lane is the
    // earliest candidate and the one a sorted-order search would have found.
    reg [$clog2(WIDTH)-1:0] idx;
    integer j;
    always @(*) begin
        idx = {$clog2(WIDTH){1'b0}};
        for (j = WIDTH - 1; j >= 0; j = j - 1)
            if (match[j]) idx = j[$clog2(WIDTH)-1:0];
    end
    assign index = idx;

endmodule

`default_nettype wire
