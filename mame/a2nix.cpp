// license:BSD-3-Clause
// copyright-holders:gs-os-nix
/*********************************************************************

    a2nix.cpp

    The a2nix accelerator card: SHA-256, store-path encoding, and (from
    L3) a tagged heap, for running Nix natively on an Apple IIgs.

    This file contains NO card behaviour. It is a shim that drives the
    Verilated model of rtl/a2nix_card.v, so that MAME and a real ECP5
    bitstream execute identical logic and there is exactly one source of
    truth. Behaviour changes belong in the RTL.

    Register map and verbs: docs/CARD-ISA.md (normative).

*********************************************************************/

#include "emu.h"
#include "a2nix.h"

#include "Va2nix_card.h"

#include <memory>

namespace {

/***************************************************************************
    The card's own clock.

    A real card is clocked by its own oscillator while the IIgs reaches it
    through a slot the FPI throttles to 1.024 MHz. That ratio is not a
    detail: it decides how much of a compression stall the CPU ever
    observes, and getting it wrong makes verb-cost measurements measure the
    harness instead of the hardware (see the methodology note in
    docs/CARD-ISA.md).

    Rather than assume a fixed number of cycles per access, the model is
    advanced according to elapsed emulated time, so the card makes progress
    while the CPU is busy elsewhere -- exactly as real hardware does.
***************************************************************************/
constexpr unsigned CARD_CLOCK_HZ = 50'000'000;

// Cap a single catch-up. The longest operation in flight is padding plus two
// block compressions, a few hundred cycles; anything beyond this means the
// card was idle and further ticks change nothing. Bounding it keeps a long
// gap between accesses from spinning the simulation.
constexpr u64 MAX_CATCHUP_CYCLES = 4096;

class a2bus_a2nix_device : public device_t, public device_a2bus_card_interface
{
public:
	a2bus_a2nix_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
		: a2bus_a2nix_device(mconfig, A2BUS_A2NIX, tag, owner, clock)
	{
	}

protected:
	a2bus_a2nix_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock)
		: device_t(mconfig, type, tag, owner, clock)
		, device_a2bus_card_interface(mconfig, *this)
		, m_card(nullptr)
		, m_last_time(attotime::zero)
		, m_irq_asserted(false)
	{
	}

	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;
	virtual void device_stop() override ATTR_COLD;

	// device_a2bus_card_interface
	virtual u8 read_c0nx(u8 offset) override;
	virtual void write_c0nx(u8 offset, u8 data) override;
	virtual u8 read_c800(u16 offset) override;
	virtual bool take_c800() const override;
	virtual void reset_from_bus() override;

private:
	void tick();
	void catch_up();
	void update_irq();

	std::unique_ptr<Va2nix_card> m_card;
	attotime m_last_time;
	bool m_irq_asserted;
};

void a2bus_a2nix_device::device_start()
{
	m_card = std::make_unique<Va2nix_card>();

	m_card->clk       = 0;
	m_card->rst_n     = 0;
	m_card->c0n_addr  = 0;
	m_card->c0n_read  = 0;
	m_card->c0n_write = 0;
	m_card->c0n_wdata = 0;
	m_card->c8_addr   = 0;

	save_item(NAME(m_irq_asserted));
	// NOTE: the Verilated model's internal state is not save-state capable.
	// Acceptable for a development card; a save/restore will reset it.
}

void a2bus_a2nix_device::device_stop()
{
	if (m_card)
	{
		m_card->final();
		m_card.reset();
	}
}

void a2bus_a2nix_device::device_reset()
{
	reset_from_bus();
}

void a2bus_a2nix_device::reset_from_bus()
{
	if (!m_card)
		return;

	m_card->rst_n     = 0;
	m_card->c0n_read  = 0;
	m_card->c0n_write = 0;
	for (int i = 0; i < 8; i++)
		tick();
	m_card->rst_n = 1;
	tick();

	m_last_time = machine().time();
	update_irq();
}

void a2bus_a2nix_device::tick()
{
	m_card->clk = 0;
	m_card->eval();
	m_card->clk = 1;
	m_card->eval();
}

// Advance the model to the present, so the card progresses while the CPU is
// doing something else instead of only when it is poked.
void a2bus_a2nix_device::catch_up()
{
	const attotime now = machine().time();
	if (now <= m_last_time)
		return;

	u64 want = (now - m_last_time).as_ticks(CARD_CLOCK_HZ);
	m_last_time = now;

	if (want > MAX_CATCHUP_CYCLES)
		want = MAX_CATCHUP_CYCLES;

	while (want--)
		tick();
}

void a2bus_a2nix_device::update_irq()
{
	const bool want = m_card->irq != 0;
	if (want != m_irq_asserted)
	{
		m_irq_asserted = want;
		if (want)
			raise_slot_irq();
		else
			lower_slot_irq();
	}
}

u8 a2bus_a2nix_device::read_c0nx(u8 offset)
{
	if (!m_card)
		return 0;

	// The debugger reads memory without wanting consequences. Sample the
	// combinational read path but skip the clock edge, so auto-incrementing
	// ports (APORT/PPORT/LEN) are not advanced by merely looking at them.
	const bool commit = !machine().side_effects_disabled();

	if (commit)
		catch_up();

	m_card->c0n_addr  = offset & 0x0f;
	m_card->c0n_read  = commit ? 1 : 0;
	m_card->c0n_write = 0;
	m_card->eval();

	const u8 result = m_card->c0n_rdata;

	if (commit)
	{
		tick();
		m_card->c0n_read = 0;
		update_irq();
	}

	return result;
}

void a2bus_a2nix_device::write_c0nx(u8 offset, u8 data)
{
	if (!m_card || machine().side_effects_disabled())
		return;

	catch_up();

	m_card->c0n_addr  = offset & 0x0f;
	m_card->c0n_wdata = data;
	m_card->c0n_write = 1;
	m_card->c0n_read  = 0;
	tick();
	m_card->c0n_write = 0;

	update_irq();
}

// $C800-$CFFF window. Inert until L3 gives the card memory of its own; the RTL
// drives take_c800 low, so the card does not contend for the shared window.
u8 a2bus_a2nix_device::read_c800(u16 offset)
{
	if (!m_card)
		return 0;

	m_card->c8_addr = offset & 0x7ff;
	m_card->eval();
	return m_card->c8_rdata;
}

bool a2bus_a2nix_device::take_c800() const
{
	return m_card && (m_card->take_c800 != 0);
}

} // anonymous namespace

DEFINE_DEVICE_TYPE_PRIVATE(A2BUS_A2NIX, device_a2bus_card_interface, a2bus_a2nix_device, "a2nix", "a2nix Nix accelerator")
