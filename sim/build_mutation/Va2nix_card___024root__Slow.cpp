// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Design implementation internals
// See Va2nix_card.h for the primary calling header

#include "Va2nix_card__pch.h"

void Va2nix_card___024root___ctor_var_reset(Va2nix_card___024root* vlSelf);

Va2nix_card___024root::Va2nix_card___024root(Va2nix_card__Syms* symsp, const char* namep)
 {
    vlSymsp = symsp;
    vlNamep = strdup(namep);
    // Reset structure values
    Va2nix_card___024root___ctor_var_reset(this);
}

void Va2nix_card___024root::__Vconfigure(bool first) {
    (void)first;  // Prevent unused variable warning
}

Va2nix_card___024root::~Va2nix_card___024root() {
    VL_DO_DANGLING(std::free(const_cast<char*>(vlNamep)), vlNamep);
}
