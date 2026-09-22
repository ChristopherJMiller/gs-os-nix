// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Va2nix_card.h for the primary calling header

#include "Va2nix_card__pch.h"

VL_ATTR_COLD void Va2nix_card___024root___eval_static(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_static\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    vlSelfRef.__Vtrigprevexpr___TOP__clk__0 = vlSelfRef.clk;
    vlSelfRef.__Vtrigprevexpr___TOP__rst_n__0 = vlSelfRef.rst_n;
    vlSelfRef.__Vtrigprevexpr___TOP__c0n_addr__0 = vlSelfRef.c0n_addr;
    vlSelfRef.__Vtrigprevexpr___TOP__c0n_read__0 = vlSelfRef.c0n_read;
    vlSelfRef.__Vtrigprevexpr___TOP__c0n_write__0 = vlSelfRef.c0n_write;
    vlSelfRef.__Vtrigprevexpr___TOP__c0n_wdata__0 = vlSelfRef.c0n_wdata;
    vlSelfRef.__Vtrigprevexpr___TOP__c8_addr__0 = vlSelfRef.c8_addr;
    vlSelfRef.__Vtrigprevexpr___TOP__clk__1 = vlSelfRef.clk;
}

VL_ATTR_COLD void Va2nix_card___024root___eval_initial(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_initial\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    {
        // Inlined CFunc: _eval_initial__TOP
        vlSelfRef.take_c800 = 0U;
        vlSelfRef.c8_rdata = 0U;
    }
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Va2nix_card___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG
VL_ATTR_COLD bool Va2nix_card___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in);
VL_ATTR_COLD void Va2nix_card___024root___stl_sequent__TOP__0(Va2nix_card___024root* vlSelf);

VL_ATTR_COLD bool Va2nix_card___024root___eval_stl(Va2nix_card___024root* vlSelf, CData/*0:0*/ firstIteration) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_stl\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Locals
    CData/*0:0*/ __VstlExecute;
    // Body
    vlSelfRef.__VstlTriggered[0U] = ((0xfffffffffffffffeULL 
                                      & vlSelfRef.__VstlTriggered[0U]) 
                                     | (IData)((IData)(firstIteration)));
#ifdef VL_DEBUG
    if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {
        Va2nix_card___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
    }
#endif
    __VstlExecute = Va2nix_card___024root___trigger_anySet__stl(vlSelfRef.__VstlTriggered);
    if (__VstlExecute) {
        {
            // Inlined CFunc: _eval_body__stl
            if ((1ULL & vlSelfRef.__VstlTriggered[0U])) {
                Va2nix_card___024root___stl_sequent__TOP__0(vlSelf);
            }
        }
    }
    return (__VstlExecute);
}

VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__stl(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_dump_triggers__stl\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Va2nix_card___024root___dump_triggers__stl(vlSelfRef.__VstlTriggered, "stl"s);
#endif
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Va2nix_card___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__ico(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_dump_triggers__ico\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Va2nix_card___024root___dump_triggers__ico(vlSelfRef.__VicoTriggered, "ico"s);
#endif
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Va2nix_card___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag);
#endif  // VL_DEBUG

VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__act(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_dump_triggers__act\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Va2nix_card___024root___dump_triggers__act(vlSelfRef.__VactTriggered, "act"s);
#endif
}

VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__nba(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_dump_triggers__nba\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
#ifdef VL_DEBUG
    Va2nix_card___024root___dump_triggers__act(vlSelfRef.__VnbaTriggered, "nba"s);
#endif
}

VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__obs(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_dump_triggers__obs\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__react(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_dump_triggers__react\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

VL_ATTR_COLD void Va2nix_card___024root___eval_final(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___eval_final\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
}

#ifdef VL_DEBUG
VL_ATTR_COLD void Va2nix_card___024root___dump_triggers__stl(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___dump_triggers__stl\n"); );
    // Body
    if ((1U & (~ (IData)(Va2nix_card___024root___trigger_anySet__stl(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: Internal 'stl' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD bool Va2nix_card___024root___trigger_anySet__stl(const VlUnpacked<QData/*63:0*/, 1> &in) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___trigger_anySet__stl\n"); );
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

extern const VlWide<64>/*2047:0*/ Va2nix_card__ConstPool__CONST_h7be248c9_0;
extern const VlWide<32>/*1023:0*/ Va2nix_card__ConstPool__CONST_h4c4fb9bc_0;

VL_ATTR_COLD void Va2nix_card___024root___stl_sequent__TOP__0(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___stl_sequent__TOP__0\n"); );
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
    vlSelfRef.irq = ((IData)(vlSelfRef.a2nix_card__DOT__irq_pend) 
                     & (IData)(vlSelfRef.a2nix_card__DOT__irq_en));
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
    vlSelfRef.__VdfgRegularize_h6e95ff9d_0_13 = (IData)(
                                                        (1U 
                                                         == 
                                                         (3U 
                                                          & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))));
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
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__pb_v = 
        (1U & ((4U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))
                ? ((2U & (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))
                    ? (~ (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state))
                    : (~ ((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__state) 
                          & (0x38U == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__bidx)))))
                : ((IData)(vlSelfRef.a2nix_card__DOT__h_valid) 
                   & (IData)(vlSelfRef.__VdfgRegularize_h6e95ff9d_0_13))));
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
    vlSelfRef.a2nix_card__DOT__u_hash__DOT__blk_full 
        = ((IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__pb_v) 
           & (0x3fU == (IData)(vlSelfRef.a2nix_card__DOT__u_hash__DOT__bidx)));
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

bool Va2nix_card___024root___trigger_anySet__ico(const VlUnpacked<QData/*63:0*/, 2> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Va2nix_card___024root___dump_triggers__ico(const VlUnpacked<QData/*63:0*/, 2> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___dump_triggers__ico\n"); );
    // Body
    if ((1U & (~ (IData)(Va2nix_card___024root___trigger_anySet__ico(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @( clk)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 1U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 1 is active: @( rst_n)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 2U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 2 is active: @( c0n_addr)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 3U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 3 is active: @( c0n_read)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 4U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 4 is active: @( c0n_write)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 5U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 5 is active: @( c0n_wdata)\n");
    }
    if ((1U & (IData)((triggers[0U] >> 6U)))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 6 is active: @( c8_addr)\n");
    }
    if ((1U & (IData)(triggers[1U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 64 is active: Internal 'ico' trigger - first iteration\n");
    }
}
#endif  // VL_DEBUG

bool Va2nix_card___024root___trigger_anySet__act(const VlUnpacked<QData/*63:0*/, 1> &in);

#ifdef VL_DEBUG
VL_ATTR_COLD void Va2nix_card___024root___dump_triggers__act(const VlUnpacked<QData/*63:0*/, 1> &triggers, const std::string &tag) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___dump_triggers__act\n"); );
    // Body
    if ((1U & (~ (IData)(Va2nix_card___024root___trigger_anySet__act(triggers))))) {
        VL_DBG_MSGS("         No '" + tag + "' region triggers active\n");
    }
    if ((1U & (IData)(triggers[0U]))) {
        VL_DBG_MSGS("         '" + tag + "' region trigger index 0 is active: @(posedge clk)\n");
    }
}
#endif  // VL_DEBUG

VL_ATTR_COLD void Va2nix_card___024root___ctor_var_reset(Va2nix_card___024root* vlSelf) {
    VL_DEBUG_IF(VL_DBG_MSGF("+    Va2nix_card___024root___ctor_var_reset\n"); );
    Va2nix_card__Syms* const __restrict vlSymsp VL_ATTR_UNUSED = vlSelf->vlSymsp;
    auto& vlSelfRef = std::ref(*vlSelf).get();
    // Body
    const uint64_t __VscopeHash = VL_MURMUR64_HASH(vlSelf->vlNamep);
    vlSelf->clk = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 16707436170211756652ull);
    vlSelf->rst_n = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1638864771569018232ull);
    vlSelf->c0n_addr = VL_SCOPED_RAND_RESET_I(4, __VscopeHash, 14548709838856734440ull);
    vlSelf->c0n_read = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 3858436410193466553ull);
    vlSelf->c0n_write = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 13212842050722440868ull);
    vlSelf->c0n_wdata = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 7321508696511664110ull);
    vlSelf->c0n_rdata = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 12509978648089169472ull);
    vlSelf->c8_addr = VL_SCOPED_RAND_RESET_I(11, __VscopeHash, 6382339021077338351ull);
    vlSelf->c8_rdata = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 12409039179787095635ull);
    vlSelf->take_c800 = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 10306784107109258875ull);
    vlSelf->irq = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 14145092342636110857ull);
    for (int __Vi0 = 0; __Vi0 < 4; ++__Vi0) {
        vlSelf->a2nix_card__DOT__areg[__Vi0] = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 722635887473999200ull);
    }
    vlSelf->a2nix_card__DOT__asel = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 3446980278264924164ull);
    vlSelf->a2nix_card__DOT__aidx = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 6710973858161444695ull);
    for (int __Vi0 = 0; __Vi0 < 8; ++__Vi0) {
        VL_SCOPED_RAND_RESET_W(256, vlSelf->a2nix_card__DOT__pslot[__Vi0], __VscopeHash, 9966940094285975760ull);
    }
    vlSelf->a2nix_card__DOT__psel = VL_SCOPED_RAND_RESET_I(3, __VscopeHash, 16016981606757001197ull);
    vlSelf->a2nix_card__DOT__pidx = VL_SCOPED_RAND_RESET_I(5, __VscopeHash, 15114373117654017959ull);
    vlSelf->a2nix_card__DOT__len = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 11649258341991976045ull);
    vlSelf->a2nix_card__DOT__lenidx = VL_SCOPED_RAND_RESET_I(2, __VscopeHash, 7236861084778238065ull);
    vlSelf->a2nix_card__DOT__busy = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 13506803665258747826ull);
    vlSelf->a2nix_card__DOT__done = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 4744550981651604477ull);
    vlSelf->a2nix_card__DOT__err = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 8495339814327143065ull);
    vlSelf->a2nix_card__DOT__errcode = VL_SCOPED_RAND_RESET_I(4, __VscopeHash, 7662205999651000497ull);
    vlSelf->a2nix_card__DOT__irq_en = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 15391709658256773672ull);
    vlSelf->a2nix_card__DOT__irq_pend = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 15593219580533546564ull);
    vlSelf->a2nix_card__DOT__push_mode = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 4800730758356388842ull);
    vlSelf->a2nix_card__DOT__h_init = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 13196224002182362964ull);
    vlSelf->a2nix_card__DOT__h_valid = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 2979734007334296888ull);
    vlSelf->a2nix_card__DOT__h_byte = VL_SCOPED_RAND_RESET_I(8, __VscopeHash, 16692655066606527497ull);
    vlSelf->a2nix_card__DOT__h_finish = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 17827477709398699914ull);
    vlSelf->a2nix_card__DOT__h_done = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 2962285973427987331ull);
    VL_SCOPED_RAND_RESET_W(256, vlSelf->a2nix_card__DOT__h_digest, __VscopeHash, 16332362132850996325ull);
    VL_SCOPED_RAND_RESET_W(256, vlSelf->a2nix_card__DOT__sp_path, __VscopeHash, 11147478616068624782ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__state = VL_SCOPED_RAND_RESET_I(3, __VscopeHash, 1571757367621713113ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__ret = VL_SCOPED_RAND_RESET_I(3, __VscopeHash, 5749412545181126154ull);
    VL_SCOPED_RAND_RESET_W(512, vlSelf->a2nix_card__DOT__u_hash__DOT__blk, __VscopeHash, 8756158300460521328ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__bidx = VL_SCOPED_RAND_RESET_I(7, __VscopeHash, 14922572099590815338ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__total = VL_SCOPED_RAND_RESET_Q(64, __VscopeHash, 13575615976560101982ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__lenidx = VL_SCOPED_RAND_RESET_I(3, __VscopeHash, 12104378370137929965ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__core_init = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1327139182060513165ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__core_start = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 6052780346447277291ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__core_busy = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 1185227756741327161ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__pb_v = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 18205293078655255472ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__blk_full = VL_SCOPED_RAND_RESET_I(1, __VscopeHash, 7406148807189136927ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__h0 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 6855306559066315529ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__h1 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 11702253794487675342ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__h2 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 321720808484134250ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__h3 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 15778551549822065880ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__h4 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 17599009491990574919ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__h5 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 2638216509716959835ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__h6 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 15407875489784555585ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__h7 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 11978620949691068017ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__a = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 15867838414843445249ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__b = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 10550796296732294486ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__c = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 17960765844831219678ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__d = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 12504132502624778948ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__e = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 11107984103334415336ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__f = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 12411839661661361063ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__g = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 11496980215480287826ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__h = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 7824251520422996232ull);
    for (int __Vi0 = 0; __Vi0 < 16; ++__Vi0) {
        vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__w[__Vi0] = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 3626235005080104516ull);
    }
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__round = VL_SCOPED_RAND_RESET_I(7, __VscopeHash, 3061031669245242919ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__t1 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 1068196738181804443ull);
    vlSelf->a2nix_card__DOT__u_hash__DOT__u_core__DOT__t2 = VL_SCOPED_RAND_RESET_I(32, __VscopeHash, 15294622161845908096ull);
    vlSelf->__VdfgRegularize_h6e95ff9d_0_13 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VstlTriggered[__Vi0] = 0;
    }
    for (int __Vi0 = 0; __Vi0 < 2; ++__Vi0) {
        vlSelf->__VicoTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__rst_n__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__c0n_addr__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__c0n_read__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__c0n_write__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__c0n_wdata__0 = 0;
    vlSelf->__Vtrigprevexpr___TOP__c8_addr__0 = 0;
    vlSelf->__VicoDidInit = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VactTriggered[__Vi0] = 0;
    }
    vlSelf->__Vtrigprevexpr___TOP__clk__1 = 0;
    for (int __Vi0 = 0; __Vi0 < 1; ++__Vi0) {
        vlSelf->__VnbaTriggered[__Vi0] = 0;
    }
}
