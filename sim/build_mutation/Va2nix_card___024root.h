// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design internal header
// See Va2nix_card.h for the primary calling header

#ifndef VERILATED_VA2NIX_CARD___024ROOT_H_
#define VERILATED_VA2NIX_CARD___024ROOT_H_  // guard

#include "verilated.h"


class Va2nix_card__Syms;

class alignas(VL_CACHE_LINE_BYTES) Va2nix_card___024root final {
  public:

    // DESIGN SPECIFIC STATE
    // Anonymous structures to workaround compiler member-count bugs
    struct {
        VL_IN8(clk,0,0);
        VL_IN8(rst_n,0,0);
        VL_IN8(c0n_addr,3,0);
        VL_IN8(c0n_read,0,0);
        VL_IN8(c0n_write,0,0);
        VL_IN8(c0n_wdata,7,0);
        VL_OUT8(c0n_rdata,7,0);
        VL_OUT8(c8_rdata,7,0);
        VL_OUT8(take_c800,0,0);
        VL_OUT8(mem_we,0,0);
        VL_OUT8(mem_wdata,7,0);
        VL_IN8(mem_rdata,7,0);
        VL_OUT8(irq,0,0);
        CData/*1:0*/ a2nix_card__DOT__asel;
        CData/*1:0*/ a2nix_card__DOT__aidx;
        CData/*2:0*/ a2nix_card__DOT__psel;
        CData/*4:0*/ a2nix_card__DOT__pidx;
        CData/*1:0*/ a2nix_card__DOT__lenidx;
        CData/*0:0*/ a2nix_card__DOT__busy;
        CData/*0:0*/ a2nix_card__DOT__done;
        CData/*0:0*/ a2nix_card__DOT__err;
        CData/*3:0*/ a2nix_card__DOT__errcode;
        CData/*0:0*/ a2nix_card__DOT__irq_en;
        CData/*0:0*/ a2nix_card__DOT__irq_pend;
        CData/*0:0*/ a2nix_card__DOT__push_mode;
        CData/*0:0*/ a2nix_card__DOT__h_init;
        CData/*0:0*/ a2nix_card__DOT__h_valid;
        CData/*7:0*/ a2nix_card__DOT__h_byte;
        CData/*0:0*/ a2nix_card__DOT__h_finish;
        CData/*0:0*/ a2nix_card__DOT__finalizing;
        CData/*0:0*/ a2nix_card__DOT__h_done;
        CData/*5:0*/ a2nix_card__DOT__wptr;
        CData/*5:0*/ a2nix_card__DOT__rptr;
        CData/*5:0*/ a2nix_card__DOT__fifo_count;
        CData/*0:0*/ a2nix_card__DOT__fifo_empty;
        CData/*0:0*/ a2nix_card__DOT__fifo_drain;
        CData/*4:0*/ a2nix_card__DOT__mstate;
        CData/*4:0*/ a2nix_card__DOT__mret;
        CData/*7:0*/ a2nix_card__DOT__mbyte;
        CData/*2:0*/ a2nix_card__DOT__mbidx;
        CData/*7:0*/ a2nix_card__DOT__hops;
        CData/*0:0*/ a2nix_card__DOT__dw_pend;
        CData/*7:0*/ a2nix_card__DOT__dw_data;
        CData/*0:0*/ a2nix_card__DOT__pf_req;
        CData/*7:0*/ a2nix_card__DOT__data_latch;
        CData/*5:0*/ a2nix_card__DOT__lk_lane;
        CData/*2:0*/ a2nix_card__DOT__u_hash__DOT__state;
        CData/*2:0*/ a2nix_card__DOT__u_hash__DOT__ret;
        CData/*6:0*/ a2nix_card__DOT__u_hash__DOT__bidx;
        CData/*2:0*/ a2nix_card__DOT__u_hash__DOT__lenidx;
        CData/*0:0*/ a2nix_card__DOT__u_hash__DOT__core_init;
        CData/*0:0*/ a2nix_card__DOT__u_hash__DOT__core_start;
        CData/*0:0*/ a2nix_card__DOT__u_hash__DOT__core_busy;
        CData/*0:0*/ a2nix_card__DOT__u_hash__DOT__pb_v;
        CData/*0:0*/ a2nix_card__DOT__u_hash__DOT__blk_full;
        CData/*6:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__round;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_5;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_6;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_7;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_8;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_9;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_10;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_11;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_12;
    };
    struct {
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_13;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_14;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_15;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_16;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_17;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_18;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_19;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_20;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_21;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_22;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_23;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_24;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_25;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_26;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_27;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_28;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_29;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_30;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_31;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_32;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_33;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_34;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_35;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_36;
        CData/*0:0*/ __VdfgRegularize_h6e95ff9d_0_45;
        CData/*0:0*/ __Vtrigprevexpr___TOP__clk__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__rst_n__0;
        CData/*3:0*/ __Vtrigprevexpr___TOP__c0n_addr__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__c0n_read__0;
        CData/*0:0*/ __Vtrigprevexpr___TOP__c0n_write__0;
        CData/*7:0*/ __Vtrigprevexpr___TOP__c0n_wdata__0;
        CData/*7:0*/ __Vtrigprevexpr___TOP__mem_rdata__0;
        CData/*0:0*/ __VicoDidInit;
        CData/*0:0*/ __Vtrigprevexpr___TOP__clk__1;
        VL_IN16(c8_addr,10,0);
        SData/*10:0*/ __Vtrigprevexpr___TOP__c8_addr__0;
        VL_OUT(mem_addr,31,0);
        IData/*31:0*/ a2nix_card__DOT__len;
        VlWide<8>/*255:0*/ a2nix_card__DOT__h_digest;
        VlWide<8>/*255:0*/ a2nix_card__DOT__sp_path;
        IData/*31:0*/ a2nix_card__DOT__mcur;
        IData/*31:0*/ a2nix_card__DOT__mptr;
        IData/*31:0*/ a2nix_card__DOT__mcount;
        IData/*31:0*/ a2nix_card__DOT__alloc_ptr;
        IData/*31:0*/ a2nix_card__DOT__dw_addr;
        VlWide<32>/*1023:0*/ a2nix_card__DOT__cam_entries;
        IData/*31:0*/ a2nix_card__DOT__cam_lane_valid;
        IData/*31:0*/ a2nix_card__DOT__lk_base;
        IData/*23:0*/ a2nix_card__DOT__lk_count;
        IData/*23:0*/ a2nix_card__DOT__lk_done;
        IData/*31:0*/ a2nix_card__DOT__lk_keys_at;
        IData/*23:0*/ a2nix_card__DOT__lk_hit_index;
        VlWide<16>/*511:0*/ a2nix_card__DOT__u_hash__DOT__blk;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__a;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__b;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__c;
    };
    struct {
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__d;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__e;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__f;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__g;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__h;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__t1;
        IData/*31:0*/ a2nix_card__DOT__u_hash__DOT__u_core__DOT__t2;
        QData/*63:0*/ a2nix_card__DOT__u_hash__DOT__total;
        VlUnpacked<IData/*31:0*/, 4> a2nix_card__DOT__areg;
        VlUnpacked<VlWide<8>/*255:0*/, 8> a2nix_card__DOT__pslot;
        VlUnpacked<CData/*7:0*/, 32> a2nix_card__DOT__fifo;
        VlUnpacked<IData/*31:0*/, 16> a2nix_card__DOT__u_hash__DOT__u_core__DOT__w;
        VlUnpacked<QData/*63:0*/, 1> __VstlTriggered;
        VlUnpacked<QData/*63:0*/, 2> __VicoTriggered;
        VlUnpacked<QData/*63:0*/, 1> __VactTriggered;
        VlUnpacked<QData/*63:0*/, 1> __VnbaTriggered;
    };

    // INTERNAL VARIABLES
    Va2nix_card__Syms* vlSymsp;
    const char* vlNamep;

    // CONSTRUCTORS
    Va2nix_card___024root(Va2nix_card__Syms* symsp, const char* namep);
    ~Va2nix_card___024root();
    VL_UNCOPYABLE(Va2nix_card___024root);

    // INTERNAL METHODS
    void __Vconfigure(bool first);
};


#endif  // guard
