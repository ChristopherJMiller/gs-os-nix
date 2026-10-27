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
    // 32 bits wide, byte-addressed with byte strobes -- the shape real SDRAM has, and
    // the shape that makes the CAM worth its gates: filling a lane is one access
    // instead of four. Measured at byte width, LOOKUP was only 1.5x cheaper than a
    // 65816 binary search because loading the key column dominated.
    //
    // mem_addr is a BYTE address; the memory returns the aligned 32-bit word
    // CONTAINING it, so the low two bits select a lane rather than changing the word.
    // rdata is valid the cycle after addr is presented; a write lands on the edge
    // where mem_we is high, under mem_wstrb.
    output reg  [31:0]  mem_addr,
    output reg          mem_we,
    output reg  [3:0]   mem_wstrb,   // per-byte write enables
    output reg  [31:0]  mem_wdata,
    input  wire [31:0]  mem_rdata,

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
                     V_FOLLOW      = 8'h21,
                     V_RESERVE     = 8'h25,
                     V_MEMCMP      = 8'h22,
                     V_LOOKUP      = 8'h30,
                     V_MARK        = 8'h40,
                     V_COMPACT     = 8'h41,
                     V_ALU64       = 8'h50;

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
                     E_BADPARAM = 4'h6,   // ALU64 divide by zero, or an unknown op
                     E_OVERRUN  = 4'h7;   // DATA written with the input FIFO full

    localparam [7:0] CARD_ID = 8'hA2;

    // --- Heap geometry ---
    // 1 MB in the harnesses; a real board sizes it to its SDRAM. The heap is two
    // semispaces for the Cheney collector (docs/DECISIONS.md D15). Each starts 16
    // bytes in so that neither can hand out address 0, the null pointer. Which half
    // is being allocated from is `space_sel`.
    localparam [31:0] SPACE_SIZE = 32'h0008_0000;              // 512 KB a side
    localparam [31:0] SPACE_A    = 32'h0000_0010;
    localparam [31:0] SPACE_B    = 32'h0008_0010;

    // Tags the collector needs to size or trace. BOOL, NULL and BLACKHOLE take the
    // 8-byte default and carry no pointers, so they need no case of their own.
    localparam [7:0] TAG_INT       = 8'h01,
                     TAG_STRING    = 8'h04,
                     TAG_PATH      = 8'h05,
                     TAG_LIST      = 8'h06,
                     TAG_ATTRS     = 8'h07,
                     TAG_THUNK     = 8'h08,
                     TAG_LAMBDA    = 8'h09,
                     TAG_INDIRECT  = 8'h0A,
                     TAG_FORWARDED = 8'h0C,
                     TAG_FLOAT     = 8'h0D;
    localparam integer MAX_FOLLOW = 64;  // chain-length guard; see FOLLOW below

    function [31:0] round4(input [31:0] v);
        round4 = (v + 32'd3) & ~32'd3;
    endfunction

    // Object size in bytes from its header word (docs/HEAP.md, "What the collector
    // traces"). Never less than 8: the forwarding pointer needs a word to live in.
    function [31:0] obj_size(input [31:0] hdr);
        reg [31:0] aux;
        reg [31:0] s;
        begin
            aux = {8'd0, hdr[31:8]};
            case (hdr[7:0])
                TAG_INT, TAG_FLOAT:     s = 32'd12;
                TAG_STRING:             s = 32'd4 + round4(aux) + 32'd4;
                TAG_PATH:               s = 32'd4 + round4(aux);
                TAG_LIST:               s = 32'd4 + (aux << 2);
                TAG_ATTRS:              s = 32'd4 + (aux << 3) + (aux << 2) + round4(aux << 1);   // 12*aux, then a 2-byte position each
                TAG_THUNK, TAG_LAMBDA:  s = 32'd12;
                default:                s = 32'd8;    // BOOL, NULL, BLACKHOLE, INDIRECT...
            endcase
            obj_size = (s < 32'd8) ? 32'd8 : s;
        end
    endfunction

    // Bytes an ALLOC of `payload` bytes actually consumes: header + rounded payload,
    // never less than 8.
    function [31:0] alloc_total(input [31:0] payload);
        reg [31:0] t;
        begin
            t = 32'd4 + round4(payload);
            alloc_total = (t < 32'd8) ? 32'd8 : t;
        end
    endfunction

    // Is payload word k of this object a pointer the collector must trace?
    // ATTRS is the trap: its first `aux` words are hashes that look like addresses.
    function is_ptr_field(input [31:0] hdr, input [31:0] k);
        reg [31:0] aux;
        begin
            aux = {8'd0, hdr[31:8]};
            case (hdr[7:0])
                TAG_STRING:             is_ptr_field = (k == (round4(aux) >> 2));
                TAG_LIST:               is_ptr_field = (k < aux);
                TAG_ATTRS:              is_ptr_field = (k >= aux) && (k < aux + (aux << 1));
                TAG_THUNK, TAG_LAMBDA:  is_ptr_field = (k < 32'd2);
                TAG_INDIRECT:           is_ptr_field = (k == 32'd0);
                default:                is_ptr_field = 1'b0;
            endcase
        end
    endfunction

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
    localparam [5:0] M_IDLE       = 6'd0,
                     M_RD_LATCH   = 6'd1,   // mem_addr presented; sample mem_rdata
                     M_PF_DONE    = 6'd2,
                     M_STREAM     = 6'd3,
                     M_STREAM_PUT = 6'd4,
                     M_ALLOC_HDR  = 6'd5,
                     M_FOLLOW     = 6'd6,
                     M_FOLLOW_TAG = 6'd7,
                     M_FOLLOW_PTR = 6'd8,
                     M_LK_CNT     = 6'd10,  // assemble the ATTRS attribute count
                     M_LK_CB      = 6'd11,
                     M_LK_BATCH   = 6'd12,  // start a batch of up to CAM_WIDTH lanes
                     M_LK_LOAD    = 6'd13,  // fill the CAM from the key-hash column
                     M_LK_HB      = 6'd14,
                     M_LK_CAM     = 6'd15,  // present the key; hit or next batch
                     M_LK_VAL     = 6'd16,  // read the matched value pointer
                     // --- Cheney collector (docs/DECISIONS.md D15) ---
                     M_GC_ROOT     = 6'd17, // next root from the root list
                     M_GC_ROOT_RD  = 6'd18,
                     M_GC_ROOT_WB  = 6'd19, // write the root's new address back
                     M_GC_EVAC     = 6'd20, // evacuate gc_obj -> gc_new
                     M_GC_EVAC_HDR = 6'd21,
                     M_GC_EVAC_FWD = 6'd22, // already copied: take the forwarding ptr
                     M_GC_COPY     = 6'd23, // copy one word per 3 cycles
                     M_GC_COPY_WR  = 6'd24,
                     M_GC_COPY_FWD = 6'd25, // leave a forwarding pointer behind
                     M_GC_SCAN     = 6'd26, // Cheney scan of to-space
                     M_GC_SCAN_HDR = 6'd27,
                     M_GC_SCAN_F   = 6'd28, // next payload word; trace if a pointer
                     M_GC_SCAN_FRD = 6'd29,
                     M_GC_SCAN_WB  = 6'd30,
                     // MEMCMP walks both operands a word at a time (docs/CARD-ISA.md)
                     M_MC_A        = 6'd31, // present the A-side word
                     M_MC_B        = 6'd32, // save it, present the B-side word
                     M_MC_CMP      = 6'd33; // compare, then advance or answer

    reg [5:0]  mstate;
    reg [5:0]  mret;          // state to resume after a generic read latches
    reg [31:0] mcur;          // working address
    reg [31:0] mword;         // word most recently read
    reg [7:0]  mbyte;         // the addressed byte of that word
    reg [31:0] mcount;        // bytes left to stream
    reg [7:0]  hops;          // FOLLOW chain length so far
    reg [31:0] mc_pa, mc_pb;  // MEMCMP: the two cursors
    reg [31:0] mc_rem;        // MEMCMP: bytes still to compare
    reg [31:0] mc_aw;         // MEMCMP: the A-side word, held while B is read

    // MEMCMP compares in ADDRESS order, so byte +0 is the most significant; the
    // last word is masked down to the bytes LEN actually covers.
    function [31:0] mc_mask(input [31:0] rem);
        case (rem)
            32'd1:   mc_mask = 32'hFF00_0000;
            32'd2:   mc_mask = 32'hFFFF_0000;
            32'd3:   mc_mask = 32'hFFFF_FF00;
            default: mc_mask = 32'hFFFF_FFFF;
        endcase
    endfunction
    wire [31:0] mc_cmp_a = { mc_aw[7:0], mc_aw[15:8], mc_aw[23:16], mc_aw[31:24] }
                           & mc_mask(mc_rem);
    wire [31:0] mc_cmp_b = { mword[7:0], mword[15:8], mword[23:16], mword[31:24] }
                           & mc_mask(mc_rem);
    reg [31:0] alloc_ptr;     // bump allocator

    // --- Collector state ---
    reg        space_sel;    // 0: allocate in A, B is to-space.  1: the reverse
    reg [31:0] gc_free;      // to-space bump pointer
    reg [31:0] gc_scan;      // Cheney scan pointer; everything below it is traced
    reg [31:0] gc_root_at;   // root list base
    reg [31:0] gc_root_n;    // root count
    reg [31:0] gc_root_i;    // root index
    reg [31:0] gc_obj;       // from-space object being evacuated
    reg [31:0] gc_new;       // its to-space address (the EVAC result)
    reg [31:0] gc_size;      // its size in bytes
    reg [31:0] gc_off;       // copy offset
    reg [31:0] gc_field;     // payload word index while scanning
    reg [5:0]  gc_ret;       // where EVAC returns to: the root path or the scan path
    // The object under SCAN has its own header/size. EVAC is nested inside the scan
    // loop and loads gc_hdr/gc_size for the object it copies; if the scan shared
    // those, returning from evacuating a list's first element would leave the loop
    // believing the LIST was an INT and skip every remaining element. It did.
    reg [31:0] sc_hdr;
    reg [31:0] sc_size;

    // to-space is simply the other half; COMPACT expresses the flip as "the old
    // from-space becomes the new to-space", so only from_base is needed here.
    wire [31:0] from_base = space_sel ? SPACE_B : SPACE_A;
    wire [31:0] from_top  = from_base + SPACE_SIZE - 32'd16;
    wire        obj_in_from = (gc_obj >= from_base) && (gc_obj < from_top);

    // DATA port: a pending write, and a prefetched byte so reads answer immediately.
    reg        dw_pend;
    reg [31:0] dw_addr;
    reg [7:0]  dw_data;
    reg        pf_req;
    reg [7:0]  data_latch;    // byte at areg[asel], kept prefetched

    // ---------------------------------------------------------------- LOOKUP -------
    // Attribute lookup, the hottest operation in Nix evaluation. The key-hash column
    // is loaded into a CAM_WIDTH-lane comparator array and matched in one cycle
    // (docs/DECISIONS.md D8); sets larger than one batch are matched in successive
    // passes, lowest lane first, which preserves sorted order.
    localparam integer CAM_WIDTH = 32;

    reg [32*CAM_WIDTH-1:0] cam_entries;
    reg [CAM_WIDTH-1:0]    cam_lane_valid;
    reg [31:0]             lk_base;    // the ATTRS object
    reg [23:0]             lk_count;   // total attributes
    reg [23:0]             lk_done;    // attributes examined in earlier batches
    reg [5:0]              lk_lane;    // lane being filled this batch

    // The key hash arrives in P0 as a byte array, byte 0 first, and the heap stores
    // the column little-endian -- so byte 0 is the LSB on both sides.
    wire [31:0] cam_key = { pslot[0][255 - 8*3 -: 8],
                            pslot[0][255 - 8*2 -: 8],
                            pslot[0][255 - 8*1 -: 8],
                            pslot[0][255 - 8*0 -: 8] };

    wire       cam_hit;
    wire [4:0] cam_index;

    keycam #(.WIDTH(CAM_WIDTH)) u_cam (
        .key        (cam_key),
        .entries    (cam_entries),
        .lane_valid (cam_lane_valid),
        .hit        (cam_hit),
        .index      (cam_index)
    );

    // Column base addresses, from the layout in docs/HEAP.md.
    wire [31:0] lk_keys_at   = lk_base + 32'd4;
    wire [31:0] lk_values_at = lk_base + 32'd4 + ({8'd0, lk_count} << 2);

    // Absolute attribute index of a hit: batches already examined, plus the lane.
    wire [23:0] lk_hit_index = lk_done + {19'd0, cam_index};

    // ---------------------------------------------------------------- ALU64 --------
    // P0 and P1 hold the operands as 8-byte little-endian integers in byte-array
    // slots (byte 0 at the top of the slot, and byte 0 is the LSB). P2 byte 0 selects
    // the operation. The result lands in P3 the same way.
    // Takes just the slot's first 8 bytes (its top 64 bits) and reverses them into
    // a little-endian integer.
    function [63:0] pslot_i64(input [63:0] top8);
        pslot_i64 = { top8[ 7: 0], top8[15: 8], top8[23:16], top8[31:24],
                      top8[39:32], top8[47:40], top8[55:48], top8[63:56] };
    endfunction

    reg         alu_start;
    reg         alu_verb;      // an ALU64 verb is in flight; gates completion
    wire        alu_done;
    wire        alu_err;
    wire [63:0] alu_result;
    // Completion is gated on the one-cycle done pulse, so the level-sensitive busy
    // output is not needed here.
    /* verilator lint_off UNUSEDSIGNAL */
    wire        alu_busy_nc;
    /* verilator lint_on UNUSEDSIGNAL */

    alu64 u_alu (
        .clk    (clk),
        .rst_n  (rst_n),
        .start  (alu_start),
        // Byte 0 of P2 is bits [255:248]; its LOW nibble is [251:248]. The first cut
        // used [255 -: 4] -- the HIGH nibble -- so every op decoded as $0 = ADD, and
        // CMP, x/0 and an unknown op all "succeeded" as additions. Six regression
        // failures from one wrong constant.
        .op     (pslot[2][251:248]),
        .a      (pslot_i64(pslot[0][255:192])),
        .b      (pslot_i64(pslot[1][255:192])),
        .busy   (alu_busy_nc),
        .done   (alu_done),
        .err    (alu_err),
        .result (alu_result)
    );

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
            mcur <= 32'd0; mword <= 32'd0; mbyte <= 8'h00;
            mcount <= 32'd0; hops <= 8'd0;
            alloc_ptr <= SPACE_A;
            space_sel <= 1'b0; gc_free <= SPACE_B; gc_scan <= SPACE_B;
            gc_root_at <= 32'd0; gc_root_n <= 32'd0; gc_root_i <= 32'd0;
            gc_obj <= 32'd0; gc_new <= 32'd0; gc_size <= 32'd0;
            gc_off <= 32'd0; gc_field <= 32'd0; gc_ret <= M_IDLE;
            sc_hdr <= 32'd0; sc_size <= 32'd0;
            cam_entries <= {32*CAM_WIDTH{1'b0}}; cam_lane_valid <= {CAM_WIDTH{1'b0}};
            lk_base <= 32'd0; lk_count <= 24'd0; lk_done <= 24'd0; lk_lane <= 6'd0;
            dw_pend <= 1'b0; dw_addr <= 32'd0; dw_data <= 8'h00;
            pf_req <= 1'b0; data_latch <= 8'h00;
            alu_start <= 1'b0; alu_verb <= 1'b0;
            mem_addr <= 32'd0; mem_we <= 1'b0; mem_wstrb <= 4'd0; mem_wdata <= 32'd0;
        end else begin
            // Single-cycle strobes.
            h_init  <= 1'b0;
            h_valid <= 1'b0;
            mem_we    <= 1'b0;        // writes are one cycle; default the port to read
            mem_wstrb <= 4'd0;
            alu_start <= 1'b0;        // one-shot

            // ------------------------------------------------ memory engine --------
            case (mstate)
                M_IDLE: begin
                    // A pending DATA write goes first, so a write followed by a read
                    // of the same address cannot observe stale data.
                    if (dw_pend) begin
                        mem_addr  <= dw_addr;
                        // Replicate the byte across all lanes and enable only the one
                        // the address selects, so a byte store needs no read-modify-
                        // write against the surrounding word.
                        mem_wdata <= {4{dw_data}};
                        mem_wstrb <= 4'b0001 << dw_addr[1:0];
                        mem_we    <= 1'b1;
                        dw_pend   <= 1'b0;
                    end else if (pf_req) begin
                        mem_addr <= areg[asel];
                        mret     <= M_PF_DONE;
                        mstate   <= M_RD_LATCH;
                        pf_req   <= 1'b0;
                    end
                end

                // mem_addr was presented last cycle, so mem_rdata is valid now. Keep
                // both the whole word (pointers, key hashes) and the addressed byte
                // (tags, streaming) -- which is the point of the wider port.
                M_RD_LATCH: begin
                    mword  <= mem_rdata;
                    mbyte  <= mem_rdata[8*mem_addr[1:0] +: 8];
                    mstate <= mret;
                end

                M_PF_DONE: begin
                    data_latch <= mbyte;
                    mstate     <= M_IDLE;
                end

                // --- HASH_STREAM: hash card-resident bytes; none cross the bus ---
                // One word fetch feeds up to four FIFO pushes. The gain here is
                // modest, because the hasher only accepts a byte every two cycles
                // regardless -- the wide port pays off in LOOKUP, not here.
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
                    if (mcount == 32'd0) begin
                        mstate <= M_STREAM;
                    end else if (!fifo_full) begin
                        fifo[wptr[4:0]] <= mword[8*mcur[1:0] +: 8];
                        wptr            <= wptr + 6'd1;
                        mcur            <= mcur + 32'd1;
                        mcount          <= mcount - 32'd1;
                        // Exhausted this word's lanes: fetch the next one.
                        if (mcur[1:0] == 2'b11) mstate <= M_STREAM;
                    end
                end

                // --- ALLOC: write the whole 4-byte header in one word access ---
                M_ALLOC_HDR: begin
                    mem_addr  <= mcur;
                    mem_wdata <= {pslot[0][255 - 8*3 -: 8], pslot[0][255 - 8*2 -: 8],
                                  pslot[0][255 - 8*1 -: 8], pslot[0][255 - 8*0 -: 8]};
                    mem_wstrb <= 4'b1111;
                    mem_we    <= 1'b1;
                    busy      <= 1'b0;
                    done      <= 1'b1;
                    mstate    <= M_IDLE;
                    pf_req    <= 1'b1;
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
                        hops     <= hops + 8'd1;
                        mem_addr <= mcur + 32'd4;   // the pointer, in one access
                        mret     <= M_FOLLOW_PTR;
                        mstate   <= M_RD_LATCH;
                    end
                end

                M_FOLLOW_PTR: begin
                    mcur   <= mword;    // pointers are stored little-endian
                    mstate <= M_FOLLOW;
                end

                // --- MEMCMP: LEN bytes at A0 against LEN bytes at A1, sign in P0.
                // A word at a time, which is the whole point of the verb: one
                // command replaces a byte-by-byte compare over the bus. The bytes
                // are compared in ADDRESS order, so the byte at +0 is the most
                // significant -- a plain little-endian word compare would order
                // two strings wrongly. The final partial word is masked, so the
                // answer does not depend on HEAP.md's zero padding being there. ---
                M_MC_A: begin
                    if (mc_rem == 32'd0) begin
                        pslot[0] <= 256'd0;                 // nothing to compare: equal
                        busy     <= 1'b0;
                        done     <= 1'b1;
                        mstate   <= M_IDLE;
                    end else begin
                        mem_addr <= mc_pa;
                        mret     <= M_MC_B;
                        mstate   <= M_RD_LATCH;
                    end
                end

                M_MC_B: begin
                    mc_aw    <= mword;                      // hold A while B is read
                    mem_addr <= mc_pb;
                    mret     <= M_MC_CMP;
                    mstate   <= M_RD_LATCH;
                end

                M_MC_CMP: begin
                    if (mc_cmp_a == mc_cmp_b) begin
                        if (mc_rem <= 32'd4) begin
                            pslot[0] <= 256'd0;
                            busy     <= 1'b0;
                            done     <= 1'b1;
                            mstate   <= M_IDLE;
                        end else begin
                            mc_pa  <= mc_pa + 32'd4;
                            mc_pb  <= mc_pb + 32'd4;
                            mc_rem <= mc_rem - 32'd4;
                            mstate <= M_MC_A;
                        end
                    end else begin
                        // $01 for A > B, $FF for A < B: a signed byte the caller
                        // reads from P0 byte 0, like strcmp's sign.
                        pslot[0] <= { (mc_cmp_a > mc_cmp_b) ? 8'h01 : 8'hFF, 248'd0 };
                        busy     <= 1'b0;
                        done     <= 1'b1;
                        mstate   <= M_IDLE;
                    end
                end

                // --- LOOKUP: the attribute count is the header's aux field, i.e.
                // the top 24 bits of the header word. One access. ---
                M_LK_CNT: begin
                    mem_addr <= lk_base;
                    mret     <= M_LK_CB;
                    mstate   <= M_RD_LATCH;
                end

                M_LK_CB: begin
                    lk_count <= mword[31:8];
                    lk_done  <= 24'd0;
                    mstate   <= M_LK_BATCH;
                end

                M_LK_BATCH: begin
                    if (lk_done >= lk_count) begin
                        // Every batch examined with no candidate.
                        busy    <= 1'b0;
                        err     <= 1'b1;
                        errcode <= E_NOTFOUND;
                        mstate  <= M_IDLE;
                    end else begin
                        cam_lane_valid <= {CAM_WIDTH{1'b0}};
                        lk_lane        <= 6'd0;
                        mstate         <= M_LK_LOAD;
                    end
                end

                // Each lane is one aligned word, so one access per lane. This is the
                // access that the wide port exists for: at byte width this was four,
                // and loading the column dominated LOOKUP's whole cost.
                M_LK_LOAD: begin
                    if (lk_lane == CAM_WIDTH[5:0] ||
                        (lk_done + {18'd0, lk_lane}) >= lk_count) begin
                        mstate <= M_LK_CAM;
                    end else begin
                        mem_addr <= lk_keys_at
                                    + ((({8'd0, lk_done} + {26'd0, lk_lane})) << 2);
                        mret     <= M_LK_HB;
                        mstate   <= M_RD_LATCH;
                    end
                end

                M_LK_HB: begin
                    // lk_lane is 6 bits so it can hold the CAM_WIDTH terminator, but
                    // only 5 bits index the array -- and this state is only reached
                    // while lk_lane < CAM_WIDTH.
                    cam_entries[32*lk_lane[4:0] +: 32] <= mword;
                    cam_lane_valid[lk_lane[4:0]]       <= 1'b1;
                    lk_lane                            <= lk_lane + 6'd1;
                    mstate                             <= M_LK_LOAD;
                end

                M_LK_CAM: begin
                    if (cam_hit) begin
                        // P1 carries the absolute attribute index, because a hash
                        // match is only a CANDIDATE -- 32-bit hashes collide, so the
                        // caller must still compare the name (docs/HEAP.md). Without
                        // the index it could not find the name to compare.
                        // P slots are byte arrays with byte 0 at the top, so the
                        // index goes out LSB-first to match how a caller reassembles
                        // it. Packing it as one 24-bit field instead would put byte 0
                        // at zero and the index big-endian in bytes 1..3 -- readable,
                        // plausible, and wrong.
                        pslot[1] <= {lk_hit_index[7:0], lk_hit_index[15:8],
                                     lk_hit_index[23:16], 232'd0};
                        mem_addr <= lk_values_at + ({8'd0, lk_hit_index} << 2);
                        mret     <= M_LK_VAL;
                        mstate   <= M_RD_LATCH;
                    end else begin
                        lk_done <= lk_done + {18'd0, lk_lane};
                        mstate  <= M_LK_BATCH;
                    end
                end

                M_LK_VAL: begin
                    areg[1] <= mword;       // the value pointer, in one access
                    busy    <= 1'b0;
                    done    <= 1'b1;
                    mstate  <= M_IDLE;
                    pf_req  <= 1'b1;
                end

                // ================= Cheney semispace collector =================
                // MARK: evacuate each root, writing its new address back in place.
                M_GC_ROOT: begin
                    if (gc_root_i >= gc_root_n) begin
                        mstate <= M_GC_SCAN;
                    end else begin
                        mem_addr <= gc_root_at + (gc_root_i << 2);
                        mret     <= M_GC_ROOT_RD;
                        mstate   <= M_RD_LATCH;
                    end
                end

                M_GC_ROOT_RD: begin
                    gc_obj <= mword;
                    gc_ret <= M_GC_ROOT_WB;
                    mstate <= M_GC_EVAC;
                end

                M_GC_ROOT_WB: begin
                    mem_addr  <= gc_root_at + (gc_root_i << 2);
                    mem_wdata <= gc_new;
                    mem_wstrb <= 4'b1111;
                    mem_we    <= 1'b1;
                    gc_root_i <= gc_root_i + 32'd1;
                    mstate    <= M_GC_ROOT;
                end

                // EVAC: gc_obj -> gc_new. Null and anything outside from-space (e.g.
                // already in to-space from an earlier MARK) pass through unchanged.
                M_GC_EVAC: begin
                    if (gc_obj == 32'd0 || !obj_in_from) begin
                        gc_new <= gc_obj;
                        mstate <= gc_ret;
                    end else begin
                        mem_addr <= gc_obj;
                        mret     <= M_GC_EVAC_HDR;
                        mstate   <= M_RD_LATCH;
                    end
                end

                M_GC_EVAC_HDR: begin
                    if (mword[7:0] == TAG_FORWARDED) begin
                        // Copied already (a shared object, or a cycle). Its word 1
                        // holds where it went. This is what keeps a DAG a DAG and
                        // a cycle finite.
                        mem_addr <= gc_obj + 32'd4;
                        mret     <= M_GC_EVAC_FWD;
                        mstate   <= M_RD_LATCH;
                    end else begin
                        gc_new  <= gc_free;
                        gc_size <= obj_size(mword);
                        gc_off  <= 32'd0;
                        mstate  <= M_GC_COPY;
                    end
                end

                M_GC_EVAC_FWD: begin
                    gc_new <= mword;
                    mstate <= gc_ret;
                end

                // Copy one word per 3 cycles: issue read, latch, write.
                M_GC_COPY: begin
                    if (gc_off >= gc_size) begin
                        // Whole object copied. Replace the original's header with a
                        // FORWARDED tag...
                        mem_addr  <= gc_obj;
                        mem_wdata <= {24'd0, TAG_FORWARDED};
                        mem_wstrb <= 4'b1111;
                        mem_we    <= 1'b1;
                        mstate    <= M_GC_COPY_FWD;
                    end else begin
                        mem_addr <= gc_obj + gc_off;
                        mret     <= M_GC_COPY_WR;
                        mstate   <= M_RD_LATCH;
                    end
                end

                M_GC_COPY_WR: begin
                    mem_addr  <= gc_new + gc_off;
                    mem_wdata <= mword;
                    mem_wstrb <= 4'b1111;
                    mem_we    <= 1'b1;
                    gc_off    <= gc_off + 32'd4;
                    mstate    <= M_GC_COPY;
                end

                M_GC_COPY_FWD: begin
                    // ...and its first payload word with the forwarding address.
                    // Every object is >= 8 bytes so this word exists (docs/HEAP.md).
                    mem_addr  <= gc_obj + 32'd4;
                    mem_wdata <= gc_new;
                    mem_wstrb <= 4'b1111;
                    mem_we    <= 1'b1;
                    gc_free   <= gc_free + gc_size;
                    mstate    <= gc_ret;
                end

                // SCAN: walk to-space from gc_scan to gc_free, tracing pointer words
                // in place. Objects copied by the scan land beyond gc_free and are
                // themselves scanned later -- the Cheney queue.
                M_GC_SCAN: begin
                    if (gc_scan >= gc_free) begin
                        busy   <= 1'b0;
                        done   <= 1'b1;
                        mstate <= M_IDLE;
                        pf_req <= 1'b1;
                    end else begin
                        mem_addr <= gc_scan;
                        mret     <= M_GC_SCAN_HDR;
                        mstate   <= M_RD_LATCH;
                    end
                end

                M_GC_SCAN_HDR: begin
                    sc_hdr   <= mword;
                    sc_size  <= obj_size(mword);
                    gc_field <= 32'd0;
                    mstate   <= M_GC_SCAN_F;
                end

                M_GC_SCAN_F: begin
                    if (32'd4 + (gc_field << 2) >= sc_size) begin
                        gc_scan <= gc_scan + sc_size;
                        mstate  <= M_GC_SCAN;
                    end else if (is_ptr_field(sc_hdr, gc_field)) begin
                        mem_addr <= gc_scan + 32'd4 + (gc_field << 2);
                        mret     <= M_GC_SCAN_FRD;
                        mstate   <= M_RD_LATCH;
                    end else begin
                        gc_field <= gc_field + 32'd1;   // a hash, a byte, an int
                    end
                end

                M_GC_SCAN_FRD: begin
                    gc_obj <= mword;
                    gc_ret <= M_GC_SCAN_WB;
                    mstate <= M_GC_EVAC;
                end

                M_GC_SCAN_WB: begin
                    mem_addr  <= gc_scan + 32'd4 + (gc_field << 2);
                    mem_wdata <= gc_new;
                    mem_wstrb <= 4'b1111;
                    mem_we    <= 1'b1;
                    gc_field  <= gc_field + 32'd1;
                    mstate    <= M_GC_SCAN_F;
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
                end else if (alu_verb && alu_done) begin
                    // alu_done is a one-cycle pulse, so unlike h_done it cannot go
                    // stale -- but gate on alu_verb anyway, for the same reason R1
                    // exists: no completion path may fire for a verb it does not own.
                    alu_verb <= 1'b0;
                    busy     <= 1'b0;
                    if (alu_err) begin
                        err     <= 1'b1;
                        errcode <= E_BADPARAM;    // divide by zero, or a bad op
                    end else begin
                        pslot[3] <= { alu_result[7:0],   alu_result[15:8],
                                      alu_result[23:16], alu_result[31:24],
                                      alu_result[39:32], alu_result[47:40],
                                      alu_result[55:48], alu_result[63:56],
                                      192'd0 };
                        done     <= 1'b1;
                    end
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
                                    // Payload rounded to 4 so pointer columns stay
                                    // aligned; total never below 8 so a forwarding
                                    // pointer fits (docs/HEAP.md). Bounded by the
                                    // current semispace, not the whole heap.
                                    if ((alloc_ptr + alloc_total(len)) > from_top) begin
                                        err     <= 1'b1;
                                        errcode <= E_OOM;
                                    end else begin
                                        areg[1]   <= alloc_ptr;
                                        mcur      <= alloc_ptr;
                                        alloc_ptr <= alloc_ptr + alloc_total(len);
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

                                // RESERVE: claim LEN bytes (rounded to 4, min 8) with
                                // no header written, for the nursery's promotion
                                // stream -- survivors are laid out contiguously by
                                // the CPU's Cheney copy and pushed as one DATA burst.
                                // A1 = base. Single cycle.
                                V_RESERVE: begin
                                    if ((alloc_ptr + alloc_total(len) - 32'd4) > from_top) begin
                                        err     <= 1'b1;
                                        errcode <= E_OOM;
                                    end else begin
                                        areg[1]   <= alloc_ptr;
                                        alloc_ptr <= alloc_ptr + alloc_total(len) - 32'd4;
                                        done      <= 1'b1;
                                    end
                                end

                                // MEMCMP: compare LEN bytes at A0 against LEN
                                // bytes at A1 and report the sign in P0. It earns
                                // its crossing by amortizing a whole string
                                // comparison into one verb -- and by letting the
                                // CPU confirm a LOOKUP candidate's name without
                                // copying the set back across the bus
                                // (docs/CARD-ISA.md, docs/HEAP.md).
                                V_MEMCMP: begin
                                    // Word-aligned operands only. Every string
                                    // payload in the heap is word-aligned by
                                    // construction (docs/HEAP.md), and requiring it
                                    // is what lets the verb compare whole words.
                                    if (areg[0][1:0] != 2'b00 || areg[1][1:0] != 2'b00) begin
                                        err     <= 1'b1;
                                        errcode <= E_BADPARAM;
                                    end else begin
                                        mc_pa  <= areg[0];
                                        mc_pb  <= areg[1];
                                        mc_rem <= len;
                                        busy   <= 1'b1;
                                        mstate <= M_MC_A;
                                    end
                                end

                                V_LOOKUP: begin
                                    lk_base <= areg[0];
                                    busy    <= 1'b1;
                                    mstate  <= M_LK_CNT;
                                end

                                V_ALU64: begin
                                    alu_start <= 1'b1;
                                    alu_verb  <= 1'b1;
                                    busy      <= 1'b1;
                                end

                                V_MARK: begin
                                    gc_root_at <= areg[0];
                                    gc_root_n  <= len;
                                    gc_root_i  <= 32'd0;
                                    busy       <= 1'b1;
                                    mstate     <= M_GC_ROOT;
                                end

                                // Flip: survivors are now the from-space, allocation
                                // resumes right after them, and the old from-space
                                // is the empty to-space for next time.
                                V_COMPACT: begin
                                    alloc_ptr <= gc_free;
                                    space_sel <= ~space_sel;
                                    gc_free   <= from_base;     // == the new to_base
                                    gc_scan   <= from_base;
                                    done      <= 1'b1;
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
