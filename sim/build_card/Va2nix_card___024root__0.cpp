// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Va2nix_card.h for the primary calling header

#include "Va2nix_card__pch.h"

void Va2nix_card___024root___eval_sample(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_sample\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Va2nix_card___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG
bool Va2nix_card___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

bool Va2nix_card___024root___eval_ico(Va2nix_card___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_ico\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VicoExecute;
    // Body
    vlSelfRef.__VicoTriggered[1U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VicoTriggered[1U]) 
                                     | (IData)((IData)(firstIteration)));
    {
        // Inlined CFunc: _eval_triggers_vec__ico
        vlSelfRef.__VicoTriggered[0U] = (QData)((IData)(
                                                        (((((IData)(vlSelfRef.c8_addr) 
                                                            != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__c8_addr__0)) 
                                                           << 6U) 
                                                          | ((((IData)(vlSelfRef.c0n_wdata) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__c0n_wdata__0)) 
                                                              << 5U) 
                                                             | (((IData)(vlSelfRef.c0n_write) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__c0n_write__0)) 
                                                                << 4U))) 
                                                         | (((((IData)(vlSelfRef.c0n_read) 
                                                               != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__c0n_read__0)) 
                                                              << 3U) 
                                                             | (((IData)(vlSelfRef.c0n_addr) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__c0n_addr__0)) 
                                                                << 2U)) 
                                                            | ((((IData)(vlSelfRef.rst_n) 
                                                                 != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__rst_n__0)) 
                                                                << 1U) 
                                                               | ((IData)(vlSelfRef.clk) 
                                                                  != (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__0)))))));
        vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
        vlSelfRef.__Vtrigprevexpr___TOP__rst_n__0 = vlSelfRef.rst_n;
        vlSelfRef.__Vtrigprevexpr___TOP__c0n_addr__0 
            = vlSelfRef.c0n_addr;
        vlSelfRef.__Vtrigprevexpr___TOP__c0n_read__0 
            = vlSelfRef.c0n_read;
        vlSelfRef.__Vtrigprevexpr___TOP__c0n_write__0 
            = vlSelfRef.c0n_write;
        vlSelfRef.__Vtrigprevexpr___TOP__c0n_wdata__0 
            = vlSelfRef.c0n_wdata;
        vlSelfRef.__Vtrigprevexpr___TOP__c8_addr__0 
            = vlSelfRef.c8_addr;
        if (VL_UNLIKELY(((1U & (~ (IData)(vlSelfRef.__VicoDidInit)))))) {
            vlSelfRef.__VicoDidInit = 1U;
            vlSelfRef.__VicoTriggered[0U] = (1ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (2ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (4ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (8ULL | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (0x0000000000000010ULL 
                                             | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (0x0000000000000020ULL 
                                             | vlSelfRef.__VicoTriggered[0U]);
            vlSelfRef.__VicoTriggered[0U] = (0x0000000000000040ULL 
                                             | vlSelfRef.__VicoTriggered[0U]);
        }
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Va2nix_card___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
    }
#endif
    __VicoExecute = Va2nix_card___024root___trigger_anySet__ico(vlSelfRef.__VicoTriggered);
    if (__VicoExecute) {
        {
            // Inlined CFunc: _eval_body__ico
            if ((4ULL & vlSelfRef.__VicoTriggered[0U])) {
                {
                    // Inlined CFunc: _ico_sequent__TOP__0
                    CData/*31:0*/ __Vinline_0__eval_body__ico___Vinline_0__ico_sequent__TOP__0___VExpandSel_Lsb_1;
                    __Vinline_0__eval_body__ico___Vinline_0__ico_sequent__TOP__0___VExpandSel_Lsb_1 
                        = (0x000000ffU & (((IData)(0xffU) 
                                           - ((IData)(vlSelfRef.a2nix_card__DOT__pidx) 
                                              << 3U)) 
                                          - (IData)(7U)));
                    vlSelfRef.c0n_rdata = (0x000000ffU 
                                           & ((8U & (IData)(vlSelfRef.c0n_addr))
                                               ? ((- (IData)(
                                                             (1U 
                                                              & (~ 
                                                                 ((IData)(vlSelfRef.c0n_addr) 
                                                                  >> 1U))))) 
                                                  & (((1U 
                                                       & (IData)(vlSelfRef.c0n_addr))
                                                       ? (IData)(vlSelfRef.a2nix_card__DOT__irq_pend)
                                                       : 
                                                      (vlSelfRef.a2nix_card__DOT__len 
                                                       >> 
                                                       ((IData)(vlSelfRef.a2nix_card__DOT__lenidx) 
                                                        << 3U))) 
                                                     & (- (IData)(
                                                                  (1U 
                                                                   & (~ 
                                                                      ((IData)(vlSelfRef.c0n_addr) 
                                                                       >> 2U)))))))
                                               : ((4U 
                                                   & (IData)(vlSelfRef.c0n_addr))
                                                   ? 
                                                  ((3U 
                                                    == 
                                                    (3U 
                                                     & (IData)(vlSelfRef.c0n_addr)))
                                                    ? 
                                                   (((0U 
                                                      == 
                                                      (0x0000001fU 
                                                       & __Vinline_0__eval_body__ico___Vinline_0__ico_sequent__TOP__0___VExpandSel_Lsb_1))
                                                      ? 0U
                                                      : 
                                                     (vlSelfRef.a2nix_card__DOT__pslot
                                                      [vlSelfRef.a2nix_card__DOT__psel]
                                                      [
                                                      (((IData)(7U) 
                                                        + __Vinline_0__eval_body__ico___Vinline_0__ico_sequent__TOP__0___VExpandSel_Lsb_1) 
                                                       >> 5U)] 
                                                      << 
                                                      ((IData)(0x00000020U) 
                                                       - 
                                                       (0x0000001fU 
                                                        & __Vinline_0__eval_body__ico___Vinline_0__ico_sequent__TOP__0___VExpandSel_Lsb_1)))) 
                                                    | (vlSelfRef.a2nix_card__DOT__pslot
                                                       [vlSelfRef.a2nix_card__DOT__psel]
                                                       [
                                                       (__Vinline_0__eval_body__ico___Vinline_0__ico_sequent__TOP__0___VExpandSel_Lsb_1 
                                                        >> 5U)] 
                                                       >> 
                                                       (0x0000001fU 
                                                        & __Vinline_0__eval_body__ico___Vinline_0__ico_sequent__TOP__0___VExpandSel_Lsb_1)))
                                                    : 0U)
                                                   : 
                                                  ((2U 
                                                    & (IData)(vlSelfRef.c0n_addr))
                                                    ? 
                                                   ((1U 
                                                     & (IData)(vlSelfRef.c0n_addr))
                                                     ? 
                                                    (vlSelfRef.a2nix_card__DOT__areg
                                                     [vlSelfRef.a2nix_card__DOT__asel] 
                                                     >> 
                                                     ((IData)(vlSelfRef.a2nix_card__DOT__aidx) 
                                                      << 3U))
                                                     : 0U)
                                                    : 
                                                   ((1U 
                                                     & (IData)(vlSelfRef.c0n_addr))
                                                     ? 0xa2U
                                                     : 
                                                    ((((IData)(vlSelfRef.a2nix_card__DOT__busy) 
                                                       << 7U) 
                                                      | ((IData)(vlSelfRef.a2nix_card__DOT__done) 
                                                         << 6U)) 
                                                     | (((IData)(vlSelfRef.a2nix_card__DOT__err) 
                                                         << 5U) 
                                                        | ((((~ (IData)(vlSelfRef.a2nix_card__DOT__h_valid)) 
                                                             & (1U 
                                                                == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))) 
                                                            << 4U) 
                                                           | (IData)(vlSelfRef.a2nix_card__DOT__errcode)))))))));
                }
            }
        }
    }
    return (__VicoExecute);
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Va2nix_card___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
void Va2nix_card___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in);

bool Va2nix_card___024root___eval_act(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_act\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    {
        // Inlined CFunc: _eval_triggers_vec__act
        vlSelfRef.__VactTriggered[0U] = (QData)((IData)(
                                                        ((IData)(vlSelfRef.clk) 
                                                         & (~ (IData)(vlSelfRef.__Vtrigprevexpr___TOP__clk__1)))));
        vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
    }
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Va2nix_card___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
    }
#endif
    Va2nix_card___024root___trigger_orInto__act_vec_vec(vlSelfRef.__VnbaTriggered, vlSelfRef.__VactTriggered);
    return (0U);
}

bool Va2nix_card___024root___eval_inact(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_inact\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Va2nix_card___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);
void Va2nix_card___024root___nba_sequent__TOP__0(Va2nix_card___024root* vlSelf);
void Va2nix_card___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out);

bool Va2nix_card___024root___eval_nba(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_nba\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VnbaExecute;
    // Body
    __VnbaExecute = Va2nix_card___024root___trigger_anySet__act(vlSelfRef.__VnbaTriggered);
    if (__VnbaExecute) {
        {
            // Inlined CFunc: _eval_body__nba
            if ((1ULL & vlSelfRef.__VnbaTriggered[0U])) {
                Va2nix_card___024root___nba_sequent__TOP__0(vlSelf);
            }
        }
        Va2nix_card___024root___trigger_clear__act(vlSelfRef.__VnbaTriggered);
    }
    return (__VnbaExecute);
}

bool Va2nix_card___024root___eval_obs(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_obs\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

bool Va2nix_card___024root___eval_react(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_react\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    return (0U);
}

void Va2nix_card___024root___eval_postponed(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_postponed\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

bool Va2nix_card___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___trigger_anySet__ico\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        if (in[n]) {
            return (1U);
        }
        n = ((IData)(1U) + n);
    } while ((2U > n));
    return (0U);
}

bool Va2nix_card___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___trigger_anySet__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        if (in[n]) {
            return (1U);
        }
        n = ((IData)(1U) + n);
    } while ((1U > n));
    return (0U);
}

extern const VlWide<16>/*511:0*/ Va2nix_card__ConstPool__CONST_h93e1b771_0;
extern const VlWide<64>/*2047:0*/ Va2nix_card__ConstPool__CONST_h7be248c9_0;
extern const VlWide<8>/*255:0*/ Va2nix_card__ConstPool__CONST_h9e67c271_0;
extern const VlWide<32>/*1023:0*/ Va2nix_card__ConstPool__CONST_h4c4fb9bc_0;

void Va2nix_card___024root___nba_sequent__TOP__0(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___nba_sequent__TOP__0\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    VlWide<8>/*255:0*/ a2nix_card__DOT__sp_digest;
    VL_ZERO_W(256, a2nix_card__DOT__sp_digest);
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_0;
    __VdfgRegularize_h6e95ff9d_0_0 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_1;
    __VdfgRegularize_h6e95ff9d_0_1 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_2;
    __VdfgRegularize_h6e95ff9d_0_2 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_3;
    __VdfgRegularize_h6e95ff9d_0_3 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_4;
    __VdfgRegularize_h6e95ff9d_0_4 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_5;
    __VdfgRegularize_h6e95ff9d_0_5 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_6;
    __VdfgRegularize_h6e95ff9d_0_6 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_7;
    __VdfgRegularize_h6e95ff9d_0_7 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_8;
    __VdfgRegularize_h6e95ff9d_0_8 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_9;
    __VdfgRegularize_h6e95ff9d_0_9 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_10;
    __VdfgRegularize_h6e95ff9d_0_10 = 0;
    CData/*7:0*/ __VdfgRegularize_h6e95ff9d_0_11;
    __VdfgRegularize_h6e95ff9d_0_11 = 0;
    CData/*0:0*/ __Vdly__a2nix_card__DOT__busy;
    __Vdly__a2nix_card__DOT__busy = 0;
    CData/*1:0*/ __Vdly__a2nix_card__DOT__lenidx;
    __Vdly__a2nix_card__DOT__lenidx = 0;
    CData/*4:0*/ __Vdly__a2nix_card__DOT__pidx;
    __Vdly__a2nix_card__DOT__pidx = 0;
    CData/*1:0*/ __Vdly__a2nix_card__DOT__aidx;
    __Vdly__a2nix_card__DOT__aidx = 0;
    CData/*6:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__bidx;
    __Vdly__a2nix_card__DOT__u_hash__DOT__bidx = 0;
    CData/*0:0*/ __Vdly__a2nix_card__DOT__h_done;
    __Vdly__a2nix_card__DOT__h_done = 0;
    CData/*2:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__lenidx;
    __Vdly__a2nix_card__DOT__u_hash__DOT__lenidx = 0;
    CData/*2:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__state;
    __Vdly__a2nix_card__DOT__u_hash__DOT__state = 0;
    CData/*2:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__ret;
    __Vdly__a2nix_card__DOT__u_hash__DOT__ret = 0;
    VlWide<16>/*511:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__blk;
    VL_ZERO_W(512, __Vdly__a2nix_card__DOT__u_hash__DOT__blk);
    CData/*0:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__core_busy;
    __Vdly__a2nix_card__DOT__u_hash__DOT__core_busy = 0;
    CData/*6:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__round;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__round = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0 = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1 = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2 = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3 = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4 = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5 = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6 = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7 = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__a;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__a = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__b;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__b = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__c;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__c = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__d;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__d = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__e;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__e = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__f;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__f = 0;
    IData/*31:0*/ __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__g;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__g = 0;
    VlWide<8>/*255:0*/ __VdlyVal__a2nix_card__DOT__pslot__v0;
    VL_ZERO_W(256, __VdlyVal__a2nix_card__DOT__pslot__v0);
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__pslot__v0;
    __VdlySet__a2nix_card__DOT__pslot__v0 = 0;
    VlWide<8>/*255:0*/ __VdlyVal__a2nix_card__DOT__pslot__v1;
    VL_ZERO_W(256, __VdlyVal__a2nix_card__DOT__pslot__v1);
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__pslot__v1;
    __VdlySet__a2nix_card__DOT__pslot__v1 = 0;
    CData/*7:0*/ __VdlyVal__a2nix_card__DOT__pslot__v2;
    __VdlyVal__a2nix_card__DOT__pslot__v2 = 0;
    CData/*7:0*/ __VdlyLsb__a2nix_card__DOT__pslot__v2;
    __VdlyLsb__a2nix_card__DOT__pslot__v2 = 0;
    CData/*2:0*/ __VdlyDim0__a2nix_card__DOT__pslot__v2;
    __VdlyDim0__a2nix_card__DOT__pslot__v2 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__pslot__v2;
    __VdlySet__a2nix_card__DOT__pslot__v2 = 0;
    CData/*7:0*/ __VdlyVal__a2nix_card__DOT__areg__v0;
    __VdlyVal__a2nix_card__DOT__areg__v0 = 0;
    CData/*4:0*/ __VdlyLsb__a2nix_card__DOT__areg__v0;
    __VdlyLsb__a2nix_card__DOT__areg__v0 = 0;
    CData/*1:0*/ __VdlyDim0__a2nix_card__DOT__areg__v0;
    __VdlyDim0__a2nix_card__DOT__areg__v0 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__areg__v0;
    __VdlySet__a2nix_card__DOT__areg__v0 = 0;
    VlWide<8>/*255:0*/ __VdlyVal__a2nix_card__DOT__pslot__v3;
    VL_ZERO_W(256, __VdlyVal__a2nix_card__DOT__pslot__v3);
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__pslot__v3;
    __VdlySet__a2nix_card__DOT__pslot__v3 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__areg__v1;
    __VdlySet__a2nix_card__DOT__areg__v1 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v0;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v0 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v0;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v0 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v1;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v1 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v1;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v1 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v2;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v2 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v3;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v3 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v4;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v4 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v5;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v5 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v6;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v6 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v7;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v7 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v8;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v8 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v9;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v9 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v10;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v10 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v11;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v11 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v12;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v12 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v13;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v13 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v14;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v14 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v15;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v15 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v16;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v16 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v16;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v16 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v17;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v17 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v17;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v17 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v18;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v18 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v19;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v19 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v20;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v20 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v20;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v20 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v21;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v21 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v22;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v22 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v23;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v23 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v23;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v23 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v24;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v24 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v25;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v25 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v26;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v26 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v26;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v26 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v27;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v27 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v28;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v28 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v29;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v29 = 0;
    CData/*0:0*/ __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v29;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v29 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v30;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v30 = 0;
    IData/*31:0*/ __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v31;
    __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v31 = 0;
    CData/*31:0*/ __VExpandSel_Lsb_1;
    IData/*31:0*/ __VExpandSel_Lsb_2;
    IData/*31:0*/ __VExpandSel_Lsb_3;
    IData/*31:0*/ __VExpandSel_Lsb_4;
    IData/*31:0*/ __VExpandSel_Lsb_5;
    IData/*31:0*/ __VExpandSel_Lsb_6;
    IData/*31:0*/ __VExpandSel_Lsb_7;
    IData/*31:0*/ __VExpandSel_Lsb_8;
    IData/*31:0*/ __VExpandSel_Lsb_9;
    IData/*31:0*/ __VExpandSel_Lsb_10;
    IData/*31:0*/ __VExpandSel_Lsb_11;
    IData/*31:0*/ __VExpandSel_Lsb_12;
    IData/*31:0*/ __VExpandSel_Lsb_13;
    IData/*31:0*/ __VExpandSel_Lsb_14;
    IData/*31:0*/ __VExpandSel_Lsb_15;
    IData/*31:0*/ __VExpandSel_Lsb_16;
    IData/*31:0*/ __VExpandSel_Lsb_17;
    // Body
    __Vdly__a2nix_card__DOT__lenidx = vlSelfRef.a2nix_card__DOT__lenidx;
    __Vdly__a2nix_card__DOT__pidx = vlSelfRef.a2nix_card__DOT__pidx;
    __Vdly__a2nix_card__DOT__aidx = vlSelfRef.a2nix_card__DOT__aidx;
    __VdlySet__a2nix_card__DOT__areg__v0 = 0U;
    __Vdly__a2nix_card__DOT__busy = vlSelfRef.a2nix_card__DOT__busy;
    __VdlySet__a2nix_card__DOT__pslot__v0 = 0U;
    __VdlySet__a2nix_card__DOT__pslot__v1 = 0U;
    __VdlySet__a2nix_card__DOT__pslot__v2 = 0U;
    __VdlySet__a2nix_card__DOT__pslot__v3 = 0U;
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[0U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[0U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[1U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[1U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[2U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[2U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[3U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[3U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[4U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[4U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[5U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[5U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[6U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[6U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[7U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[7U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[8U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[8U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[9U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[9U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[10U] 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[10U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[11U] 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[11U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[12U] 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[12U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[13U] 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[13U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[14U] 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[14U];
    __Vdly__a2nix_card__DOT__u_hash__DOT__blk[15U] 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[15U];
    __VdlySet__a2nix_card__DOT__areg__v1 = 0U;
    __Vdly__a2nix_card__DOT__h_done = vlSelfRef.a2nix_card__DOT__h_done;
    __Vdly__a2nix_card__DOT__u_hash__DOT__lenidx = vlSelfRef.a2nix_card__DOT__u_hash__DOT__lenidx;
    __Vdly__a2nix_card__DOT__u_hash__DOT__ret = vlSelfRef.a2nix_card__DOT__u_hash__DOT__ret;
    __Vdly__a2nix_card__DOT__u_hash__DOT__bidx = vlSelfRef.a2nix_card__DOT__u_hash__DOT__bidx;
    __Vdly__a2nix_card__DOT__u_hash__DOT__state = vlSelfRef.a2nix_card__DOT__u_hash__DOT__state;
    __Vdly__a2nix_card__DOT__u_hash__DOT__core_busy 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_busy;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__d 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__d;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__b 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__b;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__c 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__c;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__round 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__round;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__f 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__f;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__g 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__g;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v0 = 0U;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v1 = 0U;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v16 = 0U;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v17 = 0U;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v20 = 0U;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v23 = 0U;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v26 = 0U;
    __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v29 = 0U;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6;
    __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7 
        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7;
    if (vlSelfRef.rst_n) {
        if (vlSelfRef.a2nix_card__DOT__u_hash__DOT__pb_v) {
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[0U] 
                = ((0xffffff00U & __Vdly__a2nix_card__DOT__u_hash__DOT__blk[0U]) 
                   | (0x000000ffU & ((4U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))
                                      ? ((2U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))
                                          ? ((1U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))
                                              ? 0U : (IData)(
                                                             ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__total 
                                                               << 3U) 
                                                              >> 
                                                              (0x0000003fU 
                                                               & (((IData)(0x3fU) 
                                                                   - 
                                                                   ((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__lenidx) 
                                                                    << 3U)) 
                                                                  - (IData)(7U))))))
                                          : (0x80U 
                                             & (- (IData)(
                                                          (1U 
                                                           & (~ (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state)))))))
                                      : ((IData)(vlSelfRef.a2nix_card__DOT__h_byte) 
                                         & (- (IData)((IData)(vlSelfRef.__VdfgRegularize_h6e95ff9d_0_13)))))));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[0U] 
                = ((0x000000ffU & __Vdly__a2nix_card__DOT__u_hash__DOT__blk[0U]) 
                   | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[0U] 
                      << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[1U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[0U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[1U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[2U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[1U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[2U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[3U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[2U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[3U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[4U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[3U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[4U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[5U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[4U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[5U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[6U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[5U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[6U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[7U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[6U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[7U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[8U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[7U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[8U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[9U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[8U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[9U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[10U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[9U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[10U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[11U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[10U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[11U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[12U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[11U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[12U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[13U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[12U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[13U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[14U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[13U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[14U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__blk[15U] 
                = ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[14U] 
                    >> 0x00000018U) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[15U] 
                                       << 8U));
            __Vdly__a2nix_card__DOT__u_hash__DOT__bidx 
                = ((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk_full)
                    ? 0U : (0x0000007fU & ((IData)(1U) 
                                           + (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__bidx))));
        }
        if (vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_init) {
            __Vdly__a2nix_card__DOT__u_hash__DOT__core_busy = 0U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__round = 0U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0 = 0x6a09e667U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1 = 0xbb67ae85U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2 = 0x3c6ef372U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3 = 0xa54ff53aU;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4 = 0x510e527fU;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5 = 0x9b05688cU;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6 = 0x1f83d9abU;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7 = 0x5be0cd19U;
        } else if (((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_start) 
                    & (~ (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_busy)))) {
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__b 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__c 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__d 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__f 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__g 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6;
            vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v0 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[15U];
            __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v0 = 1U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__round = 0U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__core_busy = 1U;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v1 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[14U];
            __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v1 = 1U;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v2 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[13U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v3 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[12U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v4 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[11U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v5 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[10U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v6 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[9U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v7 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[8U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v8 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[7U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v9 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[6U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v10 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[5U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v11 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[4U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v12 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[3U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v13 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[2U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v14 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[1U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v15 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[0U];
        } else if (vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_busy) {
            vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__g;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v16 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[1U];
            __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v16 = 1U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__g 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__f;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v17 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[2U];
            __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v17 = 1U;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v18 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[3U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v19 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[4U];
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__f 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v20 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[5U];
            __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v20 = 1U;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v21 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[6U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v22 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[7U];
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
                = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__d 
                   + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__t1);
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v23 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[8U];
            __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v23 = 1U;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v24 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[9U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v25 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[10U];
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__d 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__c;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v26 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[11U];
            __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v26 = 1U;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v27 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[12U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v28 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[13U];
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__c 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__b;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v29 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[14U];
            __VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v29 = 1U;
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v30 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[15U];
            __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v31 
                = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[0U] 
                   + ((((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[14U] 
                         << 0x0000000fU) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[14U] 
                                            >> 0x00000011U)) 
                       ^ (((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[14U] 
                            << 0x0000000dU) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[14U] 
                                               >> 0x00000013U)) 
                          ^ (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[14U] 
                             >> 0x0000000aU))) + (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[9U] 
                                                  + 
                                                  (((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[1U] 
                                                     << 0x00000019U) 
                                                    | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[1U] 
                                                       >> 7U)) 
                                                   ^ 
                                                   (((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[1U] 
                                                      << 0x0000000eU) 
                                                     | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[1U] 
                                                        >> 0x00000012U)) 
                                                    ^ 
                                                    (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[1U] 
                                                     >> 3U))))));
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__b 
                = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a;
            if ((0x3fU == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__round))) {
                __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0 
                    = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0 
                       + (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__t1 
                          + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__t2));
                __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1 
                    = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1 
                       + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a);
                __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2 
                    = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2 
                       + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__b);
                __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3 
                    = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3 
                       + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__c);
                __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4 
                    = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4 
                       + (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__d 
                          + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__t1));
                __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5 
                    = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5 
                       + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e);
                __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6 
                    = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6 
                       + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__f);
                __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7 
                    = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7 
                       + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__g);
                __Vdly__a2nix_card__DOT__u_hash__DOT__core_busy = 0U;
            } else {
                __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__round 
                    = (0x0000007fU & ((IData)(1U) + (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__round)));
            }
            __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
                = (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__t1 
                   + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__t2);
        }
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_init = 0U;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_start = 0U;
        if (((1U == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state)) 
             & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__pb_v))) {
            vlSelfRef.a2nix_card__DOT__u_hash__DOT__total 
                = (1ULL + vlSelfRef.a2nix_card__DOT__u_hash__DOT__total);
        }
        if ((4U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))) {
            if ((2U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))) {
                if ((1U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))) {
                    __Vdly__a2nix_card__DOT__h_done = 1U;
                    if (vlSelfRef.a2nix_card__DOT__h_init) {
                        vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_init = 1U;
                        __Vdly__a2nix_card__DOT__u_hash__DOT__bidx = 0U;
                        vlSelfRef.a2nix_card__DOT__u_hash__DOT__total = 0ULL;
                        __Vdly__a2nix_card__DOT__u_hash__DOT__lenidx = 0U;
                        __Vdly__a2nix_card__DOT__h_done = 0U;
                        __Vdly__a2nix_card__DOT__u_hash__DOT__state = 1U;
                    }
                } else if ((7U == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__lenidx))) {
                    __Vdly__a2nix_card__DOT__u_hash__DOT__ret = 7U;
                    __Vdly__a2nix_card__DOT__u_hash__DOT__state = 2U;
                } else {
                    __Vdly__a2nix_card__DOT__u_hash__DOT__lenidx 
                        = (7U & ((IData)(1U) + (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__lenidx)));
                }
            } else if ((1U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))) {
                if ((0x38U == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__bidx))) {
                    __Vdly__a2nix_card__DOT__u_hash__DOT__state = 6U;
                } else if (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk_full) {
                    __Vdly__a2nix_card__DOT__u_hash__DOT__ret = 5U;
                    __Vdly__a2nix_card__DOT__u_hash__DOT__state = 2U;
                }
            } else if (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk_full) {
                __Vdly__a2nix_card__DOT__u_hash__DOT__ret = 5U;
                __Vdly__a2nix_card__DOT__u_hash__DOT__state = 2U;
            } else {
                __Vdly__a2nix_card__DOT__u_hash__DOT__state = 5U;
            }
        } else if ((2U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))) {
            if ((1U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))) {
                if ((1U & (~ (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_busy)))) {
                    __Vdly__a2nix_card__DOT__u_hash__DOT__state 
                        = vlSelfRef.a2nix_card__DOT__u_hash__DOT__ret;
                }
            } else if (vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_busy) {
                __Vdly__a2nix_card__DOT__u_hash__DOT__state = 3U;
            } else {
                vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_start = 1U;
            }
        } else if ((1U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))) {
            if (vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk_full) {
                __Vdly__a2nix_card__DOT__u_hash__DOT__ret = 1U;
                __Vdly__a2nix_card__DOT__u_hash__DOT__state = 2U;
            } else if (((IData)(vlSelfRef.a2nix_card__DOT__h_finish) 
                        & (~ (IData)(vlSelfRef.a2nix_card__DOT__h_valid)))) {
                __Vdly__a2nix_card__DOT__u_hash__DOT__state = 4U;
            }
        } else if (vlSelfRef.a2nix_card__DOT__h_init) {
            vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_init = 1U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__bidx = 0U;
            vlSelfRef.a2nix_card__DOT__u_hash__DOT__total = 0ULL;
            __Vdly__a2nix_card__DOT__u_hash__DOT__lenidx = 0U;
            __Vdly__a2nix_card__DOT__h_done = 0U;
            __Vdly__a2nix_card__DOT__u_hash__DOT__state = 1U;
        }
        vlSelfRef.a2nix_card__DOT__h_valid = 0U;
        vlSelfRef.a2nix_card__DOT__h_init = 0U;
        if (vlSelfRef.a2nix_card__DOT__busy) {
            if (vlSelfRef.a2nix_card__DOT__h_done) {
                vlSelfRef.a2nix_card__DOT__h_finish = 0U;
                __VdlyVal__a2nix_card__DOT__pslot__v0[0U] 
                    = vlSelfRef.a2nix_card__DOT__h_digest[0U];
                __VdlyVal__a2nix_card__DOT__pslot__v0[1U] 
                    = vlSelfRef.a2nix_card__DOT__h_digest[1U];
                __VdlyVal__a2nix_card__DOT__pslot__v0[2U] 
                    = vlSelfRef.a2nix_card__DOT__h_digest[2U];
                __VdlyVal__a2nix_card__DOT__pslot__v0[3U] 
                    = vlSelfRef.a2nix_card__DOT__h_digest[3U];
                __VdlyVal__a2nix_card__DOT__pslot__v0[4U] 
                    = vlSelfRef.a2nix_card__DOT__h_digest[4U];
                __VdlyVal__a2nix_card__DOT__pslot__v0[5U] 
                    = vlSelfRef.a2nix_card__DOT__h_digest[5U];
                __VdlyVal__a2nix_card__DOT__pslot__v0[6U] 
                    = vlSelfRef.a2nix_card__DOT__h_digest[6U];
                __VdlyVal__a2nix_card__DOT__pslot__v0[7U] 
                    = vlSelfRef.a2nix_card__DOT__h_digest[7U];
                __VdlySet__a2nix_card__DOT__pslot__v0 = 1U;
                __Vdly__a2nix_card__DOT__busy = 0U;
                vlSelfRef.a2nix_card__DOT__done = 1U;
                vlSelfRef.a2nix_card__DOT__irq_pend = 1U;
                __VdlyVal__a2nix_card__DOT__pslot__v1[0U] 
                    = vlSelfRef.a2nix_card__DOT__sp_path[0U];
                __VdlyVal__a2nix_card__DOT__pslot__v1[1U] 
                    = vlSelfRef.a2nix_card__DOT__sp_path[1U];
                __VdlyVal__a2nix_card__DOT__pslot__v1[2U] 
                    = vlSelfRef.a2nix_card__DOT__sp_path[2U];
                __VdlyVal__a2nix_card__DOT__pslot__v1[3U] 
                    = vlSelfRef.a2nix_card__DOT__sp_path[3U];
                __VdlyVal__a2nix_card__DOT__pslot__v1[4U] 
                    = vlSelfRef.a2nix_card__DOT__sp_path[4U];
                __VdlyVal__a2nix_card__DOT__pslot__v1[5U] 
                    = vlSelfRef.a2nix_card__DOT__sp_path[5U];
                __VdlyVal__a2nix_card__DOT__pslot__v1[6U] 
                    = vlSelfRef.a2nix_card__DOT__sp_path[6U];
                __VdlyVal__a2nix_card__DOT__pslot__v1[7U] 
                    = vlSelfRef.a2nix_card__DOT__sp_path[7U];
                __VdlySet__a2nix_card__DOT__pslot__v1 = 1U;
            }
        }
        if (vlSelfRef.c0n_write) {
            if ((1U & (~ ((IData)(vlSelfRef.c0n_addr) 
                          >> 3U)))) {
                if ((1U & (~ ((IData)(vlSelfRef.c0n_addr) 
                              >> 2U)))) {
                    if ((1U & (~ ((IData)(vlSelfRef.c0n_addr) 
                                  >> 1U)))) {
                        if ((1U & (IData)(vlSelfRef.c0n_addr))) {
                            vlSelfRef.a2nix_card__DOT__irq_en 
                                = (1U & ((IData)(vlSelfRef.c0n_wdata) 
                                         >> 1U));
                        }
                    }
                }
                if ((4U & (IData)(vlSelfRef.c0n_addr))) {
                    if ((1U & (~ ((IData)(vlSelfRef.c0n_addr) 
                                  >> 1U)))) {
                        if ((1U & (~ (IData)(vlSelfRef.c0n_addr)))) {
                            if (((IData)(vlSelfRef.a2nix_card__DOT__push_mode) 
                                 & (1U == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state)))) {
                                vlSelfRef.a2nix_card__DOT__h_valid = 1U;
                                vlSelfRef.a2nix_card__DOT__h_byte 
                                    = vlSelfRef.c0n_wdata;
                            }
                        }
                    }
                }
            }
            if ((8U & (IData)(vlSelfRef.c0n_addr))) {
                if ((1U & (~ ((IData)(vlSelfRef.c0n_addr) 
                              >> 2U)))) {
                    if ((1U & (~ ((IData)(vlSelfRef.c0n_addr) 
                                  >> 1U)))) {
                        if ((1U & (IData)(vlSelfRef.c0n_addr))) {
                            vlSelfRef.a2nix_card__DOT__irq_pend = 0U;
                        } else {
                            vlSelfRef.a2nix_card__DOT__len 
                                = (((~ ((IData)(0x000000ffU) 
                                        << (0x0000001fU 
                                            & ((IData)(vlSelfRef.a2nix_card__DOT__lenidx) 
                                               << 3U)))) 
                                    & vlSelfRef.a2nix_card__DOT__len) 
                                   | (0x00000000ffffffffULL 
                                      & ((IData)(vlSelfRef.c0n_wdata) 
                                         << (0x0000001fU 
                                             & ((IData)(vlSelfRef.a2nix_card__DOT__lenidx) 
                                                << 3U)))));
                            __Vdly__a2nix_card__DOT__lenidx 
                                = (3U & ((IData)(1U) 
                                         + (IData)(vlSelfRef.a2nix_card__DOT__lenidx)));
                        }
                    }
                }
            } else if ((4U & (IData)(vlSelfRef.c0n_addr))) {
                if ((2U & (IData)(vlSelfRef.c0n_addr))) {
                    if ((1U & (IData)(vlSelfRef.c0n_addr))) {
                        __VdlyVal__a2nix_card__DOT__pslot__v2 
                            = vlSelfRef.c0n_wdata;
                        __VdlyLsb__a2nix_card__DOT__pslot__v2 
                            = (0x000000ffU & (((IData)(0xffU) 
                                               - ((IData)(vlSelfRef.a2nix_card__DOT__pidx) 
                                                  << 3U)) 
                                              - (IData)(7U)));
                        __VdlyDim0__a2nix_card__DOT__pslot__v2 
                            = vlSelfRef.a2nix_card__DOT__psel;
                        __VdlySet__a2nix_card__DOT__pslot__v2 = 1U;
                        __Vdly__a2nix_card__DOT__pidx 
                            = (0x0000001fU & ((IData)(1U) 
                                              + (IData)(vlSelfRef.a2nix_card__DOT__pidx)));
                    } else {
                        __Vdly__a2nix_card__DOT__pidx = 0U;
                    }
                }
            } else if ((2U & (IData)(vlSelfRef.c0n_addr))) {
                if ((1U & (IData)(vlSelfRef.c0n_addr))) {
                    __VdlyVal__a2nix_card__DOT__areg__v0 
                        = vlSelfRef.c0n_wdata;
                    __VdlyLsb__a2nix_card__DOT__areg__v0 
                        = (0x0000001fU & ((IData)(vlSelfRef.a2nix_card__DOT__aidx) 
                                          << 3U));
                    __VdlyDim0__a2nix_card__DOT__areg__v0 
                        = vlSelfRef.a2nix_card__DOT__asel;
                    __VdlySet__a2nix_card__DOT__areg__v0 = 1U;
                    __Vdly__a2nix_card__DOT__aidx = 
                        (3U & ((IData)(1U) + (IData)(vlSelfRef.a2nix_card__DOT__aidx)));
                } else {
                    __Vdly__a2nix_card__DOT__aidx = 0U;
                }
            } else if ((1U & (IData)(vlSelfRef.c0n_addr))) {
                if ((1U & (IData)(vlSelfRef.c0n_wdata))) {
                    vlSelfRef.a2nix_card__DOT__push_mode = 0U;
                    __Vdly__a2nix_card__DOT__busy = 0U;
                    vlSelfRef.a2nix_card__DOT__done = 0U;
                    vlSelfRef.a2nix_card__DOT__err = 0U;
                    vlSelfRef.a2nix_card__DOT__errcode = 0U;
                    vlSelfRef.a2nix_card__DOT__irq_pend = 0U;
                    vlSelfRef.a2nix_card__DOT__h_init = 1U;
                }
            } else if (vlSelfRef.a2nix_card__DOT__busy) {
                vlSelfRef.a2nix_card__DOT__err = 1U;
                vlSelfRef.a2nix_card__DOT__errcode = 5U;
            } else {
                vlSelfRef.a2nix_card__DOT__done = 0U;
                vlSelfRef.a2nix_card__DOT__err = 0U;
                vlSelfRef.a2nix_card__DOT__errcode = 0U;
                if ((0U != (IData)(vlSelfRef.c0n_wdata))) {
                    if ((1U == (IData)(vlSelfRef.c0n_wdata))) {
                        __Vdly__a2nix_card__DOT__aidx = 0U;
                        __Vdly__a2nix_card__DOT__pidx = 0U;
                        __Vdly__a2nix_card__DOT__lenidx = 0U;
                        vlSelfRef.a2nix_card__DOT__push_mode = 0U;
                        vlSelfRef.a2nix_card__DOT__h_init = 1U;
                        vlSelfRef.a2nix_card__DOT__irq_pend = 0U;
                    } else if ((0x10U == (IData)(vlSelfRef.c0n_wdata))) {
                        vlSelfRef.a2nix_card__DOT__h_init = 1U;
                        vlSelfRef.a2nix_card__DOT__push_mode = 0U;
                    } else if ((0x15U == (IData)(vlSelfRef.c0n_wdata))) {
                        vlSelfRef.a2nix_card__DOT__push_mode = 1U;
                    } else if ((0x12U == (IData)(vlSelfRef.c0n_wdata))) {
                        vlSelfRef.a2nix_card__DOT__push_mode = 0U;
                        vlSelfRef.a2nix_card__DOT__h_finish = 1U;
                        __Vdly__a2nix_card__DOT__busy = 1U;
                    } else if ((0x14U == (IData)(vlSelfRef.c0n_wdata))) {
                        __VdlyVal__a2nix_card__DOT__pslot__v3[0U] 
                            = vlSelfRef.a2nix_card__DOT__sp_path[0U];
                        __VdlyVal__a2nix_card__DOT__pslot__v3[1U] 
                            = vlSelfRef.a2nix_card__DOT__sp_path[1U];
                        __VdlyVal__a2nix_card__DOT__pslot__v3[2U] 
                            = vlSelfRef.a2nix_card__DOT__sp_path[2U];
                        __VdlyVal__a2nix_card__DOT__pslot__v3[3U] 
                            = vlSelfRef.a2nix_card__DOT__sp_path[3U];
                        __VdlyVal__a2nix_card__DOT__pslot__v3[4U] 
                            = vlSelfRef.a2nix_card__DOT__sp_path[4U];
                        __VdlyVal__a2nix_card__DOT__pslot__v3[5U] 
                            = vlSelfRef.a2nix_card__DOT__sp_path[5U];
                        __VdlyVal__a2nix_card__DOT__pslot__v3[6U] 
                            = vlSelfRef.a2nix_card__DOT__sp_path[6U];
                        __VdlyVal__a2nix_card__DOT__pslot__v3[7U] 
                            = vlSelfRef.a2nix_card__DOT__sp_path[7U];
                        __VdlySet__a2nix_card__DOT__pslot__v3 = 1U;
                        vlSelfRef.a2nix_card__DOT__done = 1U;
                    } else {
                        vlSelfRef.a2nix_card__DOT__err = 1U;
                        vlSelfRef.a2nix_card__DOT__errcode = 1U;
                    }
                }
            }
            if ((1U & (~ ((IData)(vlSelfRef.c0n_addr) 
                          >> 3U)))) {
                if ((4U & (IData)(vlSelfRef.c0n_addr))) {
                    if ((2U & (IData)(vlSelfRef.c0n_addr))) {
                        if ((1U & (~ (IData)(vlSelfRef.c0n_addr)))) {
                            vlSelfRef.a2nix_card__DOT__psel 
                                = (7U & (IData)(vlSelfRef.c0n_wdata));
                        }
                    }
                }
                if ((1U & (~ ((IData)(vlSelfRef.c0n_addr) 
                              >> 2U)))) {
                    if ((2U & (IData)(vlSelfRef.c0n_addr))) {
                        if ((1U & (~ (IData)(vlSelfRef.c0n_addr)))) {
                            vlSelfRef.a2nix_card__DOT__asel 
                                = (3U & (IData)(vlSelfRef.c0n_wdata));
                        }
                    }
                }
            }
        }
        if (vlSelfRef.c0n_read) {
            if ((3U == (IData)(vlSelfRef.c0n_addr))) {
                __Vdly__a2nix_card__DOT__aidx = (3U 
                                                 & ((IData)(1U) 
                                                    + (IData)(vlSelfRef.a2nix_card__DOT__aidx)));
            } else if ((7U == (IData)(vlSelfRef.c0n_addr))) {
                __Vdly__a2nix_card__DOT__pidx = (0x0000001fU 
                                                 & ((IData)(1U) 
                                                    + (IData)(vlSelfRef.a2nix_card__DOT__pidx)));
            } else if ((8U == (IData)(vlSelfRef.c0n_addr))) {
                __Vdly__a2nix_card__DOT__lenidx = (3U 
                                                   & ((IData)(1U) 
                                                      + (IData)(vlSelfRef.a2nix_card__DOT__lenidx)));
            }
        }
    } else {
        VL_ASSIGN_W(512, __Vdly__a2nix_card__DOT__u_hash__DOT__blk, Va2nix_card__ConstPool__CONST_h93e1b771_0);
        vlSelfRef.a2nix_card__DOT__irq_en = 0U;
        __Vdly__a2nix_card__DOT__u_hash__DOT__core_busy = 0U;
        __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__round = 0U;
        __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0 = 0x6a09e667U;
        __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1 = 0xbb67ae85U;
        __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2 = 0x3c6ef372U;
        __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3 = 0xa54ff53aU;
        __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4 = 0x510e527fU;
        __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5 = 0x9b05688cU;
        __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6 = 0x1f83d9abU;
        __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7 = 0x5be0cd19U;
        __Vdly__a2nix_card__DOT__u_hash__DOT__state = 0U;
        __Vdly__a2nix_card__DOT__u_hash__DOT__ret = 0U;
        __Vdly__a2nix_card__DOT__u_hash__DOT__bidx = 0U;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__total = 0ULL;
        __Vdly__a2nix_card__DOT__u_hash__DOT__lenidx = 0U;
        __Vdly__a2nix_card__DOT__h_done = 0U;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_init = 0U;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_start = 0U;
        vlSelfRef.a2nix_card__DOT__h_valid = 0U;
        vlSelfRef.a2nix_card__DOT__h_byte = 0U;
        __Vdly__a2nix_card__DOT__aidx = 0U;
        __Vdly__a2nix_card__DOT__pidx = 0U;
        __Vdly__a2nix_card__DOT__lenidx = 0U;
        vlSelfRef.a2nix_card__DOT__len = 0U;
        __Vdly__a2nix_card__DOT__busy = 0U;
        vlSelfRef.a2nix_card__DOT__done = 0U;
        vlSelfRef.a2nix_card__DOT__err = 0U;
        vlSelfRef.a2nix_card__DOT__errcode = 0U;
        vlSelfRef.a2nix_card__DOT__irq_pend = 0U;
        vlSelfRef.a2nix_card__DOT__push_mode = 0U;
        vlSelfRef.a2nix_card__DOT__h_init = 0U;
        vlSelfRef.a2nix_card__DOT__h_finish = 0U;
        __VdlySet__a2nix_card__DOT__areg__v1 = 1U;
        vlSelfRef.a2nix_card__DOT__psel = 0U;
        vlSelfRef.a2nix_card__DOT__asel = 0U;
    }
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__d 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__d;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[0U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[0U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[1U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[1U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[2U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[2U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[3U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[3U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[4U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[4U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[5U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[5U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[6U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[6U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[7U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[7U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[8U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[8U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[9U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[9U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[10U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[10U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[11U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[11U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[12U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[12U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[13U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[13U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[14U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[14U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk[15U] 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__blk[15U];
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__a;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__b 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__b;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__c 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__c;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__round 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__round;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__e;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__f 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__f;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__g 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__g;
    if (__VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v0) {
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[0U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v0;
    }
    if (__VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v1) {
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[1U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v1;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[2U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v2;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[3U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v3;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[4U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v4;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[5U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v5;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[6U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v6;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[7U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v7;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[8U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v8;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[9U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v9;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[10U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v10;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[11U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v11;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[12U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v12;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[13U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v13;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[14U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v14;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[15U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v15;
    }
    if (__VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v16) {
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[0U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v16;
    }
    if (__VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v17) {
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[1U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v17;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[2U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v18;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[3U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v19;
    }
    if (__VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v20) {
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[4U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v20;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[5U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v21;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[6U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v22;
    }
    if (__VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v23) {
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[7U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v23;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[8U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v24;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[9U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v25;
    }
    if (__VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v26) {
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[10U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v26;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[11U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v27;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[12U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v28;
    }
    if (__VdlySet__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v29) {
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[13U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v29;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[14U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v30;
        vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[15U] 
            = __VdlyVal__a2nix_card__DOT__u_hash__DOT__u_core__DOT__w__v31;
    }
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__t2 
        = (((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
             & vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__b) 
            ^ ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
                & vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__c) 
               ^ (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__b 
                  & vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__c))) 
           + (((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
                << 0x0000001eU) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
                                   >> 2U)) ^ (((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
                                                << 0x00000013U) 
                                               | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
                                                  >> 0x0000000dU)) 
                                              ^ ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
                                                  << 0x0000000aU) 
                                                 | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__a 
                                                    >> 0x00000016U)))));
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__t1 
        = ((((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
              << 0x0000001aU) | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
                                 >> 6U)) ^ (((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
                                              << 0x00000015U) 
                                             | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
                                                >> 0x0000000bU)) 
                                            ^ ((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
                                                << 7U) 
                                               | (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
                                                  >> 0x00000019U)))) 
           + (((vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e 
                & vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__f) 
               ^ ((~ vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__e) 
                  & vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__g)) 
              + (vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h 
                 + (Va2nix_card__ConstPool__CONST_h7be248c9_0
                    [(0x0000003fU & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__round))] 
                    + vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[0U]))));
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__lenidx 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__lenidx;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__ret = __Vdly__a2nix_card__DOT__u_hash__DOT__ret;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__core_busy 
        = __Vdly__a2nix_card__DOT__u_hash__DOT__core_busy;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__bidx = __Vdly__a2nix_card__DOT__u_hash__DOT__bidx;
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__state = __Vdly__a2nix_card__DOT__u_hash__DOT__state;
    vlSelfRef.__VdfgRegularize_h6e95ff9d_0_13 = (IData)(
                                                        (1U 
                                                         == 
                                                         (3U 
                                                          & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))));
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__pb_v = 
        (1U & ((4U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))
                ? ((2U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))
                    ? (~ (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))
                    : (~ ((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state) 
                          & (0x38U == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__bidx)))))
                : ((IData)(vlSelfRef.a2nix_card__DOT__h_valid) 
                   & (IData)(vlSelfRef.__VdfgRegularize_h6e95ff9d_0_13))));
    vlSelfRef.a2nix_card__DOT__h_done = __Vdly__a2nix_card__DOT__h_done;
    vlSelfRef.a2nix_card__DOT__lenidx = __Vdly__a2nix_card__DOT__lenidx;
    vlSelfRef.a2nix_card__DOT__pidx = __Vdly__a2nix_card__DOT__pidx;
    vlSelfRef.a2nix_card__DOT__aidx = __Vdly__a2nix_card__DOT__aidx;
    vlSelfRef.a2nix_card__DOT__h_digest[0U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7;
    vlSelfRef.a2nix_card__DOT__h_digest[1U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6;
    vlSelfRef.a2nix_card__DOT__h_digest[2U] = (IData)(
                                                      (((QData)((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4)) 
                                                        << 0x00000020U) 
                                                       | (QData)((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5))));
    vlSelfRef.a2nix_card__DOT__h_digest[3U] = (IData)(
                                                      ((((QData)((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4)) 
                                                         << 0x00000020U) 
                                                        | (QData)((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5))) 
                                                       >> 0x00000020U));
    vlSelfRef.a2nix_card__DOT__h_digest[4U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3;
    vlSelfRef.a2nix_card__DOT__h_digest[5U] = vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2;
    vlSelfRef.a2nix_card__DOT__h_digest[6U] = (IData)(
                                                      (((QData)((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0)) 
                                                        << 0x00000020U) 
                                                       | (QData)((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1))));
    vlSelfRef.a2nix_card__DOT__h_digest[7U] = (IData)(
                                                      ((((QData)((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0)) 
                                                         << 0x00000020U) 
                                                        | (QData)((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1))) 
                                                       >> 0x00000020U));
    vlSelfRef.a2nix_card__DOT__busy = __Vdly__a2nix_card__DOT__busy;
    if (__VdlySet__a2nix_card__DOT__areg__v0) {
        vlSelfRef.a2nix_card__DOT__areg[__VdlyDim0__a2nix_card__DOT__areg__v0] 
            = (((~ ((IData)(0x000000ffU) << (IData)(__VdlyLsb__a2nix_card__DOT__areg__v0))) 
                & vlSelfRef.a2nix_card__DOT__areg[__VdlyDim0__a2nix_card__DOT__areg__v0]) 
               | (0x00000000ffffffffULL & ((IData)(__VdlyVal__a2nix_card__DOT__areg__v0) 
                                           << (IData)(__VdlyLsb__a2nix_card__DOT__areg__v0))));
    }
    if (__VdlySet__a2nix_card__DOT__pslot__v0) {
        vlSelfRef.a2nix_card__DOT__pslot[0U][0U] = __VdlyVal__a2nix_card__DOT__pslot__v0[0U];
        vlSelfRef.a2nix_card__DOT__pslot[0U][1U] = __VdlyVal__a2nix_card__DOT__pslot__v0[1U];
        vlSelfRef.a2nix_card__DOT__pslot[0U][2U] = __VdlyVal__a2nix_card__DOT__pslot__v0[2U];
        vlSelfRef.a2nix_card__DOT__pslot[0U][3U] = __VdlyVal__a2nix_card__DOT__pslot__v0[3U];
        vlSelfRef.a2nix_card__DOT__pslot[0U][4U] = __VdlyVal__a2nix_card__DOT__pslot__v0[4U];
        vlSelfRef.a2nix_card__DOT__pslot[0U][5U] = __VdlyVal__a2nix_card__DOT__pslot__v0[5U];
        vlSelfRef.a2nix_card__DOT__pslot[0U][6U] = __VdlyVal__a2nix_card__DOT__pslot__v0[6U];
        vlSelfRef.a2nix_card__DOT__pslot[0U][7U] = __VdlyVal__a2nix_card__DOT__pslot__v0[7U];
    }
    if (__VdlySet__a2nix_card__DOT__pslot__v1) {
        vlSelfRef.a2nix_card__DOT__pslot[3U][0U] = __VdlyVal__a2nix_card__DOT__pslot__v1[0U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][1U] = __VdlyVal__a2nix_card__DOT__pslot__v1[1U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][2U] = __VdlyVal__a2nix_card__DOT__pslot__v1[2U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][3U] = __VdlyVal__a2nix_card__DOT__pslot__v1[3U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][4U] = __VdlyVal__a2nix_card__DOT__pslot__v1[4U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][5U] = __VdlyVal__a2nix_card__DOT__pslot__v1[5U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][6U] = __VdlyVal__a2nix_card__DOT__pslot__v1[6U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][7U] = __VdlyVal__a2nix_card__DOT__pslot__v1[7U];
    }
    if (__VdlySet__a2nix_card__DOT__pslot__v2) {
        VL_ASSIGNSEL_WI(256, 8, (IData)(__VdlyLsb__a2nix_card__DOT__pslot__v2), vlSelfRef.a2nix_card__DOT__pslot
                        [__VdlyDim0__a2nix_card__DOT__pslot__v2], __VdlyVal__a2nix_card__DOT__pslot__v2);
    }
    if (__VdlySet__a2nix_card__DOT__pslot__v3) {
        vlSelfRef.a2nix_card__DOT__pslot[3U][0U] = __VdlyVal__a2nix_card__DOT__pslot__v3[0U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][1U] = __VdlyVal__a2nix_card__DOT__pslot__v3[1U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][2U] = __VdlyVal__a2nix_card__DOT__pslot__v3[2U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][3U] = __VdlyVal__a2nix_card__DOT__pslot__v3[3U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][4U] = __VdlyVal__a2nix_card__DOT__pslot__v3[4U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][5U] = __VdlyVal__a2nix_card__DOT__pslot__v3[5U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][6U] = __VdlyVal__a2nix_card__DOT__pslot__v3[6U];
        vlSelfRef.a2nix_card__DOT__pslot[3U][7U] = __VdlyVal__a2nix_card__DOT__pslot__v3[7U];
    }
    if (__VdlySet__a2nix_card__DOT__areg__v1) {
        vlSelfRef.a2nix_card__DOT__areg[0U] = 0U;
        vlSelfRef.a2nix_card__DOT__areg[1U] = 0U;
        vlSelfRef.a2nix_card__DOT__areg[2U] = 0U;
        vlSelfRef.a2nix_card__DOT__areg[3U] = 0U;
        VL_ASSIGN_W(256, vlSelfRef.a2nix_card__DOT__pslot[0U], Va2nix_card__ConstPool__CONST_h9e67c271_0);
        VL_ASSIGN_W(256, vlSelfRef.a2nix_card__DOT__pslot[1U], Va2nix_card__ConstPool__CONST_h9e67c271_0);
        VL_ASSIGN_W(256, vlSelfRef.a2nix_card__DOT__pslot[2U], Va2nix_card__ConstPool__CONST_h9e67c271_0);
        VL_ASSIGN_W(256, vlSelfRef.a2nix_card__DOT__pslot[3U], Va2nix_card__ConstPool__CONST_h9e67c271_0);
        VL_ASSIGN_W(256, vlSelfRef.a2nix_card__DOT__pslot[4U], Va2nix_card__ConstPool__CONST_h9e67c271_0);
        VL_ASSIGN_W(256, vlSelfRef.a2nix_card__DOT__pslot[5U], Va2nix_card__ConstPool__CONST_h9e67c271_0);
        VL_ASSIGN_W(256, vlSelfRef.a2nix_card__DOT__pslot[6U], Va2nix_card__ConstPool__CONST_h9e67c271_0);
        VL_ASSIGN_W(256, vlSelfRef.a2nix_card__DOT__pslot[7U], Va2nix_card__ConstPool__CONST_h9e67c271_0);
    }
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk_full 
        = ((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__pb_v) 
           & (0x3fU == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__bidx)));
    vlSelfRef.irq = ((IData)(vlSelfRef.a2nix_card__DOT__irq_pend) 
                     & (IData)(vlSelfRef.a2nix_card__DOT__irq_en));
    if (vlSelfRef.a2nix_card__DOT__busy) {
        a2nix_card__DOT__sp_digest[0U] = vlSelfRef.a2nix_card__DOT__h_digest[0U];
        a2nix_card__DOT__sp_digest[1U] = vlSelfRef.a2nix_card__DOT__h_digest[1U];
        a2nix_card__DOT__sp_digest[2U] = vlSelfRef.a2nix_card__DOT__h_digest[2U];
        a2nix_card__DOT__sp_digest[3U] = vlSelfRef.a2nix_card__DOT__h_digest[3U];
        a2nix_card__DOT__sp_digest[4U] = vlSelfRef.a2nix_card__DOT__h_digest[4U];
        a2nix_card__DOT__sp_digest[5U] = vlSelfRef.a2nix_card__DOT__h_digest[5U];
        a2nix_card__DOT__sp_digest[6U] = vlSelfRef.a2nix_card__DOT__h_digest[6U];
        a2nix_card__DOT__sp_digest[7U] = vlSelfRef.a2nix_card__DOT__h_digest[7U];
    } else {
        a2nix_card__DOT__sp_digest[0U] = vlSelfRef.a2nix_card__DOT__pslot[0U][0U];
        a2nix_card__DOT__sp_digest[1U] = vlSelfRef.a2nix_card__DOT__pslot[0U][1U];
        a2nix_card__DOT__sp_digest[2U] = vlSelfRef.a2nix_card__DOT__pslot[0U][2U];
        a2nix_card__DOT__sp_digest[3U] = vlSelfRef.a2nix_card__DOT__pslot[0U][3U];
        a2nix_card__DOT__sp_digest[4U] = vlSelfRef.a2nix_card__DOT__pslot[0U][4U];
        a2nix_card__DOT__sp_digest[5U] = vlSelfRef.a2nix_card__DOT__pslot[0U][5U];
        a2nix_card__DOT__sp_digest[6U] = vlSelfRef.a2nix_card__DOT__pslot[0U][6U];
        a2nix_card__DOT__sp_digest[7U] = vlSelfRef.a2nix_card__DOT__pslot[0U][7U];
    }
    __VdfgRegularize_h6e95ff9d_0_0 = (0x000000ffU & 
                                      (a2nix_card__DOT__sp_digest[5U] 
                                       ^ a2nix_card__DOT__sp_digest[0U]));
    __VdfgRegularize_h6e95ff9d_0_1 = (0x000000ffU & 
                                      (((a2nix_card__DOT__sp_digest[5U] 
                                         << 8U) | (a2nix_card__DOT__sp_digest[5U] 
                                                   >> 0x00000018U)) 
                                       ^ ((a2nix_card__DOT__sp_digest[0U] 
                                           << 8U) | 
                                          (a2nix_card__DOT__sp_digest[0U] 
                                           >> 0x00000018U))));
    __VdfgRegularize_h6e95ff9d_0_2 = (0x000000ffU & 
                                      (((a2nix_card__DOT__sp_digest[6U] 
                                         << 0x00000018U) 
                                        | (a2nix_card__DOT__sp_digest[6U] 
                                           >> 8U)) 
                                       ^ ((a2nix_card__DOT__sp_digest[1U] 
                                           << 0x00000018U) 
                                          | (a2nix_card__DOT__sp_digest[1U] 
                                             >> 8U))));
    __VdfgRegularize_h6e95ff9d_0_3 = (0x000000ffU & 
                                      (a2nix_card__DOT__sp_digest[7U] 
                                       ^ a2nix_card__DOT__sp_digest[2U]));
    __VdfgRegularize_h6e95ff9d_0_4 = (0x000000ffU & 
                                      (((a2nix_card__DOT__sp_digest[7U] 
                                         << 0x00000010U) 
                                        | (a2nix_card__DOT__sp_digest[7U] 
                                           >> 0x00000010U)) 
                                       ^ ((a2nix_card__DOT__sp_digest[2U] 
                                           << 0x00000010U) 
                                          | (a2nix_card__DOT__sp_digest[2U] 
                                             >> 0x00000010U))));
    __VdfgRegularize_h6e95ff9d_0_5 = (0x000000ffU & 
                                      (((a2nix_card__DOT__sp_digest[5U] 
                                         << 0x00000018U) 
                                        | (a2nix_card__DOT__sp_digest[5U] 
                                           >> 8U)) 
                                       ^ ((a2nix_card__DOT__sp_digest[0U] 
                                           << 0x00000018U) 
                                          | (a2nix_card__DOT__sp_digest[0U] 
                                             >> 8U))));
    __VdfgRegularize_h6e95ff9d_0_6 = (0x000000ffU & 
                                      (((a2nix_card__DOT__sp_digest[5U] 
                                         << 0x00000010U) 
                                        | (a2nix_card__DOT__sp_digest[5U] 
                                           >> 0x00000010U)) 
                                       ^ ((a2nix_card__DOT__sp_digest[0U] 
                                           << 0x00000010U) 
                                          | (a2nix_card__DOT__sp_digest[0U] 
                                             >> 0x00000010U))));
    __VdfgRegularize_h6e95ff9d_0_7 = (0x000000ffU & 
                                      (a2nix_card__DOT__sp_digest[6U] 
                                       ^ a2nix_card__DOT__sp_digest[1U]));
    __VdfgRegularize_h6e95ff9d_0_8 = (0x000000ffU & 
                                      (((a2nix_card__DOT__sp_digest[6U] 
                                         << 0x00000010U) 
                                        | (a2nix_card__DOT__sp_digest[6U] 
                                           >> 0x00000010U)) 
                                       ^ ((a2nix_card__DOT__sp_digest[1U] 
                                           << 0x00000010U) 
                                          | (a2nix_card__DOT__sp_digest[1U] 
                                             >> 0x00000010U))));
    __VdfgRegularize_h6e95ff9d_0_9 = (0x000000ffU & 
                                      (((a2nix_card__DOT__sp_digest[6U] 
                                         << 8U) | (a2nix_card__DOT__sp_digest[6U] 
                                                   >> 0x00000018U)) 
                                       ^ ((a2nix_card__DOT__sp_digest[1U] 
                                           << 8U) | 
                                          (a2nix_card__DOT__sp_digest[1U] 
                                           >> 0x00000018U))));
    __VdfgRegularize_h6e95ff9d_0_10 = (0x000000ffU 
                                       & (((a2nix_card__DOT__sp_digest[7U] 
                                            << 0x00000018U) 
                                           | (a2nix_card__DOT__sp_digest[7U] 
                                              >> 8U)) 
                                          ^ ((a2nix_card__DOT__sp_digest[2U] 
                                              << 0x00000018U) 
                                             | (a2nix_card__DOT__sp_digest[2U] 
                                                >> 8U))));
    __VdfgRegularize_h6e95ff9d_0_11 = (0x000000ffU 
                                       & (((a2nix_card__DOT__sp_digest[7U] 
                                            << 8U) 
                                           | (a2nix_card__DOT__sp_digest[7U] 
                                              >> 0x00000018U)) 
                                          ^ ((a2nix_card__DOT__sp_digest[2U] 
                                              << 8U) 
                                             | (a2nix_card__DOT__sp_digest[2U] 
                                                >> 0x00000018U))));
    __VExpandSel_Lsb_1 = (0x000000ffU & (((IData)(0xffU) 
                                          - ((IData)(vlSelfRef.a2nix_card__DOT__pidx) 
                                             << 3U)) 
                                         - (IData)(7U)));
    vlSelfRef.c0n_rdata = (0x000000ffU & ((8U & (IData)(vlSelfRef.c0n_addr))
                                           ? ((- (IData)(
                                                         (1U 
                                                          & (~ 
                                                             ((IData)(vlSelfRef.c0n_addr) 
                                                              >> 1U))))) 
                                              & (((1U 
                                                   & (IData)(vlSelfRef.c0n_addr))
                                                   ? (IData)(vlSelfRef.a2nix_card__DOT__irq_pend)
                                                   : 
                                                  (vlSelfRef.a2nix_card__DOT__len 
                                                   >> 
                                                   ((IData)(vlSelfRef.a2nix_card__DOT__lenidx) 
                                                    << 3U))) 
                                                 & (- (IData)(
                                                              (1U 
                                                               & (~ 
                                                                  ((IData)(vlSelfRef.c0n_addr) 
                                                                   >> 2U)))))))
                                           : ((4U & (IData)(vlSelfRef.c0n_addr))
                                               ? ((3U 
                                                   == 
                                                   (3U 
                                                    & (IData)(vlSelfRef.c0n_addr)))
                                                   ? 
                                                  (((0U 
                                                     == 
                                                     (0x0000001fU 
                                                      & __VExpandSel_Lsb_1))
                                                     ? 0U
                                                     : 
                                                    (vlSelfRef.a2nix_card__DOT__pslot
                                                     [vlSelfRef.a2nix_card__DOT__psel]
                                                     [
                                                     (((IData)(7U) 
                                                       + __VExpandSel_Lsb_1) 
                                                      >> 5U)] 
                                                     << 
                                                     ((IData)(0x00000020U) 
                                                      - 
                                                      (0x0000001fU 
                                                       & __VExpandSel_Lsb_1)))) 
                                                   | (vlSelfRef.a2nix_card__DOT__pslot
                                                      [vlSelfRef.a2nix_card__DOT__psel]
                                                      [
                                                      (__VExpandSel_Lsb_1 
                                                       >> 5U)] 
                                                      >> 
                                                      (0x0000001fU 
                                                       & __VExpandSel_Lsb_1)))
                                                   : 0U)
                                               : ((2U 
                                                   & (IData)(vlSelfRef.c0n_addr))
                                                   ? 
                                                  ((1U 
                                                    & (IData)(vlSelfRef.c0n_addr))
                                                    ? 
                                                   (vlSelfRef.a2nix_card__DOT__areg
                                                    [vlSelfRef.a2nix_card__DOT__asel] 
                                                    >> 
                                                    ((IData)(vlSelfRef.a2nix_card__DOT__aidx) 
                                                     << 3U))
                                                    : 0U)
                                                   : 
                                                  ((1U 
                                                    & (IData)(vlSelfRef.c0n_addr))
                                                    ? 0xa2U
                                                    : 
                                                   ((((IData)(vlSelfRef.a2nix_card__DOT__busy) 
                                                      << 7U) 
                                                     | ((IData)(vlSelfRef.a2nix_card__DOT__done) 
                                                        << 6U)) 
                                                    | (((IData)(vlSelfRef.a2nix_card__DOT__err) 
                                                        << 5U) 
                                                       | ((((~ (IData)(vlSelfRef.a2nix_card__DOT__h_valid)) 
                                                            & (1U 
                                                               == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))) 
                                                           << 4U) 
                                                          | (IData)(vlSelfRef.a2nix_card__DOT__errcode)))))))));
    __VExpandSel_Lsb_2 = ((0x000003c0U & ((IData)(__VdfgRegularize_h6e95ff9d_0_10) 
                                          << 6U)) | 
                          (0x00000020U & ((IData)(__VdfgRegularize_h6e95ff9d_0_4) 
                                          >> 2U)));
    __VExpandSel_Lsb_3 = ((0x00000300U & ((IData)(__VdfgRegularize_h6e95ff9d_0_4) 
                                          << 8U)) | 
                          (0x000000e0U & (IData)(__VdfgRegularize_h6e95ff9d_0_11)));
    vlSelfRef.a2nix_card__DOT__sp_path[0U] = ((((0x0000ff00U 
                                                 & ((((0U 
                                                       == 
                                                       (0x0000001fU 
                                                        & __VExpandSel_Lsb_2))
                                                       ? 0U
                                                       : 
                                                      (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                       [
                                                       (((IData)(7U) 
                                                         + __VExpandSel_Lsb_2) 
                                                        >> 5U)] 
                                                       << 
                                                       ((IData)(0x00000020U) 
                                                        - 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_2)))) 
                                                     | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (__VExpandSel_Lsb_2 
                                                         >> 5U)] 
                                                        >> 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_2))) 
                                                    << 8U)) 
                                                | (0x000000ffU 
                                                   & Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                   [
                                                   (0x0000001fU 
                                                    & ((IData)(__VdfgRegularize_h6e95ff9d_0_4) 
                                                       >> 2U))])) 
                                               << 0x00000010U) 
                                              | ((0x0000ff00U 
                                                  & ((((0U 
                                                        == 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_3))
                                                        ? 0U
                                                        : 
                                                       (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (((IData)(7U) 
                                                          + __VExpandSel_Lsb_3) 
                                                         >> 5U)] 
                                                        << 
                                                        ((IData)(0x00000020U) 
                                                         - 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_3)))) 
                                                      | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (__VExpandSel_Lsb_3 
                                                          >> 5U)] 
                                                         >> 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_3))) 
                                                     << 8U)) 
                                                 | (0x000000ffU 
                                                    & Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                    [
                                                    (0x0000001fU 
                                                     & (IData)(__VdfgRegularize_h6e95ff9d_0_11))])));
    __VExpandSel_Lsb_4 = ((0x00000380U & ((IData)(__VdfgRegularize_h6e95ff9d_0_9) 
                                          << 7U)) | 
                          (0x00000060U & ((IData)(__VdfgRegularize_h6e95ff9d_0_3) 
                                          >> 1U)));
    __VExpandSel_Lsb_5 = ((0x00000200U & ((IData)(__VdfgRegularize_h6e95ff9d_0_3) 
                                          << 9U)) | 
                          (0x000001e0U & ((IData)(__VdfgRegularize_h6e95ff9d_0_10) 
                                          << 1U)));
    vlSelfRef.a2nix_card__DOT__sp_path[1U] = ((((0x0000ff00U 
                                                 & (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                    [
                                                    (0x0000001fU 
                                                     & ((IData)(__VdfgRegularize_h6e95ff9d_0_9) 
                                                        >> 3U))] 
                                                    << 8U)) 
                                                | (0x000000ffU 
                                                   & (((0U 
                                                        == 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_4))
                                                        ? 0U
                                                        : 
                                                       (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (((IData)(7U) 
                                                          + __VExpandSel_Lsb_4) 
                                                         >> 5U)] 
                                                        << 
                                                        ((IData)(0x00000020U) 
                                                         - 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_4)))) 
                                                      | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (__VExpandSel_Lsb_4 
                                                          >> 5U)] 
                                                         >> 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_4))))) 
                                               << 0x00000010U) 
                                              | ((0x0000ff00U 
                                                  & (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                     [
                                                     (0x0000001fU 
                                                      & ((IData)(__VdfgRegularize_h6e95ff9d_0_3) 
                                                         >> 1U))] 
                                                     << 8U)) 
                                                 | (0x000000ffU 
                                                    & (((0U 
                                                         == 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_5))
                                                         ? 0U
                                                         : 
                                                        (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (((IData)(7U) 
                                                           + __VExpandSel_Lsb_5) 
                                                          >> 5U)] 
                                                         << 
                                                         ((IData)(0x00000020U) 
                                                          - 
                                                          (0x0000001fU 
                                                           & __VExpandSel_Lsb_5)))) 
                                                       | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                          [
                                                          (__VExpandSel_Lsb_5 
                                                           >> 5U)] 
                                                          >> 
                                                          (0x0000001fU 
                                                           & __VExpandSel_Lsb_5))))));
    __VExpandSel_Lsb_6 = ((0x000003c0U & ((IData)(__VdfgRegularize_h6e95ff9d_0_7) 
                                          << 6U)) | 
                          (0x00000020U & ((IData)(__VdfgRegularize_h6e95ff9d_0_2) 
                                          >> 2U)));
    __VExpandSel_Lsb_7 = ((0x00000300U & ((IData)(__VdfgRegularize_h6e95ff9d_0_2) 
                                          << 8U)) | 
                          (0x000000e0U & (IData)(__VdfgRegularize_h6e95ff9d_0_8)));
    vlSelfRef.a2nix_card__DOT__sp_path[2U] = ((((0x0000ff00U 
                                                 & ((((0U 
                                                       == 
                                                       (0x0000001fU 
                                                        & __VExpandSel_Lsb_6))
                                                       ? 0U
                                                       : 
                                                      (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                       [
                                                       (((IData)(7U) 
                                                         + __VExpandSel_Lsb_6) 
                                                        >> 5U)] 
                                                       << 
                                                       ((IData)(0x00000020U) 
                                                        - 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_6)))) 
                                                     | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (__VExpandSel_Lsb_6 
                                                         >> 5U)] 
                                                        >> 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_6))) 
                                                    << 8U)) 
                                                | (0x000000ffU 
                                                   & Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                   [
                                                   (0x0000001fU 
                                                    & ((IData)(__VdfgRegularize_h6e95ff9d_0_2) 
                                                       >> 2U))])) 
                                               << 0x00000010U) 
                                              | ((0x0000ff00U 
                                                  & ((((0U 
                                                        == 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_7))
                                                        ? 0U
                                                        : 
                                                       (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (((IData)(7U) 
                                                          + __VExpandSel_Lsb_7) 
                                                         >> 5U)] 
                                                        << 
                                                        ((IData)(0x00000020U) 
                                                         - 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_7)))) 
                                                      | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (__VExpandSel_Lsb_7 
                                                          >> 5U)] 
                                                         >> 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_7))) 
                                                     << 8U)) 
                                                 | (0x000000ffU 
                                                    & Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                    [
                                                    (0x0000001fU 
                                                     & (IData)(__VdfgRegularize_h6e95ff9d_0_8))])));
    __VExpandSel_Lsb_8 = ((0x00000380U & ((IData)(__VdfgRegularize_h6e95ff9d_0_6) 
                                          << 7U)) | 
                          (0x00000060U & ((IData)(__VdfgRegularize_h6e95ff9d_0_1) 
                                          >> 1U)));
    __VExpandSel_Lsb_9 = ((0x00000200U & ((IData)(__VdfgRegularize_h6e95ff9d_0_1) 
                                          << 9U)) | 
                          (0x000001e0U & ((IData)(__VdfgRegularize_h6e95ff9d_0_7) 
                                          << 1U)));
    vlSelfRef.a2nix_card__DOT__sp_path[3U] = ((((0x0000ff00U 
                                                 & (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                    [
                                                    (0x0000001fU 
                                                     & ((IData)(__VdfgRegularize_h6e95ff9d_0_6) 
                                                        >> 3U))] 
                                                    << 8U)) 
                                                | (0x000000ffU 
                                                   & (((0U 
                                                        == 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_8))
                                                        ? 0U
                                                        : 
                                                       (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (((IData)(7U) 
                                                          + __VExpandSel_Lsb_8) 
                                                         >> 5U)] 
                                                        << 
                                                        ((IData)(0x00000020U) 
                                                         - 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_8)))) 
                                                      | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (__VExpandSel_Lsb_8 
                                                          >> 5U)] 
                                                         >> 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_8))))) 
                                               << 0x00000010U) 
                                              | ((0x0000ff00U 
                                                  & (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                     [
                                                     (0x0000001fU 
                                                      & ((IData)(__VdfgRegularize_h6e95ff9d_0_1) 
                                                         >> 1U))] 
                                                     << 8U)) 
                                                 | (0x000000ffU 
                                                    & (((0U 
                                                         == 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_9))
                                                         ? 0U
                                                         : 
                                                        (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (((IData)(7U) 
                                                           + __VExpandSel_Lsb_9) 
                                                          >> 5U)] 
                                                         << 
                                                         ((IData)(0x00000020U) 
                                                          - 
                                                          (0x0000001fU 
                                                           & __VExpandSel_Lsb_9)))) 
                                                       | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                          [
                                                          (__VExpandSel_Lsb_9 
                                                           >> 5U)] 
                                                          >> 
                                                          (0x0000001fU 
                                                           & __VExpandSel_Lsb_9))))));
    __VExpandSel_Lsb_10 = ((0x000003c0U & (a2nix_card__DOT__sp_digest[4U] 
                                           >> 0x00000012U)) 
                           | (0x00000020U & ((IData)(__VdfgRegularize_h6e95ff9d_0_0) 
                                             >> 2U)));
    __VExpandSel_Lsb_11 = ((0x00000300U & ((IData)(__VdfgRegularize_h6e95ff9d_0_0) 
                                           << 8U)) 
                           | (0x000000e0U & (IData)(__VdfgRegularize_h6e95ff9d_0_5)));
    vlSelfRef.a2nix_card__DOT__sp_path[4U] = ((((0x0000ff00U 
                                                 & ((((0U 
                                                       == 
                                                       (0x0000001fU 
                                                        & __VExpandSel_Lsb_10))
                                                       ? 0U
                                                       : 
                                                      (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                       [
                                                       (((IData)(7U) 
                                                         + __VExpandSel_Lsb_10) 
                                                        >> 5U)] 
                                                       << 
                                                       ((IData)(0x00000020U) 
                                                        - 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_10)))) 
                                                     | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (__VExpandSel_Lsb_10 
                                                         >> 5U)] 
                                                        >> 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_10))) 
                                                    << 8U)) 
                                                | (0x000000ffU 
                                                   & Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                   [
                                                   (0x0000001fU 
                                                    & ((IData)(__VdfgRegularize_h6e95ff9d_0_0) 
                                                       >> 2U))])) 
                                               << 0x00000010U) 
                                              | ((0x0000ff00U 
                                                  & ((((0U 
                                                        == 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_11))
                                                        ? 0U
                                                        : 
                                                       (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (((IData)(7U) 
                                                          + __VExpandSel_Lsb_11) 
                                                         >> 5U)] 
                                                        << 
                                                        ((IData)(0x00000020U) 
                                                         - 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_11)))) 
                                                      | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (__VExpandSel_Lsb_11 
                                                          >> 5U)] 
                                                         >> 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_11))) 
                                                     << 8U)) 
                                                 | (0x000000ffU 
                                                    & Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                    [
                                                    (0x0000001fU 
                                                     & (IData)(__VdfgRegularize_h6e95ff9d_0_5))])));
    __VExpandSel_Lsb_12 = ((0x00000380U & (a2nix_card__DOT__sp_digest[4U] 
                                           >> 1U)) 
                           | (0x00000060U & (a2nix_card__DOT__sp_digest[4U] 
                                             >> 0x00000011U)));
    __VExpandSel_Lsb_13 = ((0x00000200U & (a2nix_card__DOT__sp_digest[4U] 
                                           >> 7U)) 
                           | (0x000001e0U & (a2nix_card__DOT__sp_digest[4U] 
                                             >> 0x00000017U)));
    vlSelfRef.a2nix_card__DOT__sp_path[5U] = ((((0x0000ff00U 
                                                 & (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                    [
                                                    (0x0000001fU 
                                                     & (a2nix_card__DOT__sp_digest[4U] 
                                                        >> 0x0000000bU))] 
                                                    << 8U)) 
                                                | (0x000000ffU 
                                                   & (((0U 
                                                        == 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_12))
                                                        ? 0U
                                                        : 
                                                       (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (((IData)(7U) 
                                                          + __VExpandSel_Lsb_12) 
                                                         >> 5U)] 
                                                        << 
                                                        ((IData)(0x00000020U) 
                                                         - 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_12)))) 
                                                      | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (__VExpandSel_Lsb_12 
                                                          >> 5U)] 
                                                         >> 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_12))))) 
                                               << 0x00000010U) 
                                              | ((0x0000ff00U 
                                                  & (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                     [
                                                     (0x0000001fU 
                                                      & (a2nix_card__DOT__sp_digest[4U] 
                                                         >> 0x00000011U))] 
                                                     << 8U)) 
                                                 | (0x000000ffU 
                                                    & (((0U 
                                                         == 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_13))
                                                         ? 0U
                                                         : 
                                                        (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (((IData)(7U) 
                                                           + __VExpandSel_Lsb_13) 
                                                          >> 5U)] 
                                                         << 
                                                         ((IData)(0x00000020U) 
                                                          - 
                                                          (0x0000001fU 
                                                           & __VExpandSel_Lsb_13)))) 
                                                       | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                          [
                                                          (__VExpandSel_Lsb_13 
                                                           >> 5U)] 
                                                          >> 
                                                          (0x0000001fU 
                                                           & __VExpandSel_Lsb_13))))));
    __VExpandSel_Lsb_14 = ((0x000003c0U & (a2nix_card__DOT__sp_digest[3U] 
                                           >> 0x0000000aU)) 
                           | (0x00000020U & (a2nix_card__DOT__sp_digest[3U] 
                                             >> 0x0000001aU)));
    __VExpandSel_Lsb_15 = ((0x00000300U & (a2nix_card__DOT__sp_digest[3U] 
                                           >> 0x00000010U)) 
                           | (0x000000e0U & a2nix_card__DOT__sp_digest[4U]));
    vlSelfRef.a2nix_card__DOT__sp_path[6U] = ((((0x0000ff00U 
                                                 & ((((0U 
                                                       == 
                                                       (0x0000001fU 
                                                        & __VExpandSel_Lsb_14))
                                                       ? 0U
                                                       : 
                                                      (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                       [
                                                       (((IData)(7U) 
                                                         + __VExpandSel_Lsb_14) 
                                                        >> 5U)] 
                                                       << 
                                                       ((IData)(0x00000020U) 
                                                        - 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_14)))) 
                                                     | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (__VExpandSel_Lsb_14 
                                                         >> 5U)] 
                                                        >> 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_14))) 
                                                    << 8U)) 
                                                | (0x000000ffU 
                                                   & Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                   [
                                                   (0x0000001fU 
                                                    & (a2nix_card__DOT__sp_digest[3U] 
                                                       >> 0x0000001aU))])) 
                                               << 0x00000010U) 
                                              | ((0x0000ff00U 
                                                  & ((((0U 
                                                        == 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_15))
                                                        ? 0U
                                                        : 
                                                       (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (((IData)(7U) 
                                                          + __VExpandSel_Lsb_15) 
                                                         >> 5U)] 
                                                        << 
                                                        ((IData)(0x00000020U) 
                                                         - 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_15)))) 
                                                      | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (__VExpandSel_Lsb_15 
                                                          >> 5U)] 
                                                         >> 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_15))) 
                                                     << 8U)) 
                                                 | (0x000000ffU 
                                                    & Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                    [
                                                    (0x0000001fU 
                                                     & a2nix_card__DOT__sp_digest[4U])])));
    __VExpandSel_Lsb_16 = ((0x00000380U & (a2nix_card__DOT__sp_digest[3U] 
                                           << 7U)) 
                           | (0x00000060U & (a2nix_card__DOT__sp_digest[3U] 
                                             >> 9U)));
    __VExpandSel_Lsb_17 = ((0x00000200U & (a2nix_card__DOT__sp_digest[3U] 
                                           << 1U)) 
                           | (0x000001e0U & (a2nix_card__DOT__sp_digest[3U] 
                                             >> 0x0000000fU)));
    vlSelfRef.a2nix_card__DOT__sp_path[7U] = ((((0x0000ff00U 
                                                 & (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                    [
                                                    (0x0000001fU 
                                                     & (a2nix_card__DOT__sp_digest[3U] 
                                                        >> 3U))] 
                                                    << 8U)) 
                                                | (0x000000ffU 
                                                   & (((0U 
                                                        == 
                                                        (0x0000001fU 
                                                         & __VExpandSel_Lsb_16))
                                                        ? 0U
                                                        : 
                                                       (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                        [
                                                        (((IData)(7U) 
                                                          + __VExpandSel_Lsb_16) 
                                                         >> 5U)] 
                                                        << 
                                                        ((IData)(0x00000020U) 
                                                         - 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_16)))) 
                                                      | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (__VExpandSel_Lsb_16 
                                                          >> 5U)] 
                                                         >> 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_16))))) 
                                               << 0x00000010U) 
                                              | ((0x0000ff00U 
                                                  & (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                     [
                                                     (0x0000001fU 
                                                      & (a2nix_card__DOT__sp_digest[3U] 
                                                         >> 9U))] 
                                                     << 8U)) 
                                                 | (0x000000ffU 
                                                    & (((0U 
                                                         == 
                                                         (0x0000001fU 
                                                          & __VExpandSel_Lsb_17))
                                                         ? 0U
                                                         : 
                                                        (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                         [
                                                         (((IData)(7U) 
                                                           + __VExpandSel_Lsb_17) 
                                                          >> 5U)] 
                                                         << 
                                                         ((IData)(0x00000020U) 
                                                          - 
                                                          (0x0000001fU 
                                                           & __VExpandSel_Lsb_17)))) 
                                                       | (Va2nix_card__ConstPool__CONST_h4c4fb9bc_0
                                                          [
                                                          (__VExpandSel_Lsb_17 
                                                           >> 5U)] 
                                                          >> 
                                                          (0x0000001fU 
                                                           & __VExpandSel_Lsb_17))))));
}

void Va2nix_card___024root___trigger_orInto__act_vec_vec(VlUnpacked<QData/*63:0*/, 1> &out, const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___trigger_orInto__act_vec_vec\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = (out[n] | in[n]);
        n = ((IData)(1U) + n);
    } while ((0U >= n));
}

void Va2nix_card___024root___trigger_clear__act(VlUnpacked<QData/*63:0*/, 1> &out) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___trigger_clear__act\n"); );
    // Locals
    IData/*31:0*/ n;
    // Body
    n = 0U;
    do {
        out[n] = 0ULL;
        n = ((IData)(1U) + n);
    } while ((1U > n));
}

#ifdef VL_DEBUG
void Va2nix_card___024root___eval_debug_assertions(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_debug_assertions\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    if (VL_UNLIKELY(((vlSelfRef.clk & 0xfeU)))) {
        Verilated::overWidthError("clk");
    }
    if (VL_UNLIKELY(((vlSelfRef.rst_n & 0xfeU)))) {
        Verilated::overWidthError("rst_n");
    }
    if (VL_UNLIKELY(((vlSelfRef.c0n_addr & 0xf0U)))) {
        Verilated::overWidthError("c0n_addr");
    }
    if (VL_UNLIKELY(((vlSelfRef.c0n_read & 0xfeU)))) {
        Verilated::overWidthError("c0n_read");
    }
    if (VL_UNLIKELY(((vlSelfRef.c0n_write & 0xfeU)))) {
        Verilated::overWidthError("c0n_write");
    }
    if (VL_UNLIKELY(((vlSelfRef.c8_addr & 0xf800U)))) {
        Verilated::overWidthError("c8_addr");
    }
}
#endif  // VL_DEBUG
