// SHA-256 with byte-stream input and FIPS-180-4 padding.
//
// Wraps sha256_core with a block assembler and the padding state machine, so callers
// push bytes and assert `finish`. This is the module the card's HASH_STREAM verb
// drives (see docs/CARD-ISA.md).
//
// Protocol:
//   init                     -> begin a new message
//   in_valid/in_byte         -> push a byte while in_ready is high
//   finish                   -> no more data; pad and compress
//   done                     -> `digest` is valid
`default_nettype none

module sha256 (
    input  wire          clk,
    input  wire          rst_n,
    input  wire          init,
    input  wire          in_valid,
    input  wire [7:0]    in_byte,
    output wire          in_ready,
    input  wire          finish,
    output reg           done,
    output wire [255:0]  digest
);

    localparam [2:0] S_IDLE     = 3'd0,
                     S_ACCEPT   = 3'd1,
                     S_COMPRESS = 3'd2,
                     S_WAIT     = 3'd3,
                     S_PAD80    = 3'd4,
                     S_PADZERO  = 3'd5,
                     S_PADLEN   = 3'd6,
                     S_DONE     = 3'd7;

    reg [2:0]   state, ret;
    reg [511:0] blk;        // block under assembly, first byte in the MSBs
    reg [6:0]   bidx;       // bytes currently in `blk` (0..63)
    reg [63:0]  total;      // message length in bytes (padding excluded)
    reg [2:0]   lenidx;     // which of the 8 length bytes is being emitted
    reg         core_init, core_start;

    wire        core_busy;

    assign in_ready = (state == S_ACCEPT);

    // Message bit length, emitted big-endian.
    wire [63:0] bitlen  = {total[60:0], 3'b000};
    wire [7:0]  lenbyte = bitlen[63 - 8*lenidx -: 8];

    // Byte source for the block assembler.
    reg       pb_v;
    reg [7:0] pb_b;
    always @(*) begin
        pb_v = 1'b0;
        pb_b = 8'h00;
        case (state)
            S_ACCEPT:  begin pb_v = in_valid; pb_b = in_byte; end
            S_PAD80:   begin pb_v = 1'b1;     pb_b = 8'h80;   end
            S_PADZERO: begin pb_v = (bidx != 7'd56); pb_b = 8'h00; end
            S_PADLEN:  begin pb_v = 1'b1;     pb_b = lenbyte; end
            default:   begin pb_v = 1'b0;     pb_b = 8'h00;   end
        endcase
    end

    wire blk_full = pb_v && (bidx == 7'd63);

    sha256_core u_core (
        .clk    (clk),
        .rst_n  (rst_n),
        .init   (core_init),
        .start  (core_start),
        .block  (blk),
        .busy   (core_busy),
        .digest (digest)
    );

    always @(posedge clk) begin
        if (!rst_n) begin
            state      <= S_IDLE;
            ret        <= S_IDLE;
            bidx       <= 7'd0;
            total      <= 64'd0;
            lenidx     <= 3'd0;
            done       <= 1'b0;
            core_init  <= 1'b0;
            core_start <= 1'b0;
            blk        <= 512'd0;
        end else begin
            core_init  <= 1'b0;
            core_start <= 1'b0;

            // --- Block assembler ---
            if (pb_v) begin
                blk  <= {blk[503:0], pb_b};
                bidx <= blk_full ? 7'd0 : (bidx + 7'd1);
            end
            if (state == S_ACCEPT && pb_v)
                total <= total + 64'd1;

            // --- Control ---
            //
            // `init` means "abandon whatever you were doing and start a new message",
            // so it is handled ahead of the state machine and honoured from ANY state.
            // Checking it only in S_IDLE/S_DONE made it a no-op mid-message, which let
            // a previous partial message leak into the next digest -- caught by the
            // card's FIFO-overrun test polluting the following empty-string vector.
            if (init) begin
                core_init <= 1'b1;
                bidx      <= 7'd0;
                total     <= 64'd0;
                lenidx    <= 3'd0;
                done      <= 1'b0;
                state     <= S_ACCEPT;
            end else
            case (state)
                S_IDLE: begin
                    // Waiting for `init`, handled above.
                end

                S_ACCEPT: begin
                    if (blk_full) begin
                        ret   <= S_ACCEPT;
                        state <= S_COMPRESS;
                    end else if (finish && !in_valid) begin
                        state <= S_PAD80;
                    end
                end

                // Kick the core, then wait for it to raise busy.
                S_COMPRESS: begin
                    if (core_busy) begin
                        state <= S_WAIT;
                    end else begin
                        core_start <= 1'b1;
                    end
                end

                S_WAIT: begin
                    if (!core_busy)
                        state <= ret;
                end

                S_PAD80: begin
                    if (blk_full) begin
                        ret   <= S_PADZERO;
                        state <= S_COMPRESS;
                    end else begin
                        state <= S_PADZERO;
                    end
                end

                // Zero-fill until exactly 56 bytes sit in the block, so the 8 length
                // bytes complete it. Spills into a second block when 0x80 landed late.
                S_PADZERO: begin
                    if (bidx == 7'd56) begin
                        state <= S_PADLEN;
                    end else if (blk_full) begin
                        ret   <= S_PADZERO;
                        state <= S_COMPRESS;
                    end
                end

                S_PADLEN: begin
                    if (lenidx == 3'd7) begin
                        ret   <= S_DONE;
                        state <= S_COMPRESS;
                    end else begin
                        lenidx <= lenidx + 3'd1;
                    end
                end

                S_DONE: begin
                    // Hold `done` and the digest until `init` starts a new message.
                    done <= 1'b1;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
