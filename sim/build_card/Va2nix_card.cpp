// Verilated -*- C++ -*-
// DESCRIPTION: Verilator output: Model implementation (design independent parts)

#include "Va2nix_card__pch.h"

//============================================================
// Constructors

Va2nix_card::Va2nix_card(VerilatedContext* _vcontextp__, const char* _vcname__)
    : VerilatedModel{*_vcontextp__}
    , vlSymsp{new Va2nix_card__Syms(contextp(), _vcname__, this)}
    , m_evalLoop{*this, /*convergeLimit:*/ 10000}
    , clk{vlSymsp->TOP.clk}
    , rst_n{vlSymsp->TOP.rst_n}
    , c0n_addr{vlSymsp->TOP.c0n_addr}
    , c0n_read{vlSymsp->TOP.c0n_read}
    , c0n_write{vlSymsp->TOP.c0n_write}
    , c0n_wdata{vlSymsp->TOP.c0n_wdata}
    , c0n_rdata{vlSymsp->TOP.c0n_rdata}
    , c8_rdata{vlSymsp->TOP.c8_rdata}
    , take_c800{vlSymsp->TOP.take_c800}
    , mem_we{vlSymsp->TOP.mem_we}
    , mem_wdata{vlSymsp->TOP.mem_wdata}
    , mem_rdata{vlSymsp->TOP.mem_rdata}
    , irq{vlSymsp->TOP.irq}
    , c8_addr{vlSymsp->TOP.c8_addr}
    , mem_addr{vlSymsp->TOP.mem_addr}
    , rootp{&(vlSymsp->TOP)}
{
    // Register model with the context
    contextp()->addModel(this);
}

Va2nix_card::Va2nix_card(const char* _vcname__)
    : Va2nix_card(Verilated::threadContextp(), _vcname__)
{
}

//============================================================
// Destructor

Va2nix_card::~Va2nix_card() {
    delete vlSymsp;
}

//============================================================
// Evaluation function

#ifdef VL_DEBUG
void Va2nix_card___024root___eval_debug_assertions(Va2nix_card___024root* vlSelf);
#endif  // VL_DEBUG
VL_ATTR_COLD void Va2nix_card___024root___eval_static(Va2nix_card___024root* vlSelf);
VL_ATTR_COLD void Va2nix_card___024root___eval_initial(Va2nix_card___024root* vlSelf);
VL_ATTR_COLD bool Va2nix_card___024root___eval_stl(Va2nix_card___024root* vlSelf, CData/*0:0*/ firstIteration);
void Va2nix_card___024root___eval_sample(Va2nix_card___024root* vlSelf);
bool Va2nix_card___024root___eval_ico(Va2nix_card___024root* vlSelf, CData/*0:0*/ firstIteration);
bool Va2nix_card___024root___eval_act(Va2nix_card___024root* vlSelf);
bool Va2nix_card___024root___eval_inact(Va2nix_card___024root* vlSelf);
bool Va2nix_card___024root___eval_nba(Va2nix_card___024root* vlSelf);
bool Va2nix_card___024root___eval_obs(Va2nix_card___024root* vlSelf);
bool Va2nix_card___024root___eval_react(Va2nix_card___024root* vlSelf);
void Va2nix_card___024root___eval_postponed(Va2nix_card___024root* vlSelf);
VL_ATTR_COLD void Va2nix_card___024root___eval_final(Va2nix_card___024root* vlSelf);
VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__stl(Va2nix_card___024root* vlSelf);
VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__ico(Va2nix_card___024root* vlSelf);
VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__act(Va2nix_card___024root* vlSelf);
VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__nba(Va2nix_card___024root* vlSelf);
VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__obs(Va2nix_card___024root* vlSelf);
VL_ATTR_COLD void Va2nix_card___024root___eval_dump_triggers__react(Va2nix_card___024root* vlSelf);

void Va2nix_card::eval_step() {
    VL_DEBUG_IF(VL_DBG_MSGF("+++++TOP Evaluate Va2nix_card::eval_step\n"); );
    m_evalLoop.eval();
}

void Va2nix_card::evalBegin() {
#ifdef VL_DEBUG
    // Debug assertions
    Va2nix_card___024root___eval_debug_assertions(&(vlSymsp->TOP));
#endif  // VL_DEBUG
    vlSymsp->__Vm_deleter.deleteAll();
}

void Va2nix_card::evalEnd() {
    // Evaluate cleanup
    Verilated::endOfEval(vlSymsp->__Vm_evalMsgQp);
}

void Va2nix_card::evalStatic() {
    Va2nix_card___024root___eval_static(&(vlSymsp->TOP));
}

void Va2nix_card::evalInitial() {
    Va2nix_card___024root___eval_initial(&(vlSymsp->TOP));
}

bool Va2nix_card::evalStl(bool firstIteration) {
    return Va2nix_card___024root___eval_stl(&(vlSymsp->TOP), firstIteration);
}

void Va2nix_card::evalSample() {
    Va2nix_card___024root___eval_sample(&(vlSymsp->TOP));
}

bool Va2nix_card::evalIco(bool firstIteration) {
    return Va2nix_card___024root___eval_ico(&(vlSymsp->TOP), firstIteration);
}

bool Va2nix_card::evalAct() {
    return Va2nix_card___024root___eval_act(&(vlSymsp->TOP));
}

bool Va2nix_card::evalInact() {
    return Va2nix_card___024root___eval_inact(&(vlSymsp->TOP));
}

bool Va2nix_card::evalNba() {
    return Va2nix_card___024root___eval_nba(&(vlSymsp->TOP));
}

bool Va2nix_card::evalObs() {
    return Va2nix_card___024root___eval_obs(&(vlSymsp->TOP));
}

bool Va2nix_card::evalReact() {
    return Va2nix_card___024root___eval_react(&(vlSymsp->TOP));
}

void Va2nix_card::evalPostponed() {
    Va2nix_card___024root___eval_postponed(&(vlSymsp->TOP));
}

void Va2nix_card::evalFinal() {
    Va2nix_card___024root___eval_final(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Va2nix_card::dumpTriggersStl() {
    Va2nix_card___024root___eval_dump_triggers__stl(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Va2nix_card::dumpTriggersIco() {
    Va2nix_card___024root___eval_dump_triggers__ico(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Va2nix_card::dumpTriggersAct() {
    Va2nix_card___024root___eval_dump_triggers__act(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Va2nix_card::dumpTriggersNba() {
    Va2nix_card___024root___eval_dump_triggers__nba(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Va2nix_card::dumpTriggersObs() {
    Va2nix_card___024root___eval_dump_triggers__obs(&(vlSymsp->TOP));
}

VL_ATTR_COLD void Va2nix_card::dumpTriggersReact() {
    Va2nix_card___024root___eval_dump_triggers__react(&(vlSymsp->TOP));
}

//============================================================
// Events and timing
bool Va2nix_card::eventsPending() { return false; }

uint64_t Va2nix_card::nextTimeSlot() {
    VL_FATAL_MT(__FILE__, __LINE__, "", "No delays in the design");
    return 0;
}

//============================================================
// Utilities

const char* Va2nix_card::name() const {
    return vlSymsp->name();
}

//============================================================
// Invoke final blocks

VL_ATTR_COLD void Va2nix_card::final() {
    contextp()->executingFinal(true);
    evalFinal();
    contextp()->executingFinal(false);
}

//============================================================
// Implementations of abstract methods from VerilatedModel

const char* Va2nix_card::hierName() const { return vlSymsp->name(); }
const char* Va2nix_card::modelName() const { return "Va2nix_card"; }
unsigned Va2nix_card::threads() const { return 1; }
void Va2nix_card::prepareClone() const { contextp()->prepareClone(); }
void Va2nix_card::atClone() const {
    contextp()->threadPoolpOnClone();
}
