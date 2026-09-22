// alu64 -- 64-bit integer arithmetic for Nix integers.
//
// The 65816 has no multiply or divide instruction at all; a 64-bit multiply is a
// 64-iteration shift-add loop over four 16-bit words, thousands of cycles. Nix integers
// are 64-bit, so every `*`, `/` and `%` pays that. This unit does the same work in
// gates.
//
// Sequential by design. A combinational 64x64 multiplier is ~4k LUT4 on an ECP5; a
// 64-cycle shift-add is a few hundred. At ~48 card cycles per bus access, 64 cycles is
// about one access of latency -- invisible to the CPU, which is polling DONE anyway
// (docs/DECISIONS.md D5: latency the bus cannot see is free).
//
// Semantics follow Nix, which follows C: division truncates toward zero, and the
// remainder takes the sign of the dividend. So (-7)/2 = -3 and (-7)%2 = -1. Division
// by zero is an ERROR, never a silent wrap -- Nix throws, and so do we.
//
// Add, subtract, logic and compare are single-cycle; multiply, divide and modulo take
// 64 cycles plus sign fixup.
`default_nettype none

module alu64 (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        start,
    input  wire [3:0]  op,
    input  wire [63:0] a,
    input  wire [63:0] b,
    output reg         busy,
    output reg         done,       // one-cycle pulse
    output reg         err,        // divide by zero
    output reg  [63:0] result
);

    localparam [3:0] OP_ADD = 4'd0,
                     OP_SUB = 4'd1,
                     OP_MUL = 4'd2,
                     OP_DIV = 4'd3,
                     OP_MOD = 4'd4,
                     OP_AND = 4'd5,
                     OP_OR  = 4'd6,
                     OP_XOR = 4'd7,
                     OP_CMP = 4'd8,     // -1, 0, 1 as a signed 64-bit result
                     OP_NEG = 4'd9;

    // --- Sequential datapath ---
    reg [3:0]  cur_op;
    reg [63:0] acc;        // MUL: product accumulator.  DIV: quotient
    reg [63:0] rem;        // DIV: remainder. Always < divisor after a step, so 64
                           // bits suffice; only the shifted comparand needs 65.
    reg [63:0] x;          // MUL: multiplier (shifts right). DIV: dividend (shifts left)
    reg [63:0] y;          // MUL: multiplicand (shifts left). DIV: divisor magnitude
    reg [6:0]  cnt;
    reg        neg_q;      // quotient sign: operands differ
    reg        neg_r;      // remainder sign: follows the dividend

    // Magnitudes for signed divide. |INT64_MIN| is 2^63, which fits unsigned.
    wire [63:0] abs_a = a[63] ? (~a + 64'd1) : a;
    wire [63:0] abs_b = b[63] ? (~b + 64'd1) : b;

    // Signed compare, computed directly rather than by subtracting (which overflows).
    wire a_lt_b = ($signed(a) < $signed(b));
    wire a_eq_b = (a == b);

    // Restoring-division step: shift in the next dividend bit, then try to subtract.
    // rem_shift can reach 2^65-1 momentarily, so the compare is 65 bits wide -- but
    // the result is always < y < 2^64. When rem_ge holds, the 64-bit subtraction is
    // exact modulo 2^64 (the true difference fits), and when it does not, rem_shift
    // itself is < y. So a 64-bit rem_next loses nothing.
    wire [64:0] rem_shift = {rem, x[63]};
    wire        rem_ge    = (rem_shift >= {1'b0, y});
    wire [63:0] rem_next  = rem_ge ? (rem_shift[63:0] - y) : rem_shift[63:0];

    always @(posedge clk) begin
        if (!rst_n) begin
            busy   <= 1'b0;
            done   <= 1'b0;
            err    <= 1'b0;
            result <= 64'd0;
            cur_op <= OP_ADD;
            acc    <= 64'd0;
            rem    <= 64'd0;
            x      <= 64'd0;
            y      <= 64'd0;
            cnt    <= 7'd0;
            neg_q  <= 1'b0;
            neg_r  <= 1'b0;
        end else begin
            done <= 1'b0;

            if (start && !busy) begin
                err    <= 1'b0;
                cur_op <= op;
                case (op)
                    // --- single cycle ---
                    OP_ADD: begin result <= a + b;               done <= 1'b1; end
                    OP_SUB: begin result <= a - b;               done <= 1'b1; end
                    OP_AND: begin result <= a & b;               done <= 1'b1; end
                    OP_OR:  begin result <= a | b;               done <= 1'b1; end
                    OP_XOR: begin result <= a ^ b;               done <= 1'b1; end
                    OP_NEG: begin result <= ~a + 64'd1;          done <= 1'b1; end
                    OP_CMP: begin
                        result <= a_eq_b ? 64'd0
                                : a_lt_b ? {64{1'b1}}            // -1
                                : 64'd1;
                        done <= 1'b1;
                    end

                    // --- multiply: 64 shift-add steps on the raw bit patterns.
                    // The low 64 bits of a two's-complement product are the same
                    // whether you treat the operands as signed or not, so no sign
                    // handling is needed here at all. ---
                    OP_MUL: begin
                        acc  <= 64'd0;
                        x    <= a;
                        y    <= b;
                        cnt  <= 7'd0;
                        busy <= 1'b1;
                    end

                    // --- divide / modulo: unsigned restoring division on the
                    // magnitudes, then fix the signs afterwards. ---
                    OP_DIV, OP_MOD: begin
                        if (b == 64'd0) begin
                            err    <= 1'b1;        // loud, never a wrap
                            result <= 64'd0;
                            done   <= 1'b1;
                        end else begin
                            acc   <= 64'd0;
                            rem   <= 64'd0;
                            x     <= abs_a;
                            y     <= abs_b;
                            neg_q <= a[63] ^ b[63];
                            neg_r <= a[63];
                            cnt   <= 7'd0;
                            busy  <= 1'b1;
                        end
                    end

                    default: begin
                        err    <= 1'b1;
                        result <= 64'd0;
                        done   <= 1'b1;
                    end
                endcase
            end else if (busy) begin
                case (cur_op)
                    OP_MUL: begin
                        if (x[0]) acc <= acc + y;
                        x   <= x >> 1;
                        y   <= y << 1;
                        cnt <= cnt + 7'd1;
                        if (cnt == 7'd63) begin
                            // acc is updated this same edge, so compose the final
                            // value from the pre-update terms.
                            result <= x[0] ? (acc + y) : acc;
                            busy   <= 1'b0;
                            done   <= 1'b1;
                        end
                    end

                    OP_DIV, OP_MOD: begin
                        rem <= rem_next[63:0];
                        acc <= {acc[62:0], rem_ge};
                        x   <= x << 1;
                        cnt <= cnt + 7'd1;
                        if (cnt == 7'd63) begin
                            busy <= 1'b0;
                            done <= 1'b1;
                            if (cur_op == OP_DIV) begin
                                result <= neg_q ? (~{acc[62:0], rem_ge} + 64'd1)
                                                :   {acc[62:0], rem_ge};
                            end else begin
                                result <= neg_r ? (~rem_next[63:0] + 64'd1)
                                                :   rem_next[63:0];
                            end
                        end
                    end

                    default: begin
                        busy <= 1'b0;
                        err  <= 1'b1;
                        done <= 1'b1;
                    end
                endcase
            end
        end
    end

endmodule

`default_nettype wire
