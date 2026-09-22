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

    // --- $C800-$CFFF expansion window (wired but inert; see take_c800) ---
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire [10:0]  c8_addr,
    /* verilator lint_on UNUSEDSIGNAL */
    output wire [7:0]   c8_rdata,
    output wire         take_c800,

    // --- Heap memory port (see docs/HEAP.md for why this is external) ---
    // BRAM-like: drive addr/we/wdata on a clock edge, rdata is valid the next cycle.
    // An SDRAM controller and the tree-PLRU cache slot in behind this later without
    // the card's logic changing.
    output reg  [31:0]  mem_addr,
    output reg          mem_we,
    output reg  [7:0]   mem_wdata,
    input  wire [7:0]   mem_rdata,

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
    localparam [7:0] V_NOP         = 8'h00,
                     V_RESET       = 8'h01,
                     V_HASH_INIT   = 8'h10,
                     V_HASH_STREAM = 8'h11,
                     V_HASH_FINAL  = 8'h12,
                     V_STOREPATH   = 8'h14,
                     V_HASH_PUSH   = 8'h15,
                     V_ALLOC       = 8'h20,
                     V_FOLLOW      = 8'h21;

    // ---------------------------------------------------------------- errors -------
    // 4 bits: STATUS b4 is READY, so the error code occupies b3..b0.
    // E_NOTFOUND is declared here to keep the numbering aligned with CARD-ISA.md;
    // it is first raised by LOOKUP, which lands with keycam.v.
    /* verilator lint_off UNUSEDPARAM */
    localparam [3:0] E_NOTFOUND = 4'h2;
    /* verilator lint_on UNUSEDPARAM */

    localparam [3:0] E_NONE     = 4'h0,
                     E_BADVERB  = 4'h1,
                     E_OOM      = 4'h3,
                     E_BADADDR  = 4'h4,
                     E_BUSYERR  = 4'h5,
                     E_OVERRUN  = 4'h7;   // DATA written with the input FIFO full

    localparam [7:0] CARD_ID = 8'hA2;

    // --- Heap geometry ---
    // Address 0 is never allocated, so a zero pointer is unambiguously null
    // (docs/HEAP.md). HEAP_TOP bounds allocation; a real board sizes it to its SDRAM.
    localparam [31:0] HEAP_BASE = 32'h0000_0010;
    localparam [31:0] HEAP_TOP  = 32'h0010_0000;   // 1 MB in the harnesses

    localparam [7:0] TAG_INDIRECT = 8'h0A;
    localparam integer MAX_FOLLOW = 64;  // chain-length guard; see FOLLOW below

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
    reg        finalizing;  // HASH_FINAL requested; waiting for the FIFO to drain

    wire          h_ready;
    wire          h_done;
    wire [255:0]  h_digest;
    wire [255:0]  sp_path;

    // ---------------------------------------------------------------- input FIFO ---
    // Without this the CPU must poll READY before EVERY byte, measured at 13.7 bus
    // accesses per byte -- which reduced a nominally ~300x accelerator to ~23x.
    //
    // Depth is set by compression latency, not block size. The slot bus delivers a
    // byte roughly every 10 fast cycles; a full-block compression stalls the hasher
    // for ~65, so the CPU can get at most ~7 bytes ahead. 32 entries is generous, and
    // exposing READY as "at least 16 free" lets the CPU write 16 bytes per poll:
    // 17 accesses per 16 bytes, or ~1.06 per byte.
    localparam integer FIFO_DEPTH = 32;
    localparam integer FIFO_BURST = 16;   // bytes the CPU may write after one READY

    reg  [7:0] fifo [0:FIFO_DEPTH-1];
    reg  [5:0] wptr, rptr;                // one extra bit so full != empty

    wire [5:0] fifo_count = wptr - rptr;
    wire       fifo_empty = (wptr == rptr);
    wire       fifo_full  = (fifo_count == FIFO_DEPTH[5:0]);
    wire [5:0] fifo_free  = FIFO_DEPTH[5:0] - fifo_count;
    wire       fifo_ready = (fifo_free >= FIFO_BURST[5:0]);

    // Hand a byte to the hasher only when nothing is already in flight. Issuing on
    // h_ready alone repeats the block-boundary loss the CPU-side poll had: in_ready is
    // still high during the cycle the hasher consumes the byte that fills a block, so
    // a second byte issued then would arrive after the state had moved to compressing
    // and be dropped. One byte per two cycles is ample -- the bus is 10x slower.
    wire fifo_drain = !fifo_empty && h_ready && !h_valid;

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

    // ---------------------------------------------------------------- mem engine ---
    // One engine owns the memory port, so every verb that touches the heap shares a
    // single read microsequence (issue, then latch) rather than each rolling its own.
    // A read costs 2 cycles; at 48 card cycles per bus access that is invisible.
    localparam [3:0] M_IDLE       = 4'd0,
                     M_RD_LATCH   = 4'd1,   // mem_addr presented; sample mem_rdata
                     M_PF_DONE    = 4'd2,
                     M_STREAM     = 4'd3,
                     M_STREAM_PUT = 4'd4,
                     M_ALLOC_HDR  = 4'd5,
                     M_FOLLOW     = 4'd6,
                     M_FOLLOW_TAG = 4'd7,
                     M_FOLLOW_PTR = 4'd8,
                     M_FOLLOW_PB  = 4'd9;

    reg [3:0]  mstate;
    reg [3:0]  mret;          // state to resume after a generic read latches
    reg [31:0] mcur;          // working address
    reg [7:0]  mbyte;         // byte most recently read
    reg [2:0]  mbidx;         // byte index within a multi-byte field
    reg [31:0] mptr;          // pointer being assembled
    reg [31:0] mcount;        // bytes left to stream
    reg [7:0]  hops;          // FOLLOW chain length so far
    reg [31:0] alloc_ptr;     // bump allocator

    // DATA port: a pending write, and a prefetched byte so reads answer immediately.
    reg        dw_pend;
    reg [31:0] dw_addr;
    reg [7:0]  dw_data;
    reg        pf_req;
    reg [7:0]  data_latch;    // byte at areg[asel], kept prefetched

    // ---------------------------------------------------------------- read path ----
    // b4 READY now means "the input FIFO has at least FIFO_BURST slots free", so one
    // poll licenses a burst of FIFO_BURST DATA writes. It deliberately does NOT track
    // the hasher's own readiness -- the FIFO absorbs compression stalls, which is the
    // entire point (see the input FIFO note above).
    //
    // Overrunning the FIFO raises ERR/OVERRUN rather than dropping bytes: a silently
    // short message yields a plausible-looking but wrong digest, the worst failure
    // mode available to a hash accelerator.
    wire [7:0] status = {busy, done, err, fifo_ready, errcode};

    reg [7:0] rdata;
    always @(*) begin
        case (c0n_addr)
            R_CMD:   rdata = status;
            R_CTRL:  rdata = CARD_ID;
            R_APORT: rdata = areg[asel][8*aidx +: 8];
            // DATA and TAG return the same prefetched byte -- the one at the selected
            // address register. The difference is the side effect: DATA advances the
            // register, TAG does not. So TAG is "what type is the object A_n points
            // at?" answered without disturbing the pointer, which is exactly what
            // type dispatch wants.
            R_DATA:  rdata = data_latch;
            R_TAG:   rdata = data_latch;
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
            push_mode <= 1'b0; finalizing <= 1'b0;
            wptr <= 6'd0; rptr <= 6'd0;
            h_init <= 1'b0; h_valid <= 1'b0; h_byte <= 8'h00; h_finish <= 1'b0;
            for (i = 0; i < 4; i = i + 1) areg[i]  <= 32'd0;
            for (i = 0; i < 8; i = i + 1) pslot[i] <= 256'd0;
            mstate <= M_IDLE; mret <= M_IDLE;
            mcur <= 32'd0; mbyte <= 8'h00; mbidx <= 3'd0;
            mptr <= 32'd0; mcount <= 32'd0; hops <= 8'd0;
            alloc_ptr <= HEAP_BASE;
            dw_pend <= 1'b0; dw_addr <= 32'd0; dw_data <= 8'h00;
            pf_req <= 1'b0; data_latch <= 8'h00;
            mem_addr <= 32'd0; mem_we <= 1'b0; mem_wdata <= 8'h00;
        end else begin
            // Single-cycle strobes.
            h_init  <= 1'b0;
            h_valid <= 1'b0;
            mem_we  <= 1'b0;          // writes are one cycle; default the port to read

            // ------------------------------------------------ memory engine --------
            case (mstate)
                M_IDLE: begin
                    // A pending DATA write goes first, so a write followed by a read
                    // of the same address cannot observe stale data.
                    if (dw_pend) begin
                        mem_addr  <= dw_addr;
                        mem_wdata <= dw_data;
                        mem_we    <= 1'b1;
                        dw_pend   <= 1'b0;
                    end else if (pf_req) begin
                        mem_addr <= areg[asel];
                        mret     <= M_PF_DONE;
                        mstate   <= M_RD_LATCH;
                        pf_req   <= 1'b0;
                    end
                end

                // mem_addr was presented last cycle, so mem_rdata is valid now.
                M_RD_LATCH: begin
                    mbyte  <= mem_rdata;
                    mstate <= mret;
                end

                M_PF_DONE: begin
                    data_latch <= mbyte;
                    mstate     <= M_IDLE;
                end

                // --- HASH_STREAM: hash card-resident bytes; none cross the bus ---
                M_STREAM: begin
                    if (mcount == 32'd0) begin
                        busy   <= 1'b0;
                        done   <= 1'b1;
                        mstate <= M_IDLE;
                        pf_req <= 1'b1;
                    end else if (!fifo_full) begin
                        mem_addr <= mcur;
                        mret     <= M_STREAM_PUT;
                        mstate   <= M_RD_LATCH;
                    end
                end

                M_STREAM_PUT: begin
                    fifo[wptr[4:0]] <= mbyte;
                    wptr            <= wptr + 6'd1;
                    mcur            <= mcur + 32'd1;
                    mcount          <= mcount - 32'd1;
                    mstate          <= M_STREAM;
                end

                // --- ALLOC: write the 4 header bytes taken from P0 ---
                M_ALLOC_HDR: begin
                    mem_addr  <= mcur + {29'd0, mbidx};
                    mem_wdata <= pslot[0][255 - 8*mbidx -: 8];
                    mem_we    <= 1'b1;
                    if (mbidx == 3'd3) begin
                        busy   <= 1'b0;
                        done   <= 1'b1;
                        mstate <= M_IDLE;
                        pf_req <= 1'b1;
                    end else begin
                        mbidx <= mbidx + 3'd1;
                    end
                end

                // --- FOLLOW: chase an INDIRECT chain to the first real object ---
                M_FOLLOW: begin
                    mem_addr <= mcur;
                    mret     <= M_FOLLOW_TAG;
                    mstate   <= M_RD_LATCH;
                end

                M_FOLLOW_TAG: begin
                    if (mbyte != TAG_INDIRECT) begin
                        areg[1] <= mcur;      // the answer
                        busy    <= 1'b0;
                        done    <= 1'b1;
                        mstate  <= M_IDLE;
                        pf_req  <= 1'b1;
                    end else if (hops >= MAX_FOLLOW[7:0]) begin
                        // A cycle of indirections would hang the card forever, so cap
                        // it and report rather than spin.
                        busy    <= 1'b0;
                        err     <= 1'b1;
                        errcode <= E_BADADDR;
                        mstate  <= M_IDLE;
                    end else begin
                        mbidx  <= 3'd0;
                        mptr   <= 32'd0;
                        hops   <= hops + 8'd1;
                        mstate <= M_FOLLOW_PTR;
                    end
                end

                M_FOLLOW_PTR: begin
                    if (mbidx == 3'd4) begin
                        mcur   <= mptr;
                        mstate <= M_FOLLOW;
                    end else begin
                        mem_addr <= mcur + 32'd4 + {29'd0, mbidx};
                        mret     <= M_FOLLOW_PB;
                        mstate   <= M_RD_LATCH;
                    end
                end

                M_FOLLOW_PB: begin
                    mptr[8*mbidx +: 8] <= mbyte;   // pointers are little-endian
                    mbidx              <= mbidx + 3'd1;
                    mstate             <= M_FOLLOW_PTR;
                end

                default: mstate <= M_IDLE;
            endcase

            // --- Drain the FIFO into the hasher ---
            if (fifo_drain) begin
                h_valid <= 1'b1;
                h_byte  <= fifo[rptr[4:0]];
                rptr    <= rptr + 6'd1;
            end

            // --- A verb in flight ---
            if (busy) begin
                if (finalizing) begin
                    // Every queued byte must reach the hasher before padding starts,
                    // or the message is silently truncated.
                    if (fifo_empty && !h_valid && !fifo_drain) begin
                        h_finish   <= 1'b1;
                        finalizing <= 1'b0;
                    end
                // `h_finish` is the gate, and it is essential. sha256 parks in its
                // done state holding h_done asserted until the next init, so testing
                // h_done alone let ANY busy verb be hijacked the cycle after it
                // started: ALLOC would have its header clobbered by the previous
                // message's digest and report done early. Only a HASH_FINAL in
                // flight asserts h_finish.
                end else if (h_finish && h_done) begin
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
                                    wptr      <= 6'd0;   // discard anything queued
                                    rptr      <= 6'd0;
                                end
                                V_HASH_PUSH: push_mode <= 1'b1;

                                // The high-value case: bytes already on the card are
                                // hashed without crossing the bus at all.
                                V_HASH_STREAM: begin
                                    push_mode <= 1'b0;
                                    mcur      <= areg[0];
                                    mcount    <= len;
                                    busy      <= 1'b1;
                                    mstate    <= M_STREAM;
                                end

                                // P0's first 4 bytes ARE the object header
                                // (docs/HEAP.md), so the caller writes tag+aux once
                                // and ALLOC reserves LEN payload bytes after it.
                                V_ALLOC: begin
                                    // Round the payload up to 4 so pointer columns
                                    // stay aligned.
                                    if ((alloc_ptr + 32'd4 + ((len + 32'd3) & ~32'd3))
                                        > HEAP_TOP) begin
                                        err     <= 1'b1;
                                        errcode <= E_OOM;
                                    end else begin
                                        areg[1]   <= alloc_ptr;
                                        mcur      <= alloc_ptr;
                                        alloc_ptr <= alloc_ptr + 32'd4
                                                     + ((len + 32'd3) & ~32'd3);
                                        mbidx     <= 3'd0;
                                        busy      <= 1'b1;
                                        mstate    <= M_ALLOC_HDR;
                                    end
                                end

                                V_FOLLOW: begin
                                    mcur   <= areg[0];
                                    hops   <= 8'd0;
                                    busy   <= 1'b1;
                                    mstate <= M_FOLLOW;
                                end
                                V_HASH_FINAL: begin
                                    // Do NOT assert finish yet: queued bytes still
                                    // have to reach the hasher first.
                                    push_mode  <= 1'b0;
                                    finalizing <= 1'b1;
                                    busy       <= 1'b1;
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
                            push_mode  <= 1'b0;
                            finalizing <= 1'b0;
                            busy       <= 1'b0;
                            done       <= 1'b0;
                            err        <= 1'b0;
                            errcode    <= E_NONE;
                            irq_pend   <= 1'b0;
                            wptr       <= 6'd0;
                            rptr       <= 6'd0;
                            h_init     <= 1'b1;
                        end
                        irq_en <= c0n_wdata[1];
                        // b3 RESET_PORTS: rewind every multi-byte port index.
                        //
                        // APORT and PPORT can be rewound via ASEL/PSEL, but LEN had no
                        // equivalent -- so a caller that read a partial LEN
                        // desynchronised its index PERMANENTLY, with every later
                        // 4-byte write landing rotated. That is silent: the card
                        // happily streams the wrong length. This gives LEN a way back.
                        if (c0n_wdata[3]) begin
                            aidx   <= 2'd0;
                            pidx   <= 5'd0;
                            lenidx <= 2'd0;
                        end
                    end

                    // Selecting a register or slot resets its byte index, so the
                    // 65816 never has to track where it is mid-transfer.
                    // Selecting a register also re-prefetches, so DATA/TAG reflect
                    // the newly selected pointer immediately.
                    R_ASEL: begin
                        asel   <= c0n_wdata[1:0];
                        aidx   <= 2'd0;
                        pf_req <= 1'b1;
                    end
                    R_PSEL: begin psel <= c0n_wdata[2:0]; pidx <= 5'd0; end

                    R_APORT: begin
                        areg[asel][8*aidx +: 8] <= c0n_wdata;
                        aidx <= aidx + 2'd1;           // wraps at 4 bytes
                        // Re-prefetch on the last byte, when the pointer is complete.
                        if (aidx == 2'd3) pf_req <= 1'b1;
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
                        if (push_mode) begin
                            // Queued for the hasher.
                            if (fifo_full) begin
                                // Loud, not silent. A dropped byte would produce a
                                // wrong-but-plausible digest.
                                err     <= 1'b1;
                                errcode <= E_OVERRUN;
                            end else begin
                                fifo[wptr[4:0]] <= c0n_wdata;
                                wptr            <= wptr + 6'd1;
                            end
                        end else begin
                            // Store to heap memory at the selected pointer, then
                            // advance it and re-prefetch.
                            dw_pend      <= 1'b1;
                            dw_addr      <= areg[asel];
                            dw_data      <= c0n_wdata;
                            areg[asel]   <= areg[asel] + 32'd1;
                            pf_req       <= 1'b1;
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
                    // Reading DATA advances the pointer and prefetches the next byte,
                    // which is what makes sequential heap access cheap. Reading TAG
                    // deliberately does not.
                    R_DATA: begin
                        areg[asel] <= areg[asel] + 32'd1;
                        pf_req     <= 1'b1;
                    end
                    default: ;
                endcase
            end
        end
    end

endmodule

`default_nettype wire
