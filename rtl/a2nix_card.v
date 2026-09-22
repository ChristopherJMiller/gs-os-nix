// a2nix accelerator card: Apple II slot interface and verb dispatch.
//
// Implements the register map in docs/CARD-ISA.md, which is NORMATIVE -- change that
// document and this module together, and tb_card.py/card_main.cpp will hold you to it.
//
// Bus model
// ---------
// MAME's read_c0nx() must return a byte synchronously, so reads here are COMBINATIONAL
// on the current state, while their side effects (index auto-increment) land on the
// clock edge with `c0n_read` asserted. The MAME wrapper therefore does:
//
//     set c0n_addr + c0n_read -> eval() -> sample c0n_rdata -> tick() to commit
//
// which keeps auto-incrementing read ports working without MAME needing to know the
// card is a state machine.
//
// Scope at L2: registers, the hashing verbs, and STOREPATH. Heap and evaluation verbs
// return ERR/BADVERB until L3 gives the card memory of its own.
`default_nettype none

module a2nix_card (
    input  wire         clk,
    input  wire         rst_n,

    // --- $C0nX device select (16 bytes) ---
    input  wire [3:0]   c0n_addr,
    input  wire         c0n_read,    // strobe: commit read side effects on this edge
    input  wire         c0n_write,   // strobe: commit the write on this edge
    input  wire [7:0]   c0n_wdata,
    output wire [7:0]   c0n_rdata,   // combinational

    // --- $C800-$CFFF expansion window (L3; wired but inert) ---
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire [10:0]  c8_addr,       // L3: indexes the CAS window
    /* verilator lint_on UNUSEDSIGNAL */
    output wire [7:0]   c8_rdata,
    output wire         take_c800,

    output wire         irq
);

    // ---------------------------------------------------------------- registers ----
    localparam [3:0] R_CMD   = 4'h0,  // W: verb   R: status
                     R_CTRL  = 4'h1,  // W: control  R: id
                     R_ASEL  = 4'h2,
                     R_APORT = 4'h3,
                     R_DATA  = 4'h4,
                     R_TAG   = 4'h5,
                     R_PSEL  = 4'h6,
                     R_PPORT = 4'h7,
                     R_LEN   = 4'h8,
                     R_IRQ   = 4'h9;

    // ---------------------------------------------------------------- verbs --------
    localparam [7:0] V_NOP        = 8'h00,
                     V_RESET      = 8'h01,
                     V_HASH_INIT  = 8'h10,
                     V_HASH_FINAL = 8'h12,
                     V_STOREPATH  = 8'h14,
                     V_HASH_PUSH  = 8'h15;

    // ---------------------------------------------------------------- errors -------
    // 4 bits: STATUS b4 is READY, so the error code occupies b3..b0.
    localparam [3:0] E_NONE     = 4'h0,
                     E_BADVERB  = 4'h1,
                     E_BUSYERR  = 4'h5;

    localparam [7:0] CARD_ID = 8'hA2;

    reg [31:0] areg [0:3];
    reg [1:0]  asel;
    reg [1:0]  aidx;        // byte index within the selected address register

    reg [255:0] pslot [0:7];
    reg [2:0]   psel;
    reg [4:0]   pidx;       // byte index within the selected parameter slot

    reg [31:0] len;
    reg [1:0]  lenidx;

    reg        busy, done, err;
    reg [3:0]  errcode;
    reg        irq_en, irq_pend;
    reg        push_mode;   // HASH_PUSH armed: DATA writes feed the hash

    assign irq       = irq_pend & irq_en;
    assign take_c800 = 1'b0;      // L3
    assign c8_rdata  = 8'h00;     // L3

    // ---------------------------------------------------------------- hasher -------
    reg        h_init, h_valid;
    reg [7:0]  h_byte;
    reg        h_finish;

    wire          h_ready;
    wire          h_done;
    wire [255:0]  h_digest;
    wire [255:0]  sp_path;

    sha256 u_hash (
        .clk      (clk),
        .rst_n    (rst_n),
        .init     (h_init),
        .in_valid (h_valid),
        .in_byte  (h_byte),
        .in_ready (h_ready),
        .finish   (h_finish),
        .done     (h_done),
        .digest   (h_digest)
    );

    // While a verb is in flight, fold the hasher's live digest -- pslot[0] has not
    // been written yet at the moment HASH_FINAL completes, so folding it would encode
    // the PREVIOUS message. Idle, fold pslot[0] so a caller can supply a digest it
    // computed some other way ($14 STOREPATH).
    wire [255:0] sp_digest = busy ? h_digest : pslot[0];
    // The 160-bit folded hash is not exposed: callers want the encoded path, and the
    // fold is recoverable from it. Kept connected so the port list stays honest.
    /* verilator lint_off UNUSEDSIGNAL */
    wire [159:0] sp_folded_nc;
    /* verilator lint_on UNUSEDSIGNAL */

    // Combinational: once the digest settles the encoded path is free (DECISIONS D8).
    storepath u_sp (
        .digest (sp_digest),
        .folded (sp_folded_nc),
        .path   (sp_path)
    );

    // ---------------------------------------------------------------- read path ----
    // b4 READY is load-bearing: the hasher stops accepting for ~65 cycles while it
    // compresses a full block, and the bus has no back-pressure of its own, so the
    // 65816 MUST poll this before each DATA write in push mode or bytes are dropped.
    //
    // `& ~h_valid` is essential, not defensive. A DATA write latches the byte and the
    // hasher consumes it on the NEXT edge, so for one cycle a byte is in flight while
    // the hasher still reports in_ready. Without masking that window, the CPU sees
    // READY, writes the next byte, and the in-flight one is overwritten -- silently,
    // and only on the 64-byte boundary where the block fills. Which is exactly the
    // bug that showed up as "8 of 12 NIST vectors pass".
    wire [7:0] status = {busy, done, err, h_ready & ~h_valid, errcode};

    reg [7:0] rdata;
    always @(*) begin
        case (c0n_addr)
            R_CMD:   rdata = status;
            R_CTRL:  rdata = CARD_ID;
            R_APORT: rdata = areg[asel][8*aidx +: 8];
            R_DATA:  rdata = 8'h00;                      // L3: card memory
            R_TAG:   rdata = 8'h00;                      // L3: tagged heap
            // P slots are BYTE ARRAYS: index 0 is the first byte, so digests and path
            // strings read out in their natural order. Contrast A registers and LEN,
            // which are little-endian integers matching 65816 pointer layout.
            R_PPORT: rdata = pslot[psel][255 - 8*pidx -: 8];
            R_LEN:   rdata = len[8*lenidx +: 8];
            R_IRQ:   rdata = {7'b0, irq_pend};
            default: rdata = 8'h00;                      // reserved reads as $00
        endcase
    end
    assign c0n_rdata = rdata;

    // ---------------------------------------------------------------- sequencing ---
    integer i;
    always @(posedge clk) begin
        if (!rst_n) begin
            asel <= 2'd0; aidx <= 2'd0;
            psel <= 3'd0; pidx <= 5'd0;
            len  <= 32'd0; lenidx <= 2'd0;
            busy <= 1'b0; done <= 1'b0; err <= 1'b0; errcode <= E_NONE;
            irq_en <= 1'b0; irq_pend <= 1'b0;
            push_mode <= 1'b0;
            h_init <= 1'b0; h_valid <= 1'b0; h_byte <= 8'h00; h_finish <= 1'b0;
            for (i = 0; i < 4; i = i + 1) areg[i]  <= 32'd0;
            for (i = 0; i < 8; i = i + 1) pslot[i] <= 256'd0;
        end else begin
            // Single-cycle strobes.
            h_init  <= 1'b0;
            h_valid <= 1'b0;

            // --- A verb in flight: wait for the hasher, then publish results ---
            if (busy) begin
                if (h_done) begin
                    h_finish     <= 1'b0;
                    pslot[0]     <= h_digest;
                    pslot[3]     <= sp_path;
                    busy         <= 1'b0;
                    done         <= 1'b1;
                    irq_pend     <= 1'b1;
                end
            end

            // --- Writes ---
            if (c0n_write) begin
                case (c0n_addr)
                    R_CMD: begin
                        if (busy) begin
                            err     <= 1'b1;
                            errcode <= E_BUSYERR;
                        end else begin
                            done    <= 1'b0;
                            err     <= 1'b0;
                            errcode <= E_NONE;
                            case (c0n_wdata)
                                V_NOP: ;
                                V_RESET: begin
                                    push_mode <= 1'b0;
                                    h_init    <= 1'b1;
                                    aidx      <= 2'd0;
                                    pidx      <= 5'd0;
                                    lenidx    <= 2'd0;
                                    irq_pend  <= 1'b0;
                                end
                                V_HASH_INIT: begin
                                    h_init    <= 1'b1;
                                    push_mode <= 1'b0;
                                end
                                V_HASH_PUSH: push_mode <= 1'b1;
                                V_HASH_FINAL: begin
                                    push_mode <= 1'b0;
                                    h_finish  <= 1'b1;
                                    busy      <= 1'b1;
                                end
                                V_STOREPATH: begin
                                    // Purely combinational: answer immediately.
                                    pslot[3] <= sp_path;
                                    done     <= 1'b1;
                                end
                                default: begin
                                    err     <= 1'b1;
                                    errcode <= E_BADVERB;
                                end
                            endcase
                        end
                    end

                    R_CTRL: begin
                        if (c0n_wdata[0]) begin        // RESET
                            push_mode <= 1'b0;
                            busy      <= 1'b0;
                            done      <= 1'b0;
                            err       <= 1'b0;
                            errcode   <= E_NONE;
                            irq_pend  <= 1'b0;
                            h_init    <= 1'b1;
                        end
                        irq_en <= c0n_wdata[1];
                    end

                    // Selecting a register or slot resets its byte index, so the
                    // 65816 never has to track where it is mid-transfer.
                    R_ASEL: begin asel <= c0n_wdata[1:0]; aidx <= 2'd0; end
                    R_PSEL: begin psel <= c0n_wdata[2:0]; pidx <= 5'd0; end

                    R_APORT: begin
                        areg[asel][8*aidx +: 8] <= c0n_wdata;
                        aidx <= aidx + 2'd1;           // wraps at 4 bytes
                    end

                    R_PPORT: begin
                        pslot[psel][255 - 8*pidx -: 8] <= c0n_wdata;
                        pidx <= pidx + 5'd1;           // wraps at 32 bytes
                    end

                    R_LEN: begin
                        len[8*lenidx +: 8] <= c0n_wdata;
                        lenidx <= lenidx + 2'd1;
                    end

                    R_DATA: begin
                        // In push mode the byte goes straight into the hash; otherwise
                        // it would address card memory, which does not exist until L3.
                        if (push_mode && h_ready) begin
                            h_valid <= 1'b1;
                            h_byte  <= c0n_wdata;
                        end
                    end

                    R_IRQ: irq_pend <= 1'b0;           // acknowledge

                    default: ;
                endcase
            end

            // --- Read side effects (committed on the strobe edge) ---
            if (c0n_read) begin
                case (c0n_addr)
                    R_APORT: aidx   <= aidx + 2'd1;
                    R_PPORT: pidx   <= pidx + 5'd1;
                    R_LEN:   lenidx <= lenidx + 2'd1;
                    default: ;
                endcase
            end
        end
    end

endmodule

`default_nettype wire
