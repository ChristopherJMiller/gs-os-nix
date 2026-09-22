// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Symbol table internal header
//
// Internal details; most calling programs do not need this header,
// unless using verilator public meta comments.

#ifndef VERILATED_VA2NIX_CARD__SYMS_H_
#define VERILATED_VA2NIX_CARD__SYMS_H_  // guard

#include "verilated.h"

// INCLUDE MODEL CLASS

#include "Va2nix_card.h"

// INCLUDE MODULE CLASSES
#include "Va2nix_card___024root.h"

// SYMS CLASS (contains all model state)
class alignas(VL_CACHE_LINE_BYTES) Va2nix_card__Syms final : public VerilatedSyms {
  public:
    // INTERNAL STATE
    Va2nix_card* const __Vm_modelp;
    VlDeleter __Vm_deleter;
    bool& __Vm_didInit;

    // MODULE INSTANCE STATE
    Va2nix_card___024root          TOP;

    // CONSTRUCTORS
    Va2nix_card__Syms(VerilatedContext* contextp, const char* namep, Va2nix_card* modelp);
    ~Va2nix_card__Syms();

    // METHODS
    const char* name() const { return TOP.vlNamep; }
};

#endif  // guard
