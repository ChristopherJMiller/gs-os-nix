// license:BSD-3-Clause
// copyright-holders:gs-os-nix
/*********************************************************************

    a2nix.h

    Implementation of the a2nix accelerator card for the Apple IIgs.

    This device is a thin shim over the Verilated RTL in rtl/: the
    actual card behaviour lives in a2nix_card.v, so MAME and a real
    ECP5 bitstream run the same logic. Do not implement card
    behaviour here -- add it to the RTL instead.

    See docs/CARD-ISA.md for the register map and verb table.

*********************************************************************/

#ifndef MAME_BUS_A2BUS_A2NIX_H
#define MAME_BUS_A2BUS_A2NIX_H

#pragma once

#include "a2bus.h"

DECLARE_DEVICE_TYPE(A2BUS_A2NIX, device_a2bus_card_interface)

#endif // MAME_BUS_A2BUS_A2NIX_H
