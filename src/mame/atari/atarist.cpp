// license:BSD-3-Clause
// copyright-holders:Curt Coder, Olivier Galibert
#include "emu.h"

#include "ataristb.h"
#include "atarist_v.h"

#include "bus/centronics/ctronics.h"
#include "bus/generic/slot.h"
#include "bus/generic/carts.h"
#include "bus/midi/midi.h"
#include "bus/rs232/rs232.h"
#include "cpu/m68000/m68000.h"
#include "cpu/m6800/m6801.h"
#include "imagedev/floppy.h"
#include "machine/6850acia.h"
#include "machine/8530scc.h"
#include "machine/clock.h"
#include "machine/input_merger.h"
#include "machine/mc68901.h"
#include "machine/ram.h"
#include "machine/rescap.h"
#include "machine/rp5c15.h"
#include "machine/wd_fdc.h"
#include "sound/ay8910.h"
#include "sound/lmc1992.h"
#include "screen.h"
#include "softlist_dev.h"
#include "speaker.h"

/*

    TODO:

    - floppy write
    - floppy DMA transfer timer
    - mouse moves too fast?
    - UK keyboard layout for the special keys
    - accurate screen timing
    - STe DMA sound and LMC1992 Microwire mixer
    - Mega ST/STe MC68881 FPU
    - Mega STe 16KB cache
    - Mega STe LAN

    http://dev-docs.atariforge.org/
    http://info-coach.fr/atari/software/protection.php

*/

#include "formats/st_dsk.h"
#include "formats/pasti_dsk.h"
#include "formats/mfi_dsk.h"
#include "formats/dfi_dsk.h"
#include "formats/ipf_dsk.h"

#include "utf8.h"


//**************************************************************************
//  CONSTANTS / MACROS
//**************************************************************************

#define LOG 0

namespace {

#define M68000_TAG      "m68000"
#define HD6301V1_TAG    "hd6301"
#define YM2149_TAG      "ym2149"
#define MC6850_0_TAG    "mc6850_0"
#define MC6850_1_TAG    "mc6850_1"
#define Z8530_TAG       "z8530"
#define COP888_TAG      "u703"
#define RP5C15_TAG      "rp5c15"
#define YM3439_TAG      "ym3439"
#define MC68901_TAG     "mc68901"
#define LMC1992_TAG     "lmc1992"
#define WD1772_TAG      "wd1772"
#define SCREEN_TAG      "screen"
#define CENTRONICS_TAG  "centronics"
#define RS232_TAG       "rs232"

// Atari ST

#define Y1      XTAL(2'457'600)

// 32028400 also exists
#define Y2      32084988.0
#define Y2_NTSC 32042400.0

// STBook

#define U517    XTAL(16'000'000)
#define Y200    XTAL(2'457'600)
#define Y700    XTAL(10'000'000)

#define DMA_STATUS_DRQ              0x04
#define DMA_STATUS_SECTOR_COUNT     0x02
#define DMA_STATUS_ERROR            0x01

#define DMA_MODE_READ_WRITE         0x100
#define DMA_MODE_FDC_HDC_ACK        0x080
#define DMA_MODE_ENABLED            0x040
#define DMA_MODE_SECTOR_COUNT       0x010
#define DMA_MODE_FDC_HDC_CS         0x008
#define DMA_MODE_A1                 0x004
#define DMA_MODE_A0                 0x002
#define DMA_MODE_ADDRESS_MASK       0x006

#define DMA_SECTOR_SIZE             512

static const double DMASOUND_RATE[] = { Y2/640.0/8.0, Y2/640.0/4.0, Y2/640.0/2.0, Y2/640.0 };

static const int IKBD_MOUSE_XYA[3][4] = { { 0, 0, 0, 0 }, { 1, 1, 0, 0 }, { 0, 1, 1, 0 } };
static const int IKBD_MOUSE_XYB[3][4] = { { 0, 0, 0, 0 }, { 0, 1, 1, 0 }, { 1, 1, 0, 0 } };

enum
{
	IKBD_MOUSE_PHASE_STATIC = 0,
	IKBD_MOUSE_PHASE_POSITIVE,
	IKBD_MOUSE_PHASE_NEGATIVE
};

class st_state : public driver_device
{
public:
	st_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag),
			m_maincpu(*this, M68000_TAG),
			m_stb(*this, "stb"),
			m_ikbd(*this, HD6301V1_TAG),
			m_fdc(*this, WD1772_TAG),
			m_floppy(*this, WD1772_TAG ":%u", 0U),
			m_mfp(*this, MC68901_TAG),
			m_acia(*this, {MC6850_0_TAG, MC6850_1_TAG}),
			m_centronics(*this, CENTRONICS_TAG),
			m_cart(*this, "cartslot"),
			m_ram(*this, RAM_TAG),
			m_rs232(*this, RS232_TAG),
			m_ymsnd(*this, YM2149_TAG),
			m_keys(*this, "P%o", 030),
			m_joy(*this, "IKBD_JOY%u", 0U),
			m_mousex(*this, "IKBD_MOUSEX"),
			m_mousey(*this, "IKBD_MOUSEY"),
			m_config(*this, "config"),
			m_ikbd_mouse_x(0),
			m_ikbd_mouse_y(0),
			m_ikbd_mouse_px(IKBD_MOUSE_PHASE_STATIC),
			m_ikbd_mouse_py(IKBD_MOUSE_PHASE_STATIC),
			m_ikbd_mouse_pc(0),
			m_ikbd_joy(1),
			m_monochrome(1),
			m_video(*this, "video"),
			m_screen(*this, "screen"),
			m_led(*this, "led1")
	{ }

	DECLARE_WRITE_LINE_MEMBER( write_monochrome );

	void st(machine_config &config);

protected:
	required_device<m68000_base_device> m_maincpu;
	optional_device<st_blitter_device> m_stb;
	required_device<cpu_device> m_ikbd;
	required_device<wd1772_device> m_fdc;
	required_device_array<floppy_connector, 2> m_floppy;
	required_device<mc68901_device> m_mfp;
	required_device_array<acia6850_device, 2> m_acia;
	required_device<centronics_device> m_centronics;
	required_device<generic_slot_device> m_cart;
	required_device<ram_device> m_ram;
	required_device<rs232_port_device> m_rs232;
	required_device<ym2149_device> m_ymsnd;
	required_ioport_array<16> m_keys;
	optional_ioport_array<2> m_joy;
	optional_ioport m_mousex;
	optional_ioport m_mousey;
	optional_ioport m_config;

	TIMER_CALLBACK_MEMBER(mouse_tick);

	// driver
	uint16_t fdc_data_r(offs_t offset);
	void fdc_data_w(offs_t offset, uint16_t data);
	uint16_t dma_status_r();
	void dma_mode_w(uint16_t data);
	uint8_t dma_counter_r(offs_t offset);
	void dma_base_w(offs_t offset, uint8_t data);
	uint8_t mmu_r();
	void mmu_w(uint8_t data);
	uint16_t berr_r();
	void berr_w(uint16_t data);
	uint8_t ikbd_port1_r();
	uint8_t ikbd_port2_r();
	void ikbd_port2_w(uint8_t data);
	void ikbd_port3_w(uint8_t data);
	uint8_t ikbd_port4_r();
	void ikbd_port4_w(uint8_t data);

	DECLARE_WRITE_LINE_MEMBER( fdc_drq_w );

	void psg_pa_w(uint8_t data);

	DECLARE_WRITE_LINE_MEMBER( ikbd_tx_w );

	DECLARE_WRITE_LINE_MEMBER( reset_w );

	void toggle_dma_fifo();
	void flush_dma_fifo();
	void fill_dma_fifo();
	void fdc_dma_transfer();

	void configure_memory();
	void state_save();

	/* memory state */
	uint8_t m_mmu = 0U;

	/* keyboard state */
	uint16_t m_ikbd_keylatch = 0U;
	uint8_t m_ikbd_mouse = 0U;
	uint8_t m_ikbd_mouse_x;
	uint8_t m_ikbd_mouse_y;
	uint8_t m_ikbd_mouse_px;
	uint8_t m_ikbd_mouse_py;
	uint8_t m_ikbd_mouse_pc;
	int m_ikbd_tx = 0;
	int m_ikbd_joy;
	int m_midi_tx = 0;

	/* floppy state */
	uint32_t m_dma_base = 0U;
	uint16_t m_dma_error = 0U;
	uint16_t m_fdc_mode = 0U;
	uint8_t m_fdc_sectors = 0U;
	uint16_t m_fdc_fifo[2][8]{};
	int m_fdc_fifo_sel = 0;
	int m_fdc_fifo_index = 0;
	int m_fdc_fifo_msb = 0;
	int m_fdc_fifo_empty[2]{};
	int m_fdc_dmabytes = 0;

	/* timers */
	emu_timer *m_mouse_timer = nullptr;

	static void floppy_formats(format_registration &fr);

	int m_monochrome;
	required_device<st_video_device> m_video;
	required_device<screen_device> m_screen;

	void common(machine_config &config);
	void ikbd_map(address_map &map);
	void cpu_space_map(address_map &map);
	void st_map(address_map &map);
	void megast_map(address_map &map);
	void keyboard(machine_config &config);

	uint16_t fpu_r();
	void fpu_w(uint16_t data);

	virtual void machine_start() override;

	output_finder<> m_led;
};

class megast_state : public st_state
{
public:
	megast_state(const machine_config &mconfig, device_type type, const char *tag)
		: st_state(mconfig, type, tag)
	{ }

	void megast(machine_config &config);
};

class ste_state : public st_state
{
public:
	ste_state(const machine_config &mconfig, device_type type, const char *tag)
		: st_state(mconfig, type, tag),
			m_lmc1992(*this, LMC1992_TAG)
	{ }

	optional_device<lmc1992_device> m_lmc1992;

	uint8_t sound_dma_control_r();
	uint8_t sound_dma_base_r(offs_t offset);
	uint8_t sound_dma_counter_r(offs_t offset);
	uint8_t sound_dma_end_r(offs_t offset);
	uint8_t sound_mode_r();
	void sound_dma_control_w(uint8_t data);
	void sound_dma_base_w(offs_t offset, uint8_t data);
	void sound_dma_end_w(offs_t offset, uint8_t data);
	void sound_mode_w(uint8_t data);
	uint16_t microwire_data_r();
	void microwire_data_w(uint16_t data);
	uint16_t microwire_mask_r();
	void microwire_mask_w(uint16_t data);

	DECLARE_WRITE_LINE_MEMBER( write_monochrome );

	void dmasound_set_state(int level);
	TIMER_CALLBACK_MEMBER(dmasound_tick);
	void microwire_shift();
	TIMER_CALLBACK_MEMBER(microwire_tick);
	void state_save();

	/* microwire state */
	uint16_t m_mw_data = 0U;
	uint16_t m_mw_mask = 0U;
	int m_mw_shift = 0;

	/* DMA sound state */
	uint32_t m_dmasnd_base = 0U;
	uint32_t m_dmasnd_end = 0U;
	uint32_t m_dmasnd_cntr = 0U;
	uint32_t m_dmasnd_baselatch = 0U;
	uint32_t m_dmasnd_endlatch = 0U;
	uint8_t m_dmasnd_ctrl = 0U;
	uint8_t m_dmasnd_mode = 0U;
	uint8_t m_dmasnd_fifo[8]{};
	uint8_t m_dmasnd_samples = 0U;
	int m_dmasnd_active = 0;

	// timers
	emu_timer *m_microwire_timer = 0;
	emu_timer *m_dmasound_timer = 0;

	void falcon40(machine_config &config);
	void tt030(machine_config &config);
	void falcon(machine_config &config);
	void ste(machine_config &config);
	void ste_map(address_map &map);
protected:
	virtual void machine_start() override;
};

class megaste_state : public ste_state
{
public:
	megaste_state(const machine_config &mconfig, device_type type, const char *tag)
		: ste_state(mconfig, type, tag)
	{ }

	[[maybe_unused]] uint16_t cache_r();
	[[maybe_unused]] void cache_w(uint16_t data);

	uint16_t m_cache = 0;
	void megaste(machine_config &config);
	void megaste_map(address_map &map);

protected:
	virtual void machine_start() override;
};

class stbook_state : public ste_state
{
public:
	stbook_state(const machine_config &mconfig, device_type type, const char *tag)
		: ste_state(mconfig, type, tag),
			m_sw400(*this, "SW400")
	{ }

	required_ioport m_sw400;

	[[maybe_unused]] uint16_t config_r();
	[[maybe_unused]] void lcd_control_w(uint16_t data);

	[[maybe_unused]] void psg_pa_w(uint8_t data);
	uint8_t mfp_gpio_r();
	void stbook_map(address_map &map);
protected:
	virtual void machine_start() override;
};


//**************************************************************************
//  FLOPPY
//**************************************************************************

//-------------------------------------------------
//  toggle_dma_fifo -
//-------------------------------------------------

void st_state::toggle_dma_fifo()
{
	if (LOG) logerror("Toggling DMA FIFO\n");

	m_fdc_fifo_sel = !m_fdc_fifo_sel;
	m_fdc_fifo_index = 0;
}


//-------------------------------------------------
//  flush_dma_fifo -
//-------------------------------------------------

void st_state::flush_dma_fifo()
{
	if (m_fdc_fifo_empty[m_fdc_fifo_sel]) return;

	if (m_fdc_dmabytes)
	{
		address_space &program = m_maincpu->space(AS_PROGRAM);
		for (int i = 0; i < 8; i++)
		{
			uint16_t data = m_fdc_fifo[m_fdc_fifo_sel][i];

			if (LOG) logerror("Flushing DMA FIFO %u data %04x to address %06x\n", m_fdc_fifo_sel, data, m_dma_base);

			if (m_dma_base >= 8)
				program.write_word(m_dma_base, data);
			m_dma_base += 2;
		}
		m_fdc_dmabytes -= 16;
		if (!m_fdc_dmabytes)
		{
			m_fdc_sectors--;

			if (m_fdc_sectors)
				m_fdc_dmabytes = DMA_SECTOR_SIZE;
		}
	}
	else
		m_dma_error = 0;

	m_fdc_fifo_empty[m_fdc_fifo_sel] = 1;
}


//-------------------------------------------------
//  fill_dma_fifo -
//-------------------------------------------------

void st_state::fill_dma_fifo()
{
	if (m_fdc_dmabytes)
	{
		address_space &program = m_maincpu->space(AS_PROGRAM);
		for (int i = 0; i < 8; i++)
		{
			uint16_t data = program.read_word(m_dma_base);

			if (LOG) logerror("Filling DMA FIFO %u with data %04x from memory address %06x\n", m_fdc_fifo_sel, data, m_dma_base);

			m_fdc_fifo[m_fdc_fifo_sel][i] = data;
			m_dma_base += 2;
		}
		m_fdc_dmabytes -= 16;
		if (!m_fdc_dmabytes)
		{
			m_fdc_sectors--;

			if (m_fdc_sectors)
				m_fdc_dmabytes = DMA_SECTOR_SIZE;
		}
	}
	else
		m_dma_error = 0;

	m_fdc_fifo_empty[m_fdc_fifo_sel] = 0;
}


//-------------------------------------------------
//  fdc_dma_transfer -
//-------------------------------------------------

void st_state::fdc_dma_transfer()
{
	if (m_fdc_mode & DMA_MODE_READ_WRITE)
	{
		uint16_t data = m_fdc_fifo[m_fdc_fifo_sel][m_fdc_fifo_index];

		if (m_fdc_fifo_msb)
		{
			// write LSB to disk
			m_fdc->data_w(data & 0xff);

			if (LOG) logerror("DMA Write to FDC %02x\n", data & 0xff);

			m_fdc_fifo_index++;
		}
		else
		{
			// write MSB to disk
			m_fdc->data_w(data >> 8);

			if (LOG) logerror("DMA Write to FDC %02x\n", data >> 8);
		}

		// toggle MSB/LSB
		m_fdc_fifo_msb = !m_fdc_fifo_msb;

		if (m_fdc_fifo_index == 8)
		{
			m_fdc_fifo_index--;
			m_fdc_fifo_empty[m_fdc_fifo_sel] = 1;

			toggle_dma_fifo();

			if (m_fdc_fifo_empty[m_fdc_fifo_sel])
			{
				fill_dma_fifo();
			}
		}
	}
	else
	{
		// read from controller to FIFO
		uint8_t data = m_fdc->data_r();

		m_fdc_fifo_empty[m_fdc_fifo_sel] = 0;

		if (LOG) logerror("DMA Read from FDC %02x\n", data);

		if (m_fdc_fifo_msb)
		{
			// write MSB to FIFO
			m_fdc_fifo[m_fdc_fifo_sel][m_fdc_fifo_index] |= data;
			m_fdc_fifo_index++;
		}
		else
		{
			// write LSB to FIFO
			m_fdc_fifo[m_fdc_fifo_sel][m_fdc_fifo_index] = data << 8;
		}

		// toggle MSB/LSB
		m_fdc_fifo_msb = !m_fdc_fifo_msb;

		if (m_fdc_fifo_index == 8)
		{
			flush_dma_fifo();
			toggle_dma_fifo();
		}
	}
}


//-------------------------------------------------
//  fdc_data_r -
//-------------------------------------------------

uint16_t st_state::fdc_data_r(offs_t offset)
{
	uint8_t data = 0;

	if (m_fdc_mode & DMA_MODE_SECTOR_COUNT)
	{
		if (LOG) logerror("Indeterminate DMA Sector Count Read!\n");

		// sector count register is write only, reading it returns unpredictable values
		data = machine().rand() & 0xff;
	}
	else
	{
		if (!(m_fdc_mode & DMA_MODE_FDC_HDC_CS))
		{
			// floppy controller
			offs_t offset = (m_fdc_mode & DMA_MODE_ADDRESS_MASK) >> 1;

			data = m_fdc->read(offset);

			if (LOG) logerror("FDC Register %u Read %02x\n", offset, data);
		}
	}

	return data;
}


//-------------------------------------------------
//  fdc_data_w -
//-------------------------------------------------

void st_state::fdc_data_w(offs_t offset, uint16_t data)
{
	if (m_fdc_mode & DMA_MODE_SECTOR_COUNT)
	{
		if (LOG) logerror("DMA Sector Count %u\n", data);

		// sector count register
		m_fdc_sectors = data;

		if (m_fdc_sectors)
		{
			m_fdc_dmabytes = DMA_SECTOR_SIZE;
		}

		if (m_fdc_mode & DMA_MODE_READ_WRITE)
		{
			// fill both FIFOs with data
			fill_dma_fifo();
			toggle_dma_fifo();
			fill_dma_fifo();
			toggle_dma_fifo();
		}
	}
	else
	{
		if (!(m_fdc_mode & DMA_MODE_FDC_HDC_CS))
		{
			// floppy controller
			offs_t offset = (m_fdc_mode & DMA_MODE_ADDRESS_MASK) >> 1;

			if (LOG) logerror("FDC Register %u Write %02x\n", offset, data);

			m_fdc->write(offset, data);
		}
	}
}


//-------------------------------------------------
//  dma_status_r -
//-------------------------------------------------

uint16_t st_state::dma_status_r()
{
	uint16_t data = 0;

	// DMA error
	data |= m_dma_error;

	// sector count null
	data |= !(m_fdc_sectors == 0) << 1;

	// DRQ state
	data |= m_fdc->drq_r() << 2;

	return data;
}


//-------------------------------------------------
//  dma_mode_w -
//-------------------------------------------------

void st_state::dma_mode_w(uint16_t data)
{
	if (LOG) logerror("DMA Mode %04x\n", data);

	if ((data & DMA_MODE_READ_WRITE) != (m_fdc_mode & DMA_MODE_READ_WRITE))
	{
		if (LOG) logerror("DMA reset\n");

		m_dma_error = 1;
		m_fdc_sectors = 0;
		m_fdc_fifo_sel = 0;
		m_fdc_fifo_msb = 0;
		m_fdc_fifo_index = 0;
	}

	m_fdc_mode = data;
}


//-------------------------------------------------
//  dma_counter_r -
//-------------------------------------------------

uint8_t st_state::dma_counter_r(offs_t offset)
{
	uint8_t data = 0;

	switch (offset)
	{
	case 0:
		data = (m_dma_base >> 16) & 0xff;
		break;
	case 1:
		data = (m_dma_base >> 8) & 0xff;
		break;
	case 2:
		data = m_dma_base & 0xff;
		break;
	}

	return data;
}


//-------------------------------------------------
//  dma_base_w -
//-------------------------------------------------

void st_state::dma_base_w(offs_t offset, uint8_t data)
{
	switch (offset)
	{
	case 0:
		m_dma_base = (m_dma_base & 0x00ffff) | (data << 16);
		if (LOG) logerror("DMA Address High %02x (%06x)\n", data & 0xff, m_dma_base);
		break;

	case 1:
		m_dma_base = (m_dma_base & 0xff00ff) | (data << 8);
		if (LOG) logerror("DMA Address Mid %02x (%06x)\n", data & 0xff, m_dma_base);
		break;

	case 2:
		m_dma_base = (m_dma_base & 0xffff00) | data;
		if (LOG) logerror("DMA Address Low %02x (%06x)\n", data & 0xff, m_dma_base);
		break;
	}
}



//**************************************************************************
//  MMU
//**************************************************************************

//-------------------------------------------------
//  mmu_r -
//-------------------------------------------------

uint8_t st_state::mmu_r()
{
	return m_mmu;
}


//-------------------------------------------------
//  mmu_w -
//-------------------------------------------------

void st_state::mmu_w(uint8_t data)
{
	if (LOG) logerror("Memory Configuration Register: %02x\n", data);

	m_mmu = data;
}


void st_state::berr_w(uint16_t data)
{
	m_maincpu->set_input_line(M68K_LINE_BUSERROR, ASSERT_LINE);
	m_maincpu->set_input_line(M68K_LINE_BUSERROR, CLEAR_LINE);
}

uint16_t st_state::berr_r()
{
	if (!machine().side_effects_disabled())
	{
		m_maincpu->set_input_line(M68K_LINE_BUSERROR, ASSERT_LINE);
		m_maincpu->set_input_line(M68K_LINE_BUSERROR, CLEAR_LINE);
	}
	return 0xffff;
}


//**************************************************************************
//  IKBD
//**************************************************************************

//-------------------------------------------------
//  mouse_tick -
//-------------------------------------------------

TIMER_CALLBACK_MEMBER(st_state::mouse_tick)
{
	/*

	        Right   Left        Up      Down

	    XA  1100    0110    YA  1100    0110
	    XB  0110    1100    YB  0110    1100

	*/

	uint8_t x = m_mousex->read();
	uint8_t y = m_mousey->read();

	if (m_ikbd_mouse_pc == 0)
	{
		if (x == m_ikbd_mouse_x)
		{
			m_ikbd_mouse_px = IKBD_MOUSE_PHASE_STATIC;
		}
		else if ((x > m_ikbd_mouse_x) || (x == 0 && m_ikbd_mouse_x == 0xff))
		{
			m_ikbd_mouse_px = IKBD_MOUSE_PHASE_POSITIVE;
		}
		else if ((x < m_ikbd_mouse_x) || (x == 0xff && m_ikbd_mouse_x == 0))
		{
			m_ikbd_mouse_px = IKBD_MOUSE_PHASE_NEGATIVE;
		}

		if (y == m_ikbd_mouse_y)
		{
			m_ikbd_mouse_py = IKBD_MOUSE_PHASE_STATIC;
		}
		else if ((y > m_ikbd_mouse_y) || (y == 0 && m_ikbd_mouse_y == 0xff))
		{
			m_ikbd_mouse_py = IKBD_MOUSE_PHASE_POSITIVE;
		}
		else if ((y < m_ikbd_mouse_y) || (y == 0xff && m_ikbd_mouse_y == 0))
		{
			m_ikbd_mouse_py = IKBD_MOUSE_PHASE_NEGATIVE;
		}

		m_ikbd_mouse_x = x;
		m_ikbd_mouse_y = y;
	}

	m_ikbd_mouse = 0;

	m_ikbd_mouse |= IKBD_MOUSE_XYB[m_ikbd_mouse_px][m_ikbd_mouse_pc];      // XB
	m_ikbd_mouse |= IKBD_MOUSE_XYA[m_ikbd_mouse_px][m_ikbd_mouse_pc] << 1; // XA
	m_ikbd_mouse |= IKBD_MOUSE_XYB[m_ikbd_mouse_py][m_ikbd_mouse_pc] << 2; // YA
	m_ikbd_mouse |= IKBD_MOUSE_XYA[m_ikbd_mouse_py][m_ikbd_mouse_pc] << 3; // YB

	m_ikbd_mouse_pc++;
	m_ikbd_mouse_pc &= 0x03;
}


//-------------------------------------------------
//  ikbd_port1_r -
//-------------------------------------------------

uint8_t st_state::ikbd_port1_r()
{
	/*

	    bit     description

	    0       Keyboard column input
	    1       Keyboard column input
	    2       Keyboard column input
	    3       Keyboard column input
	    4       Keyboard column input
	    5       Keyboard column input
	    6       Keyboard column input
	    7       Keyboard column input

	*/

	uint8_t data = 0xff;

	// keyboard data
	for (int i = 1; i < 16; i++)
		if (!BIT(m_ikbd_keylatch, i))
			data &= m_keys[i]->read();

	return data;
}


//-------------------------------------------------
//  ikbd_port2_r -
//-------------------------------------------------

uint8_t st_state::ikbd_port2_r()
{
	/*

	    bit     description

	    0       JOY 1-5
	    1       JOY 0-6
	    2       JOY 1-6
	    3       SD FROM CPU
	    4

	*/

	uint8_t data = m_joy[1].read_safe(0x06) & 0x06;

	// serial receive
	data |= m_ikbd_tx << 3;

	return data;
}


//-------------------------------------------------
//  ikbd_port2_w -
//-------------------------------------------------

void st_state::ikbd_port2_w(uint8_t data)
{
	/*

	    bit     description

	    0       joystick enable
	    1
	    2
	    3
	    4       SD TO CPU

	*/

	// joystick enable
	m_ikbd_joy = BIT(data, 0);

	// serial transmit
	m_acia[0]->write_rxd(BIT(data, 4));
}


//-------------------------------------------------
//  ikbd_port3_w -
//-------------------------------------------------

void st_state::ikbd_port3_w(uint8_t data)
{
	/*

	    bit     description

	    0       CAPS LOCK LED
	    1       Keyboard row select
	    2       Keyboard row select
	    3       Keyboard row select
	    4       Keyboard row select
	    5       Keyboard row select
	    6       Keyboard row select
	    7       Keyboard row select

	*/

	// caps lock led
	m_led = BIT(data, 0);

	// keyboard row select
	m_ikbd_keylatch = (m_ikbd_keylatch & 0xff00) | data;
}


//-------------------------------------------------
//  ikbd_port4_r -
//-------------------------------------------------

uint8_t st_state::ikbd_port4_r()
{
	/*

	    bit     description

	    0       JOY 0-1 or mouse XB
	    1       JOY 0-2 or mouse XA
	    2       JOY 0-3 or mouse YA
	    3       JOY 0-4 or mouse YB
	    4       JOY 1-1
	    5       JOY 1-2
	    6       JOY 1-3
	    7       JOY 1-4

	*/

	if (m_ikbd_joy) return 0xff;

	uint8_t data = m_joy[0].read_safe(0xff);

	if ((m_config->read() & 0x01) == 0)
	{
		data = (data & 0xf0) | m_ikbd_mouse;
	}

	return data;
}


//-------------------------------------------------
//  ikbd_port4_w -
//-------------------------------------------------

void st_state::ikbd_port4_w(uint8_t data)
{
	/*

	    bit     description

	    0       Keyboard row select
	    1       Keyboard row select
	    2       Keyboard row select
	    3       Keyboard row select
	    4       Keyboard row select
	    5       Keyboard row select
	    6       Keyboard row select
	    7       Keyboard row select

	*/

	// keyboard row select
	m_ikbd_keylatch = (data << 8) | (m_ikbd_keylatch & 0xff);
}



//**************************************************************************
//  FPU
//**************************************************************************

//-------------------------------------------------
//  fpu_r -
//-------------------------------------------------

uint16_t st_state::fpu_r()
{
	// HACK diagnostic cartridge wants to see this value
	return 0x0802;
}


void st_state::fpu_w(uint16_t data)
{
}

WRITE_LINE_MEMBER( st_state::write_monochrome )
{
	m_monochrome = state;
	m_mfp->i7_w(m_monochrome);
}

WRITE_LINE_MEMBER( st_state::reset_w )
{
	m_video->reset();
	if (m_stb.found())
		m_stb->reset();
	m_mfp->reset();
	m_ikbd->pulse_input_line(INPUT_LINE_RESET, attotime::zero);
	m_ymsnd->reset();
	m_fdc->soft_reset();
	//m_acsi->reset();
}



//**************************************************************************
//  DMA SOUND
//**************************************************************************

//-------------------------------------------------
//  dmasound_set_state -
//-------------------------------------------------

void ste_state::dmasound_set_state(int level)
{
	m_dmasnd_active = level;
	m_mfp->tai_w(m_dmasnd_active);
	m_mfp->i7_w(m_monochrome ^ m_dmasnd_active);

	if (level == 0)
	{
		m_dmasnd_baselatch = m_dmasnd_base;
		m_dmasnd_endlatch = m_dmasnd_end;
	}
	else
	{
		m_dmasnd_cntr = m_dmasnd_baselatch;
	}
}


WRITE_LINE_MEMBER( ste_state::write_monochrome )
{
	m_monochrome = state;
	m_mfp->i7_w(m_monochrome ^ m_dmasnd_active);
}

//-------------------------------------------------
//  dmasound_tick -
//-------------------------------------------------

TIMER_CALLBACK_MEMBER(ste_state::dmasound_tick)
{
	if (m_dmasnd_samples == 0)
	{
		uint8_t *RAM = m_ram->pointer();

		for (auto & elem : m_dmasnd_fifo)
		{
			elem = RAM[m_dmasnd_cntr];
			m_dmasnd_cntr++;
			m_dmasnd_samples++;

			if (m_dmasnd_cntr == m_dmasnd_endlatch)
			{
				dmasound_set_state(0);
				break;
			}
		}
	}

	if (m_dmasnd_ctrl & 0x80)
	{
		if (LOG) logerror("DMA sound left  %i\n", m_dmasnd_fifo[7 - m_dmasnd_samples]);
		m_dmasnd_samples--;

		if (LOG) logerror("DMA sound right %i\n", m_dmasnd_fifo[7 - m_dmasnd_samples]);
		m_dmasnd_samples--;
	}
	else
	{
		if (LOG) logerror("DMA sound mono %i\n", m_dmasnd_fifo[7 - m_dmasnd_samples]);
		m_dmasnd_samples--;
	}

	if ((m_dmasnd_samples == 0) && (m_dmasnd_active == 0))
	{
		if ((m_dmasnd_ctrl & 0x03) == 0x03)
		{
			dmasound_set_state(1);
		}
		else
		{
			m_dmasound_timer->enable(0);
		}
	}
}


//-------------------------------------------------
//  sound_dma_control_r -
//-------------------------------------------------

uint8_t ste_state::sound_dma_control_r()
{
	return m_dmasnd_ctrl;
}


//-------------------------------------------------
//  sound_dma_base_r -
//-------------------------------------------------

uint8_t ste_state::sound_dma_base_r(offs_t offset)
{
	uint8_t data = 0;

	switch (offset)
	{
	case 0x00:
		data = (m_dmasnd_base >> 16) & 0x3f;
		break;

	case 0x01:
		data = (m_dmasnd_base >> 8) & 0xff;
		break;

	case 0x02:
		data = m_dmasnd_base & 0xff;
		break;
	}

	return data;
}


//-------------------------------------------------
//  sound_dma_counter_r -
//-------------------------------------------------

uint8_t ste_state::sound_dma_counter_r(offs_t offset)
{
	uint8_t data = 0;

	switch (offset)
	{
	case 0x00:
		data = (m_dmasnd_cntr >> 16) & 0x3f;
		break;

	case 0x01:
		data = (m_dmasnd_cntr >> 8) & 0xff;
		break;

	case 0x02:
		data = m_dmasnd_cntr & 0xff;
		break;
	}

	return data;
}


//-------------------------------------------------
//  sound_dma_end_r -
//-------------------------------------------------

uint8_t ste_state::sound_dma_end_r(offs_t offset)
{
	uint8_t data = 0;

	switch (offset)
	{
	case 0x00:
		data = (m_dmasnd_end >> 16) & 0x3f;
		break;

	case 0x01:
		data = (m_dmasnd_end >> 8) & 0xff;
		break;

	case 0x02:
		data = m_dmasnd_end & 0xff;
		break;
	}

	return data;
}


//-------------------------------------------------
//  sound_mode_r -
//-------------------------------------------------

uint8_t ste_state::sound_mode_r()
{
	return m_dmasnd_mode;
}


//-------------------------------------------------
//  sound_dma_control_w -
//-------------------------------------------------

void ste_state::sound_dma_control_w(uint8_t data)
{
	m_dmasnd_ctrl = data & 0x03;

	if (m_dmasnd_ctrl & 0x01)
	{
		if (!m_dmasnd_active)
		{
			dmasound_set_state(1);
			m_dmasound_timer->adjust(attotime::zero, 0, attotime::from_hz(DMASOUND_RATE[m_dmasnd_mode & 0x03]));
		}
	}
	else
	{
		dmasound_set_state(0);
		m_dmasound_timer->enable(0);
	}
}


//-------------------------------------------------
//  sound_dma_base_w -
//-------------------------------------------------

void ste_state::sound_dma_base_w(offs_t offset, uint8_t data)
{
	switch (offset)
	{
	case 0x00:
		m_dmasnd_base = (data << 16) & 0x3f0000;
		break;
	case 0x01:
		m_dmasnd_base = (m_dmasnd_base & 0x3f00fe) | (data << 8);
		break;
	case 0x02:
		m_dmasnd_base = (m_dmasnd_base & 0x3fff00) | (data & 0xfe);
		break;
	}

	if (!m_dmasnd_active)
	{
		m_dmasnd_baselatch = m_dmasnd_base;
	}
}


//-------------------------------------------------
//  sound_dma_end_w -
//-------------------------------------------------

void ste_state::sound_dma_end_w(offs_t offset, uint8_t data)
{
	switch (offset)
	{
	case 0x00:
		m_dmasnd_end = (data << 16) & 0x3f0000;
		break;
	case 0x01:
		m_dmasnd_end = (m_dmasnd_end & 0x3f00fe) | (data & 0xff) << 8;
		break;
	case 0x02:
		m_dmasnd_end = (m_dmasnd_end & 0x3fff00) | (data & 0xfe);
		break;
	}

	if (!m_dmasnd_active)
	{
		m_dmasnd_endlatch = m_dmasnd_end;
	}
}


//-------------------------------------------------
//  sound_mode_w -
//-------------------------------------------------

void ste_state::sound_mode_w(uint8_t data)
{
	m_dmasnd_mode = data & 0x83;
}



//**************************************************************************
//  MICROWIRE
//**************************************************************************

//-------------------------------------------------
//  microwire_shift -
//-------------------------------------------------

void ste_state::microwire_shift()
{
	if (BIT(m_mw_mask, 15))
	{
		m_lmc1992->data_w(BIT(m_mw_data, 15));
		m_lmc1992->clock_w(1);
		m_lmc1992->clock_w(0);
	}

	// rotate mask and data left
	m_mw_mask = (m_mw_mask << 1) | BIT(m_mw_mask, 15);
	m_mw_data = (m_mw_data << 1) | BIT(m_mw_data, 15);
	m_mw_shift++;
}


//-------------------------------------------------
//  microwire_tick -
//-------------------------------------------------

TIMER_CALLBACK_MEMBER(ste_state::microwire_tick)
{
	switch (m_mw_shift)
	{
	case 0:
		m_lmc1992->enable_w(0);
		microwire_shift();
		break;

	default:
		microwire_shift();
		break;

	case 15:
		microwire_shift();
		m_lmc1992->enable_w(1);
		m_mw_shift = 0;
		m_microwire_timer->adjust(attotime::never);
		break;
	}
}


//-------------------------------------------------
//  microwire_data_r -
//-------------------------------------------------

uint16_t ste_state::microwire_data_r()
{
	return m_mw_data;
}


//-------------------------------------------------
//  microwire_data_w -
//-------------------------------------------------

void ste_state::microwire_data_w(uint16_t data)
{
	if (!m_microwire_timer->running())
	{
		m_mw_data = data;
		m_microwire_timer->adjust(attotime::zero, 0, attotime::from_usec(2));
	}
}


//-------------------------------------------------
//  microwire_mask_r -
//-------------------------------------------------

uint16_t ste_state::microwire_mask_r()
{
	return m_mw_mask;
}


//-------------------------------------------------
//  microwire_mask_w -
//-------------------------------------------------

void ste_state::microwire_mask_w(uint16_t data)
{
	if (!m_microwire_timer->running())
	{
		m_mw_mask = data;
	}
}



//**************************************************************************
//  CACHE
//**************************************************************************

//-------------------------------------------------
//  cache_r -
//-------------------------------------------------

uint16_t megaste_state::cache_r()
{
	return m_cache;
}


//-------------------------------------------------
//  cache_w -
//-------------------------------------------------

void megaste_state::cache_w(uint16_t data)
{
	m_cache = data;

	m_maincpu->set_unscaled_clock(BIT(data, 0) ? Y2/2 : Y2/4);
}



//**************************************************************************
//  STBOOK
//**************************************************************************

//-------------------------------------------------
//  config_r -
//-------------------------------------------------

uint16_t stbook_state::config_r()
{
	/*

	    bit     description

	    0       _POWER_SWITCH
	    1       _TOP_CLOSED
	    2       _RTC_ALARM
	    3       _SOURCE_DEAD
	    4       _SOURCE_LOW
	    5       _MODEM_WAKE
	    6       (reserved)
	    7       _EXPANSION_WAKE
	    8       (reserved)
	    9       (reserved)
	    10      (reserved)
	    11      (reserved)
	    12      (reserved)
	    13      SELF TEST
	    14      LOW SPEED FLOPPY
	    15      DMA AVAILABLE

	*/

	return (m_sw400->read() << 8) | 0xff;
}


//-------------------------------------------------
//  lcd_control_w -
//-------------------------------------------------

void stbook_state::lcd_control_w(uint16_t data)
{
	/*

	    bit     description

	    0       Shadow Chip OFF
	    1       _SHIFTER OFF
	    2       POWEROFF
	    3       _22ON
	    4       RS-232_OFF
	    5       (reserved)
	    6       (reserved)
	    7       MTR_PWR_ON

	*/
}


//**************************************************************************
//  ADDRESS MAPS
//**************************************************************************

//-------------------------------------------------
//  ADDRESS_MAP( ikbd_map )
//-------------------------------------------------

void st_state::ikbd_map(address_map &map)
{
	map(0x0000, 0x001f).m(HD6301V1_TAG, FUNC(hd6301_cpu_device::m6801_io));
	map(0x0080, 0x00ff).ram();
	map(0xf000, 0xffff).rom().region(HD6301V1_TAG, 0);
}


//-------------------------------------------------
//  ADDRESS_MAP( cpu_space_map )
//-------------------------------------------------

void st_state::cpu_space_map(address_map &map)
{
	map(0xfffff0, 0xffffff).m(m_maincpu, FUNC(m68000_base_device::autovectors_map));
	map(0xfffffd, 0xfffffd).r(m_mfp, FUNC(mc68901_device::get_vector));
}

//-------------------------------------------------
//  ADDRESS_MAP( st_map )
//-------------------------------------------------

void st_state::st_map(address_map &map)
{
	map.unmap_value_high();
	map(0x000000, 0x000007).rom().region(M68000_TAG, 0).w(FUNC(st_state::berr_w));
	map(0x000008, 0x1fffff).ram();
	map(0x200000, 0x3fffff).ram();
	map(0x400000, 0xf9ffff).rw(FUNC(st_state::berr_r), FUNC(st_state::berr_w));
	//map(0xfa0000, 0xfbffff)      // mapped by the cartslot
	map(0xfc0000, 0xfeffff).rom().region(M68000_TAG, 0).w(FUNC(st_state::berr_w));
	map(0xff8001, 0xff8001).rw(FUNC(st_state::mmu_r), FUNC(st_state::mmu_w));
	map(0xff8200, 0xff8203).rw(m_video, FUNC(st_video_device::shifter_base_r), FUNC(st_video_device::shifter_base_w)).umask16(0x00ff);
	map(0xff8204, 0xff8209).r(m_video, FUNC(st_video_device::shifter_counter_r)).umask16(0x00ff);
	map(0xff820a, 0xff820a).rw(m_video, FUNC(st_video_device::shifter_sync_r), FUNC(st_video_device::shifter_sync_w));
	map(0xff8240, 0xff825f).rw(m_video, FUNC(st_video_device::shifter_palette_r), FUNC(st_video_device::shifter_palette_w));
	map(0xff8260, 0xff8260).rw(m_video, FUNC(st_video_device::shifter_mode_r), FUNC(st_video_device::shifter_mode_w));
	map(0xff8604, 0xff8605).rw(FUNC(st_state::fdc_data_r), FUNC(st_state::fdc_data_w));
	map(0xff8606, 0xff8607).rw(FUNC(st_state::dma_status_r), FUNC(st_state::dma_mode_w));
	map(0xff8608, 0xff860d).rw(FUNC(st_state::dma_counter_r), FUNC(st_state::dma_base_w)).umask16(0x00ff);
	map(0xff8800, 0xff8800).rw(YM2149_TAG, FUNC(ay8910_device::data_r), FUNC(ay8910_device::address_w)).mirror(0xfc);
	map(0xff8802, 0xff8802).rw(YM2149_TAG, FUNC(ay8910_device::data_r), FUNC(ay8910_device::data_w)).mirror(0xfc);
	// no blitter on original ST
	map(0xfffa00, 0xfffa3f).rw(m_mfp, FUNC(mc68901_device::read), FUNC(mc68901_device::write)).umask16(0x00ff);
	map(0xfffc00, 0xfffc03).rw(m_acia[0], FUNC(acia6850_device::read), FUNC(acia6850_device::write)).umask16(0xff00);
	map(0xfffc04, 0xfffc07).rw(m_acia[1], FUNC(acia6850_device::read), FUNC(acia6850_device::write)).umask16(0xff00);
}


//-------------------------------------------------
//  ADDRESS_MAP( megast_map )
//-------------------------------------------------

void st_state::megast_map(address_map &map)
{
	map.unmap_value_high();
	map(0x000000, 0x000007).rom().region(M68000_TAG, 0);
	map(0x000008, 0x1fffff).ram();
	map(0x200000, 0x3fffff).ram();
	//map(0xfa0000, 0xfbffff)      // mapped by the cartslot
	map(0xfc0000, 0xfeffff).rom().region(M68000_TAG, 0);
//  map(0xff7f30, 0xff7f31).rw(m_stb, FUNC(st_blitter_device::dst_inc_y_r), FUNC(st_blitter_device::dst_inc_y_w) // for TOS 1.02
	map(0xff8001, 0xff8001).rw(FUNC(st_state::mmu_r), FUNC(st_state::mmu_w));
	map(0xff8200, 0xff8203).rw(m_video, FUNC(st_video_device::shifter_base_r), FUNC(st_video_device::shifter_base_w)).umask16(0x00ff);
	map(0xff8204, 0xff8209).r(m_video, FUNC(st_video_device::shifter_counter_r)).umask16(0x00ff);
	map(0xff820a, 0xff820a).rw(m_video, FUNC(st_video_device::shifter_sync_r), FUNC(st_video_device::shifter_sync_w));
	map(0xff8240, 0xff825f).rw(m_video, FUNC(st_video_device::shifter_palette_r), FUNC(st_video_device::shifter_palette_w));
	map(0xff8260, 0xff8260).rw(m_video, FUNC(st_video_device::shifter_mode_r), FUNC(st_video_device::shifter_mode_w));
	map(0xff8604, 0xff8605).rw(FUNC(st_state::fdc_data_r), FUNC(st_state::fdc_data_w));
	map(0xff8606, 0xff8607).rw(FUNC(st_state::dma_status_r), FUNC(st_state::dma_mode_w));
	map(0xff8608, 0xff860d).rw(FUNC(st_state::dma_counter_r), FUNC(st_state::dma_base_w)).umask16(0x00ff);
	map(0xff8800, 0xff8800).rw(YM2149_TAG, FUNC(ay8910_device::data_r), FUNC(ay8910_device::address_w));
	map(0xff8802, 0xff8802).w(YM2149_TAG, FUNC(ay8910_device::data_w));
	map(0xff8a00, 0xff8a1f).rw(m_stb, FUNC(st_blitter_device::halftone_r), FUNC(st_blitter_device::halftone_w));
	map(0xff8a20, 0xff8a21).rw(m_stb, FUNC(st_blitter_device::src_inc_x_r), FUNC(st_blitter_device::src_inc_x_w));
	map(0xff8a22, 0xff8a23).rw(m_stb, FUNC(st_blitter_device::src_inc_y_r), FUNC(st_blitter_device::src_inc_y_w));
	map(0xff8a24, 0xff8a27).rw(m_stb, FUNC(st_blitter_device::src_r), FUNC(st_blitter_device::src_w));
	map(0xff8a28, 0xff8a2d).rw(m_stb, FUNC(st_blitter_device::end_mask_r), FUNC(st_blitter_device::end_mask_w));
	map(0xff8a2e, 0xff8a2f).rw(m_stb, FUNC(st_blitter_device::dst_inc_x_r), FUNC(st_blitter_device::dst_inc_x_w));
	map(0xff8a30, 0xff8a31).rw(m_stb, FUNC(st_blitter_device::dst_inc_y_r), FUNC(st_blitter_device::dst_inc_y_w));
	map(0xff8a32, 0xff8a35).rw(m_stb, FUNC(st_blitter_device::dst_r), FUNC(st_blitter_device::dst_w));
	map(0xff8a36, 0xff8a37).rw(m_stb, FUNC(st_blitter_device::count_x_r), FUNC(st_blitter_device::count_x_w));
	map(0xff8a38, 0xff8a39).rw(m_stb, FUNC(st_blitter_device::count_y_r), FUNC(st_blitter_device::count_y_w));
	map(0xff8a3a, 0xff8a3b).rw(m_stb, FUNC(st_blitter_device::op_r), FUNC(st_blitter_device::op_w));
	map(0xff8a3c, 0xff8a3d).rw(m_stb, FUNC(st_blitter_device::ctrl_r), FUNC(st_blitter_device::ctrl_w));
	map(0xfffa00, 0xfffa3f).rw(m_mfp, FUNC(mc68901_device::read), FUNC(mc68901_device::write)).umask16(0x00ff);
	map(0xfffa40, 0xfffa57).rw(FUNC(st_state::fpu_r), FUNC(st_state::fpu_w));
	map(0xfffc00, 0xfffc03).rw(m_acia[0], FUNC(acia6850_device::read), FUNC(acia6850_device::write)).umask16(0xff00);
	map(0xfffc04, 0xfffc07).rw(m_acia[1], FUNC(acia6850_device::read), FUNC(acia6850_device::write)).umask16(0xff00);
	map(0xfffc20, 0xfffc3f).rw(RP5C15_TAG, FUNC(rp5c15_device::read), FUNC(rp5c15_device::write)).umask16(0x00ff);
}


//-------------------------------------------------
//  ADDRESS_MAP( ste_map )
//-------------------------------------------------

void ste_state::ste_map(address_map &map)
{
	st_map(map);
	map(0xe00000, 0xe3ffff).rom().region(M68000_TAG, 0);
	map(0xff8901, 0xff8901).rw(FUNC(ste_state::sound_dma_control_r), FUNC(ste_state::sound_dma_control_w));
	map(0xff8902, 0xff8907).rw(FUNC(ste_state::sound_dma_base_r), FUNC(ste_state::sound_dma_base_w)).umask16(0x00ff);
	map(0xff8908, 0xff890d).r(FUNC(ste_state::sound_dma_counter_r)).umask16(0x00ff);
	map(0xff890e, 0xff8913).rw(FUNC(ste_state::sound_dma_end_r), FUNC(ste_state::sound_dma_end_w)).umask16(0x00ff);
	map(0xff8921, 0xff8921).rw(FUNC(ste_state::sound_mode_r), FUNC(ste_state::sound_mode_w));
	map(0xff8922, 0xff8923).rw(FUNC(ste_state::microwire_data_r), FUNC(ste_state::microwire_data_w));
	map(0xff8924, 0xff8925).rw(FUNC(ste_state::microwire_mask_r), FUNC(ste_state::microwire_mask_w));
	map(0xff8a00, 0xff8a1f).rw(m_stb, FUNC(st_blitter_device::halftone_r), FUNC(st_blitter_device::halftone_w));
	map(0xff8a20, 0xff8a21).rw(m_stb, FUNC(st_blitter_device::src_inc_x_r), FUNC(st_blitter_device::src_inc_x_w));
	map(0xff8a22, 0xff8a23).rw(m_stb, FUNC(st_blitter_device::src_inc_y_r), FUNC(st_blitter_device::src_inc_y_w));
	map(0xff8a24, 0xff8a27).rw(m_stb, FUNC(st_blitter_device::src_r), FUNC(st_blitter_device::src_w));
	map(0xff8a28, 0xff8a2d).rw(m_stb, FUNC(st_blitter_device::end_mask_r), FUNC(st_blitter_device::end_mask_w));
	map(0xff8a2e, 0xff8a2f).rw(m_stb, FUNC(st_blitter_device::dst_inc_x_r), FUNC(st_blitter_device::dst_inc_x_w));
	map(0xff8a30, 0xff8a31).rw(m_stb, FUNC(st_blitter_device::dst_inc_y_r), FUNC(st_blitter_device::dst_inc_y_w));
	map(0xff8a32, 0xff8a35).rw(m_stb, FUNC(st_blitter_device::dst_r), FUNC(st_blitter_device::dst_w));
	map(0xff8a36, 0xff8a37).rw(m_stb, FUNC(st_blitter_device::count_x_r), FUNC(st_blitter_device::count_x_w));
	map(0xff8a38, 0xff8a39).rw(m_stb, FUNC(st_blitter_device::count_y_r), FUNC(st_blitter_device::count_y_w));
	map(0xff8a3a, 0xff8a3b).rw(m_stb, FUNC(st_blitter_device::op_r), FUNC(st_blitter_device::op_w));
	map(0xff8a3c, 0xff8a3d).rw(m_stb, FUNC(st_blitter_device::ctrl_r), FUNC(st_blitter_device::ctrl_w));
	map(0xff9200, 0xff9201).portr("JOY0");
	map(0xff9202, 0xff9203).portr("JOY1");
	map(0xff9210, 0xff9211).portr("PADDLE0X");
	map(0xff9212, 0xff9213).portr("PADDLE0Y");
	map(0xff9214, 0xff9215).portr("PADDLE1X");
	map(0xff9216, 0xff9217).portr("PADDLE1Y");
	map(0xff9220, 0xff9221).portr("GUNX");
	map(0xff9222, 0xff9223).portr("GUNY");
}


//-------------------------------------------------
//  ADDRESS_MAP( megaste_map )
//-------------------------------------------------

void megaste_state::megaste_map(address_map &map)
{
	megast_map(map);
	map(0xe00000, 0xe3ffff).rom().region(M68000_TAG, 0);
	map(0xff8c80, 0xff8c87).rw(Z8530_TAG, FUNC(scc8530_legacy_device::reg_r), FUNC(scc8530_legacy_device::reg_w)).umask16(0x00ff);
	map(0xff8901, 0xff8901).rw(FUNC(megaste_state::sound_dma_control_r), FUNC(megaste_state::sound_dma_control_w));
	map(0xff8902, 0xff8907).rw(FUNC(megaste_state::sound_dma_base_r), FUNC(megaste_state::sound_dma_base_w)).umask16(0x00ff);
	map(0xff8908, 0xff890d).r(FUNC(megaste_state::sound_dma_counter_r)).umask16(0x00ff);
	map(0xff890e, 0xff8913).rw(FUNC(megaste_state::sound_dma_end_r), FUNC(megaste_state::sound_dma_end_w)).umask16(0x00ff);
	map(0xff8921, 0xff8921).rw(FUNC(megaste_state::sound_mode_r), FUNC(megaste_state::sound_mode_w));
	map(0xff8922, 0xff8923).rw(FUNC(megaste_state::microwire_data_r), FUNC(megaste_state::microwire_data_w));
	map(0xff8924, 0xff8925).rw(FUNC(megaste_state::microwire_mask_r), FUNC(megaste_state::microwire_mask_w));
}


//-------------------------------------------------
//  ADDRESS_MAP( stbook_map )
//-------------------------------------------------
#if 0
void stbook_state::stbook_map(address_map &map)
{
	map(0x000000, 0x1fffff).ram();
	map(0x200000, 0x3fffff).ram();
//  map(0xd40000, 0xd7ffff).rom();
	map(0xe00000, 0xe3ffff).rom().region(M68000_TAG, 0);
//  map(0xe80000, 0xebffff).rom();
//  map(0xfa0000, 0xfbffff).rom(); // cartridge
	map(0xfc0000, 0xfeffff).rom().region(M68000_TAG, 0);
/*  map(0xf00000, 0xf1ffff).rw(FUNC(stbook_state::stbook_ide_r), FUNC(stbook_state::stbook_ide_w));
    map(0xff8000, 0xff8001).rw(FUNC(stbook_state::stbook_mmu_r), FUNC(stbook_state::stbook_mmu_w));
    map(0xff8200, 0xff8203).rw(m_video, FUNC(stbook_video_device::stbook_shifter_base_r), FUNC(stbook_video_device::stbook_shifter_base_w));
    map(0xff8204, 0xff8209).rw(m_video, FUNC(stbook_video_device::stbook_shifter_counter_r), FUNC(stbook_video_device::stbook_shifter_counter_w));
    map(0xff820a, 0xff820a).rw(m_video, FUNC(stbook_video_device::stbook_shifter_sync_r), FUNC(stbook_video_device::stbook_shifter_sync_w));
    map(0xff820c, 0xff820d).rw(m_video, FUNC(stbook_video_device::stbook_shifter_base_low_r), FUNC(stbook_video_device::stbook_shifter_base_low_w));
    map(0xff820e, 0xff820f).rw(m_video, FUNC(stbook_video_device::stbook_shifter_lineofs_r), FUNC(stbook_video_device::stbook_shifter_lineofs_w));
    map(0xff8240, 0xff8241).rw(m_video, FUNC(stbook_video_device::stbook_shifter_palette_r), FUNC(stbook_video_device::stbook_shifter_palette_w));
    map(0xff8260, 0xff8260).rw(m_video, FUNC(stbook_video_device::stbook_shifter_mode_r), FUNC(stbook_video_device::stbook_shifter_mode_w));
    map(0xff8264, 0xff8265).rw(m_video, FUNC(stbook_video_device::stbook_shifter_pixelofs_r), FUNC(stbook_video_device::stbook_shifter_pixelofs_w));
    map(0xff827e, 0xff827f).w(m_video, FUNC(stbook_video_device::lcd_control_w));*/
	map(0xff8800, 0xff8800).rw(YM3439_TAG, FUNC(ay8910_device::data_r), FUNC(ay8910_device::address_w));
	map(0xff8802, 0xff8802).w(YM3439_TAG, FUNC(ay8910_device::data_w));
/*  map(0xff8901, 0xff8901).rw(FUNC(stbook_state::sound_dma_control_r), FUNC(stbook_state::sound_dma_control_w));
    map(0xff8902, 0xff8907).rw(FUNC(stbook_state::sound_dma_base_r), FUNC(stbook_state::sound_dma_base_w)).umask16(0x00ff);
    map(0xff8908, 0xff890d).r(FUNC(stbook_state::sound_dma_counter_r)).umask16(0x00ff);
    map(0xff890e, 0xff8913).rw(FUNC(stbook_state::sound_dma_end_r), FUNC(stbook_state::sound_dma_end_w)).umask16(0x00ff);
    map(0xff8921, 0xff8921).rw(FUNC(stbook_state::sound_mode_r), FUNC(stbook_state::sound_mode_w));
    map(0xff8922, 0xff8923).rw(FUNC(stbook_state::microwire_data_r), FUNC(stbook_state::microwire_data_w));
    map(0xff8924, 0xff8925).rw(FUNC(stbook_state::microwire_mask_r), FUNC(stbook_state::microwire_mask_w));
    map(0xff8a00, 0xff8a1f).rw(m_stb, FUNC(st_blitter_device::halftone_r), FUNC(st_blitter_device::halftone_w));
    map(0xff8a20, 0xff8a21).rw(m_stb, FUNC(st_blitter_device::src_inc_x_r), FUNC(st_blitter_device::src_inc_x_w));
    map(0xff8a22, 0xff8a23).rw(m_stb, FUNC(st_blitter_device::src_inc_y_r), FUNC(st_blitter_device::src_inc_y_w));
    map(0xff8a24, 0xff8a27).rw(m_stb, FUNC(st_blitter_device::src_r), FUNC(st_blitter_device::src_w));
    map(0xff8a28, 0xff8a2d).rw(m_stb, FUNC(st_blitter_device::end_mask_r), FUNC(st_blitter_device::end_mask_w));
    map(0xff8a2e, 0xff8a2f).rw(m_stb, FUNC(st_blitter_device::dst_inc_x_r), FUNC(st_blitter_device::dst_inc_x_w));
    map(0xff8a30, 0xff8a31).rw(m_stb, FUNC(st_blitter_device::dst_inc_y_r), FUNC(st_blitter_device::dst_inc_y_w));
    map(0xff8a32, 0xff8a35).rw(m_stb, FUNC(st_blitter_device::dst_r), FUNC(st_blitter_device::dst_w));
    map(0xff8a36, 0xff8a37).rw(m_stb, FUNC(st_blitter_device::count_x_r), FUNC(st_blitter_device::count_x_w));
    map(0xff8a38, 0xff8a39).rw(m_stb, FUNC(st_blitter_device::count_y_r), FUNC(st_blitter_device::count_y_w));
    map(0xff8a3a, 0xff8a3b).rw(m_stb, FUNC(st_blitter_device::op_r), FUNC(st_blitter_device::op_w));
    map(0xff8a3c, 0xff8a3d).rw(m_stb, FUNC(st_blitter_device::ctrl_r), FUNC(st_blitter_device::ctrl_w));
    map(0xff9200, 0xff9201).r(FUNC(stbook_state::config_r));
    map(0xff9202, 0xff9203).rw(FUNC(stbook_state::lcd_contrast_r), FUNC(stbook_state::lcd_contrast_w));
    map(0xff9210, 0xff9211).rw(FUNC(stbook_state::power_r), FUNC(stbook_state::power_w));
    map(0xff9214, 0xff9215).rw(FUNC(stbook_state::reference_r), FUNC(stbook_state::reference_w));*/
}
#endif


//**************************************************************************
//  INPUT PORTS
//**************************************************************************

//-------------------------------------------------
//  INPUT_PORTS( ikbd )
//-------------------------------------------------

static INPUT_PORTS_START( ikbd )
	PORT_START("P30")
	PORT_BIT( 0xff, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("P31")
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Control") PORT_CODE(KEYCODE_LCONTROL) PORT_CHAR(UCHAR_MAMEKEY(LCONTROL))
	PORT_BIT( 0xef, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("P32")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F1) PORT_NAME("F1")
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Left Shift") PORT_CODE(KEYCODE_LSHIFT) PORT_CHAR(UCHAR_SHIFT_1)
	PORT_BIT( 0xde, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("P33")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F2) PORT_NAME("F2")
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME(DEF_STR( Alternate )) PORT_CODE(KEYCODE_LALT) PORT_CHAR(UCHAR_MAMEKEY(LALT))
	PORT_BIT( 0xbe, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("P34")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F3) PORT_NAME("F3")
	PORT_BIT( 0x7e, IP_ACTIVE_LOW, IPT_UNUSED )
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Right Shift") PORT_CODE(KEYCODE_RSHIFT) PORT_CHAR(UCHAR_SHIFT_1)

	PORT_START("P35")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F4) PORT_NAME("F4")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Esc") PORT_CODE(KEYCODE_ESC)
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_1) PORT_CHAR('1') PORT_CHAR('!')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Tab") PORT_CODE(KEYCODE_TAB) PORT_CHAR('\t')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_Q) PORT_CHAR('Q')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_A) PORT_CHAR('A')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_Z) PORT_CHAR('Z')

	PORT_START("P36")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F5) PORT_NAME("F5")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_2) PORT_CHAR('2') PORT_CHAR('@')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_3) PORT_CHAR('3') PORT_CHAR('#')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_W) PORT_CHAR('W')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_E) PORT_CHAR('E')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_S) PORT_CHAR('S')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_D) PORT_CHAR('D')
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_X) PORT_CHAR('X')

	PORT_START("P37")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F6) PORT_NAME("F6")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_4) PORT_CHAR('4') PORT_CHAR('$')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_5) PORT_CHAR('5') PORT_CHAR('%')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_R) PORT_CHAR('R')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_T) PORT_CHAR('T')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F) PORT_CHAR('F')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_C) PORT_CHAR('C')
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_V) PORT_CHAR('V')

	PORT_START("P40")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F7) PORT_NAME("F7")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_6) PORT_CHAR('6') PORT_CHAR('&')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_7) PORT_CHAR('7') PORT_CHAR('\'')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_Y) PORT_CHAR('Y')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_G) PORT_CHAR('G')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_H) PORT_CHAR('H')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_B) PORT_CHAR('B')
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_N) PORT_CHAR('N')

	PORT_START("P41")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F8) PORT_NAME("F8")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_8) PORT_CHAR('8') PORT_CHAR('(')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_9) PORT_CHAR('9') PORT_CHAR(')')
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_U) PORT_CHAR('U')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_I) PORT_CHAR('I')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_J) PORT_CHAR('J')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_K) PORT_CHAR('K')
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_M) PORT_CHAR('M')

	PORT_START("P42")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F9) PORT_NAME("F9")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_0) PORT_CHAR('0') PORT_CHAR('=')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_O) PORT_CHAR('O')
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_P) PORT_CHAR('P')
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_L) PORT_CHAR('L')
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Space") PORT_CODE(KEYCODE_SPACE) PORT_CHAR(' ')

	PORT_START("P43")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_F10) PORT_NAME("F10")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_QUOTE) PORT_CHAR(0x00B4) PORT_CHAR('`')
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Caps Lock") PORT_CODE(KEYCODE_CAPSLOCK) PORT_CHAR(UCHAR_MAMEKEY(CAPSLOCK))

	PORT_START("P44")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Help") PORT_CODE(KEYCODE_F11)
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Backspace") PORT_CODE(KEYCODE_BACKSPACE) PORT_CHAR(8)
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Delete") PORT_CODE(KEYCODE_DEL)
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Insert") PORT_CODE(KEYCODE_INSERT)
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Return") PORT_CODE(KEYCODE_ENTER) PORT_CHAR(13)
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD )
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_CODE(KEYCODE_SLASH) PORT_CHAR('-') PORT_CHAR('_')

	PORT_START("P45")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Undo") PORT_CODE(KEYCODE_F12)
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME(UTF8_UP) PORT_CODE(KEYCODE_UP) PORT_CHAR(UCHAR_MAMEKEY(UP))
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Clr Home") PORT_CODE(KEYCODE_HOME) PORT_CHAR(UCHAR_MAMEKEY(HOME))
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME(UTF8_LEFT) PORT_CODE(KEYCODE_LEFT) PORT_CHAR(UCHAR_MAMEKEY(LEFT))
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME(UTF8_DOWN) PORT_CODE(KEYCODE_DOWN) PORT_CHAR(UCHAR_MAMEKEY(DOWN))
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME(UTF8_RIGHT) PORT_CODE(KEYCODE_RIGHT) PORT_CHAR(UCHAR_MAMEKEY(RIGHT))
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 1") PORT_CODE(KEYCODE_1_PAD)
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 0") PORT_CODE(KEYCODE_0_PAD)

	PORT_START("P46")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad (")
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad )")
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 7") PORT_CODE(KEYCODE_7_PAD)
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 8") PORT_CODE(KEYCODE_8_PAD)
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 4") PORT_CODE(KEYCODE_4_PAD)
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 5") PORT_CODE(KEYCODE_5_PAD)
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 2") PORT_CODE(KEYCODE_2_PAD)
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad .") PORT_CODE(KEYCODE_DEL_PAD)

	PORT_START("P47")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad /") PORT_CODE(KEYCODE_SLASH_PAD)
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad *") PORT_CODE(KEYCODE_ASTERISK)
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 9") PORT_CODE(KEYCODE_9_PAD)
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad -") PORT_CODE(KEYCODE_MINUS_PAD)
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 6") PORT_CODE(KEYCODE_6_PAD)
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad +") PORT_CODE(KEYCODE_PLUS_PAD)
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad 3") PORT_CODE(KEYCODE_3_PAD)
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_KEYBOARD ) PORT_NAME("Keypad Enter") PORT_CODE(KEYCODE_ENTER_PAD)
INPUT_PORTS_END


//-------------------------------------------------
//  INPUT_PORTS( st )
//-------------------------------------------------

static INPUT_PORTS_START( st )
	PORT_START("config")
	PORT_CONFNAME( 0x01, 0x00, "Input Port 0 Device")
	PORT_CONFSETTING( 0x00, "Mouse" )
	PORT_CONFSETTING( 0x01, DEF_STR( Joystick ) )
	PORT_CONFNAME( 0x80, 0x80, "Monitor") PORT_WRITE_LINE_DEVICE_MEMBER(DEVICE_SELF, st_state, write_monochrome)
	PORT_CONFSETTING( 0x00, "Monochrome (Atari SM124)" )
	PORT_CONFSETTING( 0x80, "Color (Atari SC1224)" )

	PORT_INCLUDE( ikbd )

	PORT_START("IKBD_JOY0")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT )  PORT_PLAYER(1) PORT_8WAY PORT_CONDITION("config", 0x01, EQUALS, 0x01) // XB
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT ) PORT_PLAYER(1) PORT_8WAY PORT_CONDITION("config", 0x01, EQUALS, 0x01) // XA
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_JOYSTICK_UP )    PORT_PLAYER(1) PORT_8WAY PORT_CONDITION("config", 0x01, EQUALS, 0x01) // YA
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN )  PORT_PLAYER(1) PORT_8WAY PORT_CONDITION("config", 0x01, EQUALS, 0x01) // YB
	PORT_BIT( 0x10, IP_ACTIVE_LOW, IPT_JOYSTICK_UP )    PORT_PLAYER(2) PORT_8WAY
	PORT_BIT( 0x20, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN )  PORT_PLAYER(2) PORT_8WAY
	PORT_BIT( 0x40, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT )  PORT_PLAYER(2) PORT_8WAY
	PORT_BIT( 0x80, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT ) PORT_PLAYER(2) PORT_8WAY

	PORT_START("IKBD_JOY1")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_BUTTON2 ) PORT_PLAYER(1)
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_PLAYER(1)
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_PLAYER(2)

	PORT_START("IKBD_MOUSEX")
	PORT_BIT( 0xff, 0x00, IPT_MOUSE_X ) PORT_SENSITIVITY(100) PORT_KEYDELTA(5) PORT_MINMAX(0, 255) PORT_PLAYER(1) PORT_CONDITION("config", 0x01, EQUALS, 0x00)

	PORT_START("IKBD_MOUSEY")
	PORT_BIT( 0xff, 0x00, IPT_MOUSE_Y ) PORT_SENSITIVITY(100) PORT_KEYDELTA(5) PORT_MINMAX(0, 255) PORT_PLAYER(1) PORT_CONDITION("config", 0x01, EQUALS, 0x00)
INPUT_PORTS_END


//-------------------------------------------------
//  INPUT_PORTS( ste )
//-------------------------------------------------

static INPUT_PORTS_START( ste )
	PORT_START("config")
	PORT_CONFNAME( 0x01, 0x00, "Input Port 0 Device")
	PORT_CONFSETTING( 0x00, "Mouse" )
	PORT_CONFSETTING( 0x01, DEF_STR( Joystick ) )
	PORT_CONFNAME( 0x80, 0x80, "Monitor") PORT_WRITE_LINE_DEVICE_MEMBER(DEVICE_SELF, ste_state, write_monochrome)
	PORT_CONFSETTING( 0x00, "Monochrome (Atari SM124)" )
	PORT_CONFSETTING( 0x80, "Color (Atari SC1435)" )

	PORT_INCLUDE( ikbd )

	PORT_START("JOY0")
	PORT_BIT( 0x01, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_PLAYER(1)
	PORT_BIT( 0x02, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_PLAYER(3)
	PORT_BIT( 0x04, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_PLAYER(2)
	PORT_BIT( 0x08, IP_ACTIVE_LOW, IPT_BUTTON1 ) PORT_PLAYER(4)
	PORT_BIT( 0xf0, IP_ACTIVE_LOW, IPT_UNUSED )

	PORT_START("JOY1")
	PORT_BIT( 0x0001, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT ) PORT_PLAYER(1) PORT_8WAY
	PORT_BIT( 0x0002, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT )  PORT_PLAYER(1) PORT_8WAY
	PORT_BIT( 0x0004, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN )  PORT_PLAYER(1) PORT_8WAY
	PORT_BIT( 0x0008, IP_ACTIVE_LOW, IPT_JOYSTICK_UP )    PORT_PLAYER(1) PORT_8WAY
	PORT_BIT( 0x0010, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT ) PORT_PLAYER(2) PORT_8WAY
	PORT_BIT( 0x0020, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT )  PORT_PLAYER(2) PORT_8WAY
	PORT_BIT( 0x0040, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN )  PORT_PLAYER(2) PORT_8WAY
	PORT_BIT( 0x0080, IP_ACTIVE_LOW, IPT_JOYSTICK_UP )    PORT_PLAYER(2) PORT_8WAY
	PORT_BIT( 0x0100, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT ) PORT_PLAYER(3) PORT_8WAY
	PORT_BIT( 0x0200, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT )  PORT_PLAYER(3) PORT_8WAY
	PORT_BIT( 0x0400, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN )  PORT_PLAYER(3) PORT_8WAY
	PORT_BIT( 0x0800, IP_ACTIVE_LOW, IPT_JOYSTICK_UP )    PORT_PLAYER(3) PORT_8WAY
	PORT_BIT( 0x1000, IP_ACTIVE_LOW, IPT_JOYSTICK_RIGHT ) PORT_PLAYER(4) PORT_8WAY
	PORT_BIT( 0x2000, IP_ACTIVE_LOW, IPT_JOYSTICK_LEFT )  PORT_PLAYER(4) PORT_8WAY
	PORT_BIT( 0x4000, IP_ACTIVE_LOW, IPT_JOYSTICK_DOWN )  PORT_PLAYER(4) PORT_8WAY
	PORT_BIT( 0x8000, IP_ACTIVE_LOW, IPT_JOYSTICK_UP )    PORT_PLAYER(4) PORT_8WAY

	PORT_START("PADDLE0X")
	PORT_BIT( 0xff, 0x00, IPT_PADDLE ) PORT_SENSITIVITY(30) PORT_KEYDELTA(15) PORT_PLAYER(1)

	PORT_START("PADDLE0Y")
	PORT_BIT( 0xff, 0x00, IPT_PADDLE_V ) PORT_SENSITIVITY(30) PORT_KEYDELTA(15) PORT_PLAYER(1)

	PORT_START("PADDLE1X")
	PORT_BIT( 0xff, 0x00, IPT_PADDLE ) PORT_SENSITIVITY(30) PORT_KEYDELTA(15) PORT_PLAYER(2)

	PORT_START("PADDLE1Y")
	PORT_BIT( 0xff, 0x00, IPT_PADDLE_V ) PORT_SENSITIVITY(30) PORT_KEYDELTA(15) PORT_PLAYER(2)

	PORT_START("GUNX") // should be 10-bit
	PORT_BIT( 0xff, 0x80, IPT_LIGHTGUN_X ) PORT_CROSSHAIR(X, 1.0, 0.0, 0) PORT_SENSITIVITY(50) PORT_KEYDELTA(10) PORT_PLAYER(1)

	PORT_START("GUNY") // should be 10-bit
	PORT_BIT( 0xff, 0x80, IPT_LIGHTGUN_Y ) PORT_CROSSHAIR(Y, 1.0, 0.0, 0) PORT_SENSITIVITY(70) PORT_KEYDELTA(10) PORT_PLAYER(1)
INPUT_PORTS_END


//-------------------------------------------------
//  INPUT_PORTS( stbook )
//-------------------------------------------------

#if 0
static INPUT_PORTS_START( stbook )
	PORT_START("SW400")
	PORT_DIPNAME( 0x80, 0x80, "DMA sound hardware")
	PORT_DIPSETTING( 0x00, DEF_STR( No ) )
	PORT_DIPSETTING( 0x80, DEF_STR( Yes ) )
	PORT_DIPNAME( 0x40, 0x00, "WD1772 FDC")
	PORT_DIPSETTING( 0x40, "Low Speed (8 MHz)" )
	PORT_DIPSETTING( 0x00, "High Speed (16 MHz)" )
	PORT_DIPNAME( 0x20, 0x00, "Bypass Self Test")
	PORT_DIPSETTING( 0x00, DEF_STR( No ) )
	PORT_DIPSETTING( 0x20, DEF_STR( Yes ) )
	PORT_BIT( 0x1f, IP_ACTIVE_HIGH, IPT_UNUSED )
INPUT_PORTS_END
#endif


//-------------------------------------------------
//  INPUT_PORTS( tt030 )
//-------------------------------------------------

static INPUT_PORTS_START( tt030 )
	PORT_INCLUDE(ste)
INPUT_PORTS_END


//-------------------------------------------------
//  INPUT_PORTS( falcon )
//-------------------------------------------------

static INPUT_PORTS_START( falcon )
	PORT_INCLUDE(ste)
INPUT_PORTS_END



//**************************************************************************
//  DEVICE CONFIGURATION
//**************************************************************************

//-------------------------------------------------
//  ay8910_interface psg_intf
//-------------------------------------------------

void st_state::psg_pa_w(uint8_t data)
{
	/*

	    bit     description

	    0       SIDE 0
	    1       DRIVE 0
	    2       DRIVE 1
	    3       RTS
	    4       DTR
	    5       STROBE
	    6       GPO
	    7

	*/

	// drive select
	floppy_image_device *floppy = nullptr;
	if (!BIT(data, 1))
		floppy = m_floppy[0]->get_device();
	else if (!BIT(data, 2))
		floppy = m_floppy[1]->get_device();

	// side select
	if (floppy)
		floppy->ss_w(BIT(data, 0) ? 0 : 1);

	m_fdc->set_floppy(floppy);

	// request to send
	m_rs232->write_rts(BIT(data, 3));

	// data terminal ready
	m_rs232->write_dtr(BIT(data, 4));

	// centronics strobe
	m_centronics->write_strobe(BIT(data, 5));
}

//-------------------------------------------------
//  ay8910_interface stbook_psg_intf
//-------------------------------------------------

void stbook_state::psg_pa_w(uint8_t data)
{
	/*

	    bit     description

	    0       SIDE 0
	    1       DRIVE 0
	    2       DRIVE 1
	    3       RTS
	    4       DTR
	    5       STROBE
	    6       IDE RESET
	    7       DDEN

	*/

	// drive select
	floppy_image_device *floppy = nullptr;
	if (!BIT(data, 1))
		floppy = m_floppy[0]->get_device();
	else if (!BIT(data, 2))
		floppy = m_floppy[1]->get_device();

	// side select
	if (floppy)
		floppy->ss_w(BIT(data, 0) ? 0 : 1);

	m_fdc->set_floppy(floppy);

	// request to send
	m_rs232->write_rts(BIT(data, 3));

	// data terminal ready
	m_rs232->write_dtr(BIT(data, 4));

	// centronics strobe
	m_centronics->write_strobe(BIT(data, 5));

	// density select
	m_fdc->dden_w(BIT(data, 7));
}

WRITE_LINE_MEMBER( st_state::ikbd_tx_w )
{
	m_ikbd_tx = state;
}


WRITE_LINE_MEMBER( st_state::fdc_drq_w )
{
	if (state && (!(m_fdc_mode & DMA_MODE_ENABLED)) && (m_fdc_mode & DMA_MODE_FDC_HDC_ACK))
		fdc_dma_transfer();
}


//**************************************************************************
//  MACHINE INITIALIZATION
//**************************************************************************

//-------------------------------------------------
//  configure_memory -
//-------------------------------------------------

void st_state::configure_memory()
{
	address_space &program = m_maincpu->space(AS_PROGRAM);

	switch (m_ram->size())
	{
	case 256 * 1024:
		program.unmap_readwrite(0x040000, 0x3fffff);
		break;

	case 512 * 1024:
		program.unmap_readwrite(0x080000, 0x3fffff);
		break;

	case 1024 * 1024:
		program.unmap_readwrite(0x100000, 0x3fffff);
		break;

	case 2048 * 1024:
		program.unmap_readwrite(0x200000, 0x3fffff);
		break;
	}
}

//-------------------------------------------------
//  state_save -
//-------------------------------------------------

void st_state::state_save()
{
	m_dma_error = 1;

	save_item(NAME(m_mmu));
	save_item(NAME(m_dma_base));
	save_item(NAME(m_dma_error));
	save_item(NAME(m_fdc_mode));
	save_item(NAME(m_fdc_sectors));
	save_item(NAME(m_fdc_dmabytes));
	save_item(NAME(m_ikbd_keylatch));
	save_item(NAME(m_ikbd_mouse));
	save_item(NAME(m_ikbd_mouse_x));
	save_item(NAME(m_ikbd_mouse_y));
	save_item(NAME(m_ikbd_mouse_px));
	save_item(NAME(m_ikbd_mouse_py));
	save_item(NAME(m_ikbd_mouse_pc));
	save_item(NAME(m_ikbd_tx));
	save_item(NAME(m_ikbd_joy));
	save_item(NAME(m_midi_tx));
}

//-------------------------------------------------
//  MACHINE_START( st )
//-------------------------------------------------

void st_state::machine_start()
{
	m_led.resolve();

	// configure RAM banking
	configure_memory();

	if (m_cart->exists())
		m_maincpu->space(AS_PROGRAM).install_read_handler(0xfa0000, 0xfbffff, read16s_delegate(*m_cart, FUNC(generic_slot_device::read16_rom)));

	// allocate timers
	if (m_mousex.found())
	{
		m_mouse_timer = timer_alloc(FUNC(st_state::mouse_tick), this);
		m_mouse_timer->adjust(attotime::zero, 0, attotime::from_hz(500));
	}

	// register for state saving
	state_save();

	/// TODO: get callbacks to trigger these.
	m_mfp->i0_w(1);
	m_mfp->i4_w(1);
	m_mfp->i5_w(1);
	m_mfp->i7_w(1);
}


//-------------------------------------------------
//  state_save -
//-------------------------------------------------

void ste_state::state_save()
{
	st_state::state_save();

	save_item(NAME(m_dmasnd_base));
	save_item(NAME(m_dmasnd_end));
	save_item(NAME(m_dmasnd_cntr));
	save_item(NAME(m_dmasnd_baselatch));
	save_item(NAME(m_dmasnd_endlatch));
	save_item(NAME(m_dmasnd_ctrl));
	save_item(NAME(m_dmasnd_mode));
	save_item(NAME(m_dmasnd_fifo));
	save_item(NAME(m_dmasnd_samples));
	save_item(NAME(m_dmasnd_active));
	save_item(NAME(m_mw_data));
	save_item(NAME(m_mw_mask));
	save_item(NAME(m_mw_shift));
}


//-------------------------------------------------
//  MACHINE_START( ste )
//-------------------------------------------------

void ste_state::machine_start()
{
	m_led.resolve();

	/* configure RAM banking */
	configure_memory();

	if (m_cart->exists())
		m_maincpu->space(AS_PROGRAM).install_read_handler(0xfa0000, 0xfbffff, read16s_delegate(*m_cart, FUNC(generic_slot_device::read16_rom)));

	/* allocate timers */
	m_dmasound_timer = timer_alloc(FUNC(ste_state::dmasound_tick), this);
	m_microwire_timer = timer_alloc(FUNC(ste_state::microwire_tick), this);

	/* register for state saving */
	state_save();

	/// TODO: get callbacks to trigger these.
	m_mfp->i0_w(1);
	m_mfp->i4_w(1);
	m_mfp->i5_w(1);
	m_mfp->i7_w(1);
}


//-------------------------------------------------
//  MACHINE_START( megaste )
//-------------------------------------------------

void megaste_state::machine_start()
{
	ste_state::machine_start();

	save_item(NAME(m_cache));
}


//-------------------------------------------------
//  MACHINE_START( stbook )
//-------------------------------------------------

void stbook_state::machine_start()
{
	m_led.resolve();

	/* configure RAM banking */
	address_space &program = m_maincpu->space(AS_PROGRAM);

	switch (m_ram->size())
	{
	case 1024 * 1024:
		program.unmap_readwrite(0x100000, 0x3fffff);
		break;
	}

	if (m_cart->exists())
		m_maincpu->space(AS_PROGRAM).install_read_handler(0xfa0000, 0xfbffff, read16s_delegate(*m_cart, FUNC(generic_slot_device::read16_rom)));

	/* register for state saving */
	ste_state::state_save();

	/// TODO: get callbacks to trigger these.
	m_mfp->i0_w(1);
	m_mfp->i4_w(1);
	m_mfp->i5_w(1);
}

void st_state::floppy_formats(format_registration &fr)
{
	fr.add_mfm_containers();
	fr.add(FLOPPY_ST_FORMAT);
	fr.add(FLOPPY_MSA_FORMAT);
	fr.add(FLOPPY_PASTI_FORMAT);
	fr.add(FLOPPY_IPF_FORMAT);
}

static void atari_floppies(device_slot_interface &device)
{
	device.option_add("35dd", FLOPPY_35_DD);
}



//**************************************************************************
//  MACHINE CONFIGURATION
//**************************************************************************

void st_state::common(machine_config &config)
{
	// basic machine hardware
	M68000(config, m_maincpu, Y2/4);
	m_maincpu->set_addrmap(m68000_base_device::AS_CPU_SPACE, &st_state::cpu_space_map);
	m_maincpu->set_reset_callback(FUNC(st_state::reset_w));

	keyboard(config);

	// sound
	YM2149(config, m_ymsnd, Y2/16);
	m_ymsnd->set_flags(AY8910_SINGLE_OUTPUT);
	m_ymsnd->set_resistors_load(RES_K(1), 0, 0);
	m_ymsnd->port_a_write_callback().set(FUNC(st_state::psg_pa_w));
	m_ymsnd->port_b_write_callback().set("cent_data_out", FUNC(output_latch_device::write));

	// devices
	WD1772(config, m_fdc, Y2/4);
	m_fdc->intrq_wr_callback().set(m_mfp, FUNC(mc68901_device::i5_w)).invert();
	m_fdc->drq_wr_callback().set(FUNC(st_state::fdc_drq_w));
	FLOPPY_CONNECTOR(config, WD1772_TAG ":0", atari_floppies, "35dd",  st_state::floppy_formats);
	FLOPPY_CONNECTOR(config, WD1772_TAG ":1", atari_floppies, nullptr, st_state::floppy_formats);

	CENTRONICS(config, m_centronics, centronics_devices, "printer");
	m_centronics->busy_handler().set(m_mfp, FUNC(mc68901_device::i0_w));

	output_latch_device &cent_data_out(OUTPUT_LATCH(config, "cent_data_out"));
	m_centronics->set_output_latch(cent_data_out);

	MC68901(config, m_mfp, Y2/8);
	m_mfp->set_timer_clock(Y1);
	m_mfp->out_irq_cb().set_inputline(m_maincpu, M68K_IRQ_6);
	m_mfp->out_tdo_cb().set(m_mfp, FUNC(mc68901_device::tc_w));
	m_mfp->out_tdo_cb().append(m_mfp, FUNC(mc68901_device::rc_w));
	m_mfp->out_so_cb().set(m_rs232, FUNC(rs232_port_device::write_txd));

	RS232_PORT(config, m_rs232, default_rs232_devices, nullptr);
	m_rs232->rxd_handler().set(m_mfp, FUNC(mc68901_device::si_w));
	m_rs232->dcd_handler().set(m_mfp, FUNC(mc68901_device::i1_w));
	m_rs232->cts_handler().set(m_mfp, FUNC(mc68901_device::i2_w));
	m_rs232->ri_handler().set(m_mfp, FUNC(mc68901_device::i6_w));

	ACIA6850(config, m_acia[0]);
	m_acia[0]->txd_handler().set(FUNC(st_state::ikbd_tx_w));
	m_acia[0]->irq_handler().set("aciairq", FUNC(input_merger_device::in_w<0>));
	m_acia[0]->write_cts(0);
	m_acia[0]->write_dcd(0);

	ACIA6850(config, m_acia[1]);
	m_acia[1]->txd_handler().set("mdout", FUNC(midi_port_device::write_txd));
	m_acia[1]->irq_handler().set("aciairq", FUNC(input_merger_device::in_w<1>));
	m_acia[1]->write_cts(0);
	m_acia[1]->write_dcd(0);

	input_merger_device &aciairq(INPUT_MERGER_ANY_HIGH(config, "aciairq"));
	aciairq.output_handler().set(m_mfp, FUNC(mc68901_device::i4_w)).invert();

	MIDI_PORT(config, "mdin", midiin_slot, "midiin").rxd_handler().set(m_acia[1], FUNC(acia6850_device::write_rxd));
	MIDI_PORT(config, "mdout", midiout_slot, "midiout");

	clock_device &acia_clock(CLOCK(config, "acia_clock", Y2/64)); // 500kHz
	acia_clock.signal_handler().set(m_acia[0], FUNC(acia6850_device::write_txc));
	acia_clock.signal_handler().append(m_acia[0], FUNC(acia6850_device::write_rxc));
	acia_clock.signal_handler().append(m_acia[1], FUNC(acia6850_device::write_txc));
	acia_clock.signal_handler().append(m_acia[1], FUNC(acia6850_device::write_rxc));

	// cartridge
	GENERIC_CARTSLOT(config, m_cart, generic_linear_slot, "st_cart", "bin,rom");
	m_cart->set_width(GENERIC_ROM16_WIDTH);
	m_cart->set_endian(ENDIANNESS_BIG);

	// software lists
	SOFTWARE_LIST(config, "flop_list").set_original("st_flop");
	SOFTWARE_LIST(config, "cart_list").set_original("st_cart");
}

void st_state::keyboard(machine_config &config)
{
	hd6301v1_cpu_device &ikbd(HD6301V1(config, HD6301V1_TAG, 4_MHz_XTAL));
	ikbd.set_addrmap(AS_PROGRAM, &st_state::ikbd_map);
	ikbd.in_p1_cb().set(FUNC(st_state::ikbd_port1_r));
	ikbd.in_p2_cb().set(FUNC(st_state::ikbd_port2_r));
	ikbd.out_p2_cb().set(FUNC(st_state::ikbd_port2_w));
	ikbd.out_p3_cb().set(FUNC(st_state::ikbd_port3_w));
	ikbd.in_p4_cb().set(FUNC(st_state::ikbd_port4_r));
	ikbd.out_p4_cb().set(FUNC(st_state::ikbd_port4_w));
}

//-------------------------------------------------
//  machine_config( st )
//-------------------------------------------------

void st_state::st(machine_config &config)
{
	common(config);

	// basic machine hardware
	m_maincpu->set_addrmap(AS_PROGRAM, &st_state::st_map);

	// video hardware
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER);
	m_screen->set_screen_update(m_video, FUNC(st_video_device::screen_update));
	m_screen->set_raw(Y2/2, ATARIST_HTOT_PAL*2, ATARIST_HBEND_PAL*2, ATARIST_HBSTART_PAL*2, ATARIST_VTOT_PAL, ATARIST_VBEND_PAL, ATARIST_VBSTART_PAL);

	ST_VIDEO(config, m_video, Y2);
	m_video->set_screen(m_screen);
	m_video->set_ram_space(m_maincpu, AS_PROGRAM);
	m_video->de_callback().set(m_mfp, FUNC(mc68901_device::tbi_w));

	// sound hardware
	SPEAKER(config, "mono").front_center();
	m_ymsnd->add_route(ALL_OUTPUTS, "mono", 1.00);

	// internal ram
	RAM(config, m_ram);
	m_ram->set_default_size("1M"); // 1040ST
	m_ram->set_extra_options("512K,256K"); // 520ST, 260ST
}


//-------------------------------------------------
//  machine_config( megast )
//-------------------------------------------------

void megast_state::megast(machine_config &config)
{
	common(config);

	// basic machine hardware
	m_maincpu->set_addrmap(AS_PROGRAM, &megast_state::megast_map);

	ST_BLITTER(config, m_stb, Y2/4);
	m_stb->set_space(m_maincpu, AS_PROGRAM);
	m_stb->int_callback().set(m_mfp, FUNC(mc68901_device::i3_w));

	// video hardware
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER);
	m_screen->set_screen_update(m_video, FUNC(st_video_device::screen_update));
	m_screen->set_raw(Y2/4, ATARIST_HTOT_PAL, ATARIST_HBEND_PAL, ATARIST_HBSTART_PAL, ATARIST_VTOT_PAL, ATARIST_VBEND_PAL, ATARIST_VBSTART_PAL);

	ST_VIDEO(config, m_video, Y2);
	m_video->set_screen(m_screen);
	m_video->set_ram_space(m_maincpu, AS_PROGRAM);
	m_video->de_callback().set(m_mfp, FUNC(mc68901_device::tbi_w));

	// sound hardware
	SPEAKER(config, "mono").front_center();
	m_ymsnd->add_route(ALL_OUTPUTS, "mono", 1.00);

	// devices
	RP5C15(config, RP5C15_TAG, XTAL(32'768));

	// internal ram
	RAM(config, m_ram);
	m_ram->set_default_size("4M"); // Mega ST 4
	m_ram->set_extra_options("2M,1M"); // Mega ST 2, Mega ST 1
}


//-------------------------------------------------
//  machine_config( ste )
//-------------------------------------------------

void ste_state::ste(machine_config &config)
{
	common(config);

	// basic machine hardware
	m_maincpu->set_addrmap(AS_PROGRAM, &ste_state::ste_map);

	ST_BLITTER(config, m_stb, Y2/4);
	m_stb->set_space(m_maincpu, AS_PROGRAM);
	m_stb->int_callback().set(m_mfp, FUNC(mc68901_device::i3_w));

	// video hardware
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER);
	m_screen->set_screen_update(m_video, FUNC(ste_video_device::screen_update));
	m_screen->set_raw(Y2/4, ATARIST_HTOT_PAL, ATARIST_HBEND_PAL, ATARIST_HBSTART_PAL, ATARIST_VTOT_PAL, ATARIST_VBEND_PAL, ATARIST_VBSTART_PAL);

	STE_VIDEO(config, m_video, Y2);
	m_video->set_screen(m_screen);
	m_video->set_ram_space(m_maincpu, AS_PROGRAM);
	m_video->de_callback().set(m_mfp, FUNC(mc68901_device::tbi_w));

	// sound hardware
	SPEAKER(config, "lspeaker").front_left();
	SPEAKER(config, "rspeaker").front_right();
	m_ymsnd->add_route(0, "lspeaker", 0.50);
	m_ymsnd->add_route(0, "rspeaker", 0.50);
/*
    custom_device &custom_dac(CUSTOM(config, "custom", 0)); // DAC
    custom_dac.add_route(0, "rspeaker", 0.50);
    custom_dac.add_route(1, "lspeaker", 0.50);
*/
	LMC1992(config, LMC1992_TAG);

	// internal ram
	RAM(config, m_ram);
	m_ram->set_default_size("1M"); // 1040STe
	m_ram->set_extra_options("512K"); // 520STe
}


//-------------------------------------------------
//  machine_config( megaste )
//-------------------------------------------------

void megaste_state::megaste(machine_config &config)
{
	ste(config);
	m_maincpu->set_addrmap(AS_PROGRAM, &megaste_state::megaste_map);
	RP5C15(config, RP5C15_TAG, XTAL(32'768));
	SCC8530(config, Z8530_TAG, Y2/4);

	/* internal ram */
	m_ram->set_default_size("4M"); // Mega STe 4
	m_ram->set_extra_options("2M,1M"); // Mega STe 2, Mega STe 1
}


//-------------------------------------------------
//  machine_config( stbook )
//-------------------------------------------------
#if 0
void stbook_state::stbook(machine_config &config)
{
	// basic machine hardware
	M68000(config, m_maincpu, U517/2);
	m_maincpu->set_addrmap(AS_PROGRAM, &stbook_state::stbook_map);
	m_maincpu->set_reset_callback(FUNC(st_state::reset_w));

	//COP888(config, COP888_TAG, Y700);

	ST_BLITTER(config, m_stb, U517/2);
	m_stb->set_space(m_maincpu, AS_PROGRAM);
	m_stb->int_callback().set(m_mfp, FUNC(mc68901_device::i3_w));

	// video hardware
	SCREEN(config, m_screen, SCREEN_TYPE_LCD);
	m_screen->set_screen_update(m_video, FUNC(stbook_video_device::screen_update));
	m_screen->set_refresh_hz(60);
	m_screen->set_size(640, 400);
	m_screen->set_visarea(0, 639, 0, 399);

	STBOOK_VIDEO(config, m_video, Y2);
	m_video->set_screen(m_screen);
	m_video->set_ram_space(m_maincpu, AS_PROGRAM);
	m_video->de_callback().set(m_mfp, FUNC(mc68901_device::tbi_w));

	// sound hardware
	SPEAKER(config, "mono").front_center();
	ym3439_device &ym3439(YM3439(config, YM3439_TAG, U517/8));
	ym3439.set_flags(AY8910_SINGLE_OUTPUT);
	ym3439.set_resistors_load(RES_K(1), 0, 0);
	ym3439.port_a_write_callback().set(FUNC(stbook_state::psg_pa_w));
	ym3439.port_b_write_callback().set("cent_data_out", FUNC(output_latch_device::write));
	ym3439.add_route(ALL_OUTPUTS, "mono", 1.00);

	MC68901(config, m_mfp, U517/8);
	m_mfp->set_timer_clock(Y1);
	m_mfp->out_irq_cb().set_inputline(M68000_TAG, M68K_IRQ_6);
	m_mfp->out_tdo_cb().set(m_mfp, FUNC(mc68901_device::tc_w));
	m_mfp->out_tdo_cb().append(m_mfp, FUNC(mc68901_device::rc_w));
	m_mfp->out_so_cb().set(RS232_TAG, FUNC(rs232_port_device::write_txd));

	WD1772(config, m_fdc, U517/2);
	m_fdc->intrq_wr_callback().set(m_mfp, FUNC(mc68901_device::i5_w)).invert();
	m_fdc->drq_wr_callback().set(FUNC(st_state::fdc_drq_w));
	FLOPPY_CONNECTOR(config, WD1772_TAG ":0", atari_floppies, "35dd", 0, st_state::floppy_formats);
	FLOPPY_CONNECTOR(config, WD1772_TAG ":1", atari_floppies, 0,      0, st_state::floppy_formats);

	CENTRONICS(config, m_centronics, centronics_devices, "printer");
	m_centronics->busy_handler().set(m_mfp, FUNC(mc68901_device::i0_w));

	output_latch_device &cent_data_out(OUTPUT_LATCH(config, "cent_data_out");
	m_centronics->set_output_latch(cent_data_out);

	RS232_PORT(config, m_rs232, default_rs232_devices, nullptr);
	m_rs232->rxd_handler().set(m_mfp, FUNC(mc68901_device::si_w));
	m_rs232->dcd_handler().set(m_mfp, FUNC(mc68901_device::i1_w));
	m_rs232->cts_handler().set(m_mfp, FUNC(mc68901_device::i2_w));
	m_rs232->ri_handler().set(m_mfp, FUNC(mc68901_device::i6_w));

	ACIA6850(config, m_acia[0]);
	m_acia[0]->txd_handler().set(FUNC(st_state::ikbd_tx_w));
	m_acia[0]->irq_handler().set("aciairq", FUNC(input_merger_device::in_w<0>));
	m_acia[0]->write_cts(0);
	m_acia[0]->write_dcd(0);

	ACIA6850(config, m_acia[1]);
	m_acia[1]->txd_handler().set("mdout", FUNC(midi_port_device::write_txd));
	m_acia[1]->irq_handler().set("aciairq", FUNC(input_merger_device::in_w<1>));
	m_acia[1]->write_cts(0);
	m_acia[1]->write_dcd(0);

	input_merger_device &aciairq(INPUT_MERGER_ANY_HIGH(config, "aciairq"));
	aciairq.output_handler().set(m_mfp, FUNC(mc68901_device::i4_w)).invert();

	MIDI_PORT(config, "mdin", midiin_slot, "midiin").rxd_handler().set(m_acia[1], FUNC(acia6850_device::write_rxd));
	MIDI_PORT(config, "mdout", midiout_slot, "midiout");

	clock_device &acia_clock(CLOCK(config, "acia_clock", Y2/64)); // 500kHz
	acia_clock.signal_handler().set(m_acia[0], FUNC(acia6850_device::write_txc));
	acia_clock.signal_handler().append(m_acia[0], FUNC(acia6850_device::write_rxc));
	acia_clock.signal_handler().append(m_acia[1], FUNC(acia6850_device::write_txc));
	acia_clock.signal_handler().append(m_acia[1], FUNC(acia6850_device::write_rxc));

	// cartridge
	GENERIC_CARTSLOT(config, m_cart, generic_linear_slot, "st_cart", "bin,rom");
	m_cart->set_width(GENERIC_ROM16_WIDTH);
	m_cart->set_endian(ENDIANNESS_BIG);
	SOFTWARE_LIST(config, "cart_list").set_original("st_cart");

	/* internal ram */
	RAM(config, m_ram).set_default_size("4M").set_extra_options("1M");
}
#endif

//-------------------------------------------------
//  machine_config( tt030 )
//-------------------------------------------------

void ste_state::tt030(machine_config &config)
{
	ste(config);
}


//-------------------------------------------------
//  machine_config( falcon )
//-------------------------------------------------

void ste_state::falcon(machine_config &config)
{
	ste(config);
}


//-------------------------------------------------
//  machine_config( falcon40 )
//-------------------------------------------------

void ste_state::falcon40(machine_config &config)
{
	ste(config);
}



//**************************************************************************
//  ROMS
//**************************************************************************

//-------------------------------------------------
//  ROM( st )
//-------------------------------------------------

ROM_START( st )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos100")
	ROM_SYSTEM_BIOS( 0, "tos099", "TOS 0.99 (Disk TOS)" )
	ROMX_LOAD( "tos099.bin", 0x00000, 0x04000, CRC(cee3c664) SHA1(80c10b31b63b906395151204ec0a4984c8cb98d6), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos100", "TOS 1.0 (ROM TOS)" )
	ROMX_LOAD( "tos100.bin", 0x00000, 0x30000, BAD_DUMP CRC(d331af30) SHA1(7bcc2311d122f451bd03c9763ade5a119b2f90da), ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102.bin", 0x00000, 0x30000, BAD_DUMP CRC(d3c32283) SHA1(735793fdba07fe8d5295caa03484f6ef3de931f5), ROM_BIOS(2) )
	ROM_SYSTEM_BIOS( 3, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104.bin", 0x00000, 0x30000, BAD_DUMP CRC(90f4fbff) SHA1(2487f330b0895e5d88d580d4ecb24061125e88ad), ROM_BIOS(3) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( st_uk )
//-------------------------------------------------

ROM_START( st_uk )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos100")
	ROM_SYSTEM_BIOS( 0, "tos100", "TOS 1.0 (ROM TOS)" )
	ROMX_LOAD( "tos100uk.bin", 0x00000, 0x30000, BAD_DUMP CRC(1a586c64) SHA1(9a6e4c88533a9eaa4d55cdc040e47443e0226eb2), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102uk.bin", 0x00000, 0x30000, BAD_DUMP CRC(3b5cd0c5) SHA1(87900a40a890fdf03bd08be6c60cc645855cbce5), ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104uk.bin", 0x00000, 0x30000, BAD_DUMP CRC(a50d1d43) SHA1(9526ef63b9cb1d2a7109e278547ae78a5c1db6c6), ROM_BIOS(2) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( st_de )
//-------------------------------------------------

ROM_START( st_de )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos100")
	ROM_SYSTEM_BIOS( 0, "tos100", "TOS 1.0 (ROM TOS)" )
	ROMX_LOAD( "tos100de.bin", 0x00000, 0x30000, BAD_DUMP CRC(16e3e979) SHA1(663d9c87cfb44ae8ada855fe9ed3cccafaa7a4ce), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102de.bin", 0x00000, 0x30000, BAD_DUMP CRC(36a0058e) SHA1(cad5d2902e875d8bf0a14dc5b5b8080b30254148), ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104de.bin", 0x00000, 0x30000, BAD_DUMP CRC(62b82b42) SHA1(5313733f91b083c6265d93674cb9d0b7efd02da8), ROM_BIOS(2) )
	ROM_SYSTEM_BIOS( 3, "tos10x", "TOS 1.0?" )
	ROMX_LOAD( "st 7c1 a4.u4", 0x00000, 0x08000, CRC(867fdd7e) SHA1(320d12acf510301e6e9ab2e3cf3ee60b0334baa0), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "st 7c1 a9.u7", 0x00001, 0x08000, CRC(30e8f982) SHA1(253f26ff64b202b2681ab68ffc9954125120baea), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "st 7c1 b0.u3", 0x10000, 0x08000, CRC(b91337ed) SHA1(21a338f9bbd87bce4a12d38048e03a361f58d33e), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "st 7a4 a6.u6", 0x10001, 0x08000, CRC(969d7bbe) SHA1(72b998c1f25211c2a96c81a038d71b6a390585c2), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "st 7c1 a2.u2", 0x20000, 0x08000, CRC(d0513329) SHA1(49855a3585e2f75b2af932dd4414ed64e6d9501f), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "st 7c1 b1.u5", 0x20001, 0x08000, CRC(c115cbc8) SHA1(2b52b81a1a4e0818d63f98ee4b25c30e2eba61cb), ROM_SKIP(1) | ROM_BIOS(3) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( st_fr )
//-------------------------------------------------

ROM_START( st_fr )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos100")
	ROM_SYSTEM_BIOS( 0, "tos100", "TOS 1.0 (ROM TOS)" )
	ROMX_LOAD( "tos100fr.bin", 0x00000, 0x30000, BAD_DUMP CRC(2b7f2117) SHA1(ecb00a2e351a6205089a281b4ce6e08959953704), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102fr.bin", 0x00000, 0x30000, BAD_DUMP CRC(8688fce6) SHA1(f5a79aac0a4e812ca77b6ac51d58d98726f331fe), ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104fr.bin", 0x00000, 0x30000, BAD_DUMP CRC(a305a404) SHA1(20dba880344b810cf63cec5066797c5a971db870), ROM_BIOS(2) )
	ROM_SYSTEM_BIOS( 3, "tos10x", "TOS 1.0?" )
	ROMX_LOAD( "c101658-001.u63", 0x00000, 0x08000, CRC(9c937f6f) SHA1(d4a3ea47568ef6233f3f2056e384b09eedd84961), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "c101661-001.u67", 0x00001, 0x08000, CRC(997298f3) SHA1(9e06d42df88557252a36791b514afe455600f679), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "c101657-001.u59", 0x10000, 0x08000, CRC(b63be6a1) SHA1(434f443472fc649568e4f8be6880f39c2def7819), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "c101660-001.u62", 0x10001, 0x08000, CRC(a813892c) SHA1(d041c113050dfb00166c4a7a52766e1b7eac9cab), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "c101656-001.u48", 0x20000, 0x08000, CRC(dbd93fb8) SHA1(cf9ec11e4bc2465490e7e6c981d9f61eae6cb359), ROM_SKIP(1) | ROM_BIOS(3) )
	ROMX_LOAD( "c101659-001.u53", 0x20001, 0x08000, CRC(67c9785a) SHA1(917a17e9f83bee015c25b327780eebb11cb2c5a5), ROM_SKIP(1) | ROM_BIOS(3) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( st_es )
//-------------------------------------------------

ROM_START( st_es )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos104")
	ROM_SYSTEM_BIOS( 0, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104es.bin", 0x00000, 0x30000, BAD_DUMP CRC(f4e8ecd2) SHA1(df63f8ac09125d0877b55d5ba1282779b7f99c16), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( st_nl )
//-------------------------------------------------

ROM_START( st_nl )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos104")
	ROM_SYSTEM_BIOS( 0, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104nl.bin", 0x00000, 0x30000, BAD_DUMP CRC(bb4370d4) SHA1(6de7c96b2d2e5c68778f4bce3eaf85a4e121f166), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( st_se )
//-------------------------------------------------

ROM_START( st_se )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos102")
	ROM_SYSTEM_BIOS( 0, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102se.bin", 0x00000, 0x30000, BAD_DUMP CRC(673fd0c2) SHA1(433de547e09576743ae9ffc43d43f2279782e127), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104se.bin", 0x00000, 0x30000, BAD_DUMP CRC(80ecfdce) SHA1(b7ad34d5cdfbe86ea74ae79eca11dce421a7bbfd), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( st_sg )
//-------------------------------------------------

ROM_START( st_sg )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos102")
	ROM_SYSTEM_BIOS( 0, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102sg.bin", 0x00000, 0x30000, BAD_DUMP CRC(5fe16c66) SHA1(45acb2fc4b1b13bd806c751aebd66c8304fc79bc), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104sg.bin", 0x00000, 0x30000, BAD_DUMP CRC(e58f0bdf) SHA1(aa40bf7203f02b2251b9e4850a1a73ff1c7da106), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megast )
//-------------------------------------------------

ROM_START( megast )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos104")
	ROM_SYSTEM_BIOS( 0, "tos102", "TOS 1.02 (MEGA TOS)" ) // came in both 6 rom and 2 rom formats; 6 roms are 27512, and 2 roms are non-jedec RP231024 (TC531000 equivalent) 28-pin roms with A16 instead of /OE on pin 22
	ROMX_LOAD( "tos102.bin", 0x00000, 0x30000, BAD_DUMP CRC(d3c32283) SHA1(735793fdba07fe8d5295caa03484f6ef3de931f5), ROM_BIOS(0) )
	//For a C100167-001 revision B Mega ST motherboard, jumpered for 2 roms:
	//ROMX_LOAD( "c101629-001__(c)atari_1987__38__rp231024e__0564__8807_z07.rp231024.u9", 0x00000, 0x20000, NO_DUMP, ROM_BIOS(0)|ROM_SKIP(1) ) // in u9 HI-0 socket
	//ROMX_LOAD( "c101630-002__(c)atari_1987__38__rp231024e__0563__8809_z10.rp231024.u10", 0x00001, 0x20000, NO_DUMP, ROM_BIOS(0)|ROM_SKIP(1) ) // in u10 LO-0 socket
	//For a C100167-001 revision B Mega ST motherboard, jumpered for 6 roms:
	//ROMX_LOAD( "unknownmarkings_hi-0.27512.u9", 0x00000, 0x10000, NO_DUMP, ROM_BIOS(0)|ROM_SKIP(1) )
	//ROMX_LOAD( "unknownmarkings_lo-0.27512.u10", 0x00001, 0x10000, NO_DUMP, ROM_BIOS(0)|ROM_SKIP(1) )
	//ROMX_LOAD( "unknownmarkings_hi-1.27512.u6", 0x10000, 0x10000, NO_DUMP, ROM_BIOS(0)|ROM_SKIP(1) )
	//ROMX_LOAD( "unknownmarkings_lo-1.27512.u7", 0x10001, 0x10000, NO_DUMP, ROM_BIOS(0)|ROM_SKIP(1) )
	//ROMX_LOAD( "unknownmarkings_hi-2.27512.u3", 0x20000, 0x10000, NO_DUMP, ROM_BIOS(0)|ROM_SKIP(1) )
	//ROMX_LOAD( "unknownmarkings_lo-2.27512.u4", 0x20001, 0x10000, NO_DUMP, ROM_BIOS(0)|ROM_SKIP(1) )
	ROM_SYSTEM_BIOS( 1, "tos104", "TOS 1.04 (Rainbow TOS)" )  // came in both 6 rom and 2 rom formats; 6 roms are 27512, and 2 roms are non-jedec RP231024 (TC531000 equivalent) 28-pin roms with A16 instead of /OE on pin 22
	ROMX_LOAD( "tos104.bin", 0x00000, 0x30000, BAD_DUMP CRC(90f4fbff) SHA1(2487f330b0895e5d88d580d4ecb24061125e88ad), ROM_BIOS(1) )
	/*For a C100167-001 revision B Mega ST motherboard, jumpered for 2 roms:
	These came in an upgrade kit pouch with label:
	RAINBOW (TOS 1.4)
	CA400407 2CHIPSET
	(C) ATARI CORP.
	and an end-user pamphlet explaining the changes in 1.04 and a sheet giving installation instructions (jumper strapping to convert between 2/6 chip)
	*/
	//ROMX_LOAD( "rainbow_(tos_1.4)__c300683-002_h0__(c)atari_corp.rp231024e.u9", 0x00000, 0x20000, NO_DUMP, ROM_BIOS(1)|ROM_SKIP(1) )
	//ROMX_LOAD( "rainbow_(tos_1.4)__c300684-002_l0__(c)atari_corp.rp231024e.u10", 0x00001, 0x20000, NO_DUMP, ROM_BIOS(1)|ROM_SKIP(1) )
	//For a C100167-001 revision B Mega ST motherboard, jumpered for 6 roms:
	//ROMX_LOAD( "rainbow_(tos_1.4)__c300789-002_h0__(c)atari_corp.27512.u9", 0x00000, 0x10000, NO_DUMP, ROM_BIOS(1)|ROM_SKIP(1) )
	//ROMX_LOAD( "rainbow_(tos_1.4)__c300792-002_l0__(c)atari_corp.27512.u10", 0x00001, 0x10000, NO_DUMP, ROM_BIOS(1)|ROM_SKIP(1) )
	//ROMX_LOAD( "rainbow_(tos_1.4)__c300788-002_h1__(c)atari_corp.27512.u6", 0x10000, 0x10000, NO_DUMP, ROM_BIOS(1)|ROM_SKIP(1) )
	//ROMX_LOAD( "rainbow_(tos_1.4)__c300791-002_l1__(c)atari_corp.27512.u7", 0x10001, 0x10000, NO_DUMP, ROM_BIOS(1)|ROM_SKIP(1) )
	//ROMX_LOAD( "rainbow_(tos_1.4)__c300787-002_h2__(c)atari_corp.27512.u3", 0x20000, 0x10000, NO_DUMP, ROM_BIOS(1)|ROM_SKIP(1) )
	//ROMX_LOAD( "rainbow_(tos_1.4)__c300790-002_l2__(c)atari_corp.27512.u4", 0x20001, 0x10000, NO_DUMP, ROM_BIOS(1)|ROM_SKIP(1) )
	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megast_uk )
//-------------------------------------------------

ROM_START( megast_uk )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos104")
	ROM_SYSTEM_BIOS( 0, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102uk.bin", 0x00000, 0x30000, BAD_DUMP CRC(3b5cd0c5) SHA1(87900a40a890fdf03bd08be6c60cc645855cbce5), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104uk.bin", 0x00000, 0x30000, BAD_DUMP CRC(a50d1d43) SHA1(9526ef63b9cb1d2a7109e278547ae78a5c1db6c6), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megast_de )
//-------------------------------------------------

ROM_START( megast_de )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos104")
	ROM_SYSTEM_BIOS( 0, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102de.bin", 0x00000, 0x30000, BAD_DUMP CRC(36a0058e) SHA1(cad5d2902e875d8bf0a14dc5b5b8080b30254148), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104de.bin", 0x00000, 0x30000, BAD_DUMP CRC(62b82b42) SHA1(5313733f91b083c6265d93674cb9d0b7efd02da8), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megast_fr )
//-------------------------------------------------

ROM_START( megast_fr )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos104")
	ROM_SYSTEM_BIOS( 0, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102fr.bin", 0x00000, 0x30000, BAD_DUMP CRC(8688fce6) SHA1(f5a79aac0a4e812ca77b6ac51d58d98726f331fe), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104fr.bin", 0x00000, 0x30000, BAD_DUMP CRC(a305a404) SHA1(20dba880344b810cf63cec5066797c5a971db870), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megast_se )
//-------------------------------------------------

ROM_START( megast_se )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos104")
	ROM_SYSTEM_BIOS( 0, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102se.bin", 0x00000, 0x30000, BAD_DUMP CRC(673fd0c2) SHA1(433de547e09576743ae9ffc43d43f2279782e127), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104se.bin", 0x00000, 0x30000, BAD_DUMP CRC(80ecfdce) SHA1(b7ad34d5cdfbe86ea74ae79eca11dce421a7bbfd), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megast_sg )
//-------------------------------------------------

ROM_START( megast_sg )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos104")
	ROM_SYSTEM_BIOS( 0, "tos102", "TOS 1.02 (MEGA TOS)" )
	ROMX_LOAD( "tos102sg.bin", 0x00000, 0x30000, BAD_DUMP CRC(5fe16c66) SHA1(45acb2fc4b1b13bd806c751aebd66c8304fc79bc), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104sg.bin", 0x00000, 0x30000, BAD_DUMP CRC(e58f0bdf) SHA1(aa40bf7203f02b2251b9e4850a1a73ff1c7da106), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( stacy )
//-------------------------------------------------

#if 0
ROM_START( stacy )
	ROM_REGION16_BE( 0x30000, M68000_TAG, 0 )
	ROM_SYSTEM_BIOS( 0, "tos104", "TOS 1.04 (Rainbow TOS)" )
	ROMX_LOAD( "tos104.bin", 0x00000, 0x30000, BAD_DUMP CRC(a50d1d43) SHA1(9526ef63b9cb1d2a7109e278547ae78a5c1db6c6), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END
#endif


//-------------------------------------------------
//  ROM( ste )
//-------------------------------------------------

ROM_START( ste )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos106", "TOS 1.06 (STE TOS, Revision 1)" )
	ROMX_LOAD( "tos106.bin", 0x00000, 0x40000, BAD_DUMP CRC(a2e25337) SHA1(6a850810a92fdb1e64d005a06ea4079f51c97145), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos162", "TOS 1.62 (STE TOS, Revision 2)" )
	ROMX_LOAD( "tos162.bin", 0x00000, 0x40000, BAD_DUMP CRC(1c1a4eba) SHA1(42b875f542e5b728905d819c83c31a095a6a1904), ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206.bin", 0x00000, 0x40000, BAD_DUMP CRC(3f2f840f) SHA1(ee58768bdfc602c9b14942ce5481e97dd24e7c83), ROM_BIOS(2) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( ste_uk )
//-------------------------------------------------

ROM_START( ste_uk )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos106", "TOS 1.06 (STE TOS, Revision 1)" )
	ROMX_LOAD( "tos106uk.bin", 0x00000, 0x40000, BAD_DUMP CRC(d72fea29) SHA1(06f9ea322e74b682df0396acfaee8cb4d9c90cad), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos162", "TOS 1.62 (STE TOS, Revision 2)" )
	ROMX_LOAD( "tos162uk.bin", 0x00000, 0x40000, BAD_DUMP CRC(d1c6f2fa) SHA1(70db24a7c252392755849f78940a41bfaebace71), ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206uk.bin", 0x00000, 0x40000, BAD_DUMP CRC(08538e39) SHA1(2400ea95f547d6ea754a99d05d8530c03f8b28e3), ROM_BIOS(2) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( ste_de )
//-------------------------------------------------

ROM_START( ste_de )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos106", "TOS 1.06 (STE TOS, Revision 1)" )
	ROMX_LOAD( "tos106de.bin", 0x00000, 0x40000, BAD_DUMP CRC(7c67c5c9) SHA1(3b8cf5ffa41b252eb67f8824f94608fa4005d6dd), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos162", "TOS 1.62 (STE TOS, Revision 2)" )
	ROMX_LOAD( "tos162de.bin", 0x00000, 0x40000, BAD_DUMP CRC(2cdeb5e5) SHA1(10d9f61705048ee3dcbec67df741bed49b922149), ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206de.bin", 0x00000, 0x40000, BAD_DUMP CRC(143cd2ab) SHA1(d1da866560734289c4305f1028c36291d331d417), ROM_BIOS(2) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( ste_es )
//-------------------------------------------------

ROM_START( ste_es )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos106")
	ROM_SYSTEM_BIOS( 0, "tos106", "TOS 1.06 (STE TOS, Revision 1)" )
	ROMX_LOAD( "tos106es.bin", 0x00000, 0x40000, BAD_DUMP CRC(5cd2a540) SHA1(3a18f342c8288c0bc1879b7a209c73d5d57f7e81), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( ste_fr )
//-------------------------------------------------

ROM_START( ste_fr )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos106", "TOS 1.06 (STE TOS, Revision 1)" )
	ROMX_LOAD( "tos106fr.bin", 0x00000, 0x40000, BAD_DUMP CRC(b6e58a46) SHA1(7d7e3cef435caa2fd7733a3fbc6930cb9ea7bcbc), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos162", "TOS 1.62 (STE TOS, Revision 2)" )
	ROMX_LOAD( "tos162fr.bin", 0x00000, 0x40000, BAD_DUMP CRC(0ab003be) SHA1(041e134da613f718fca8bd47cd7733076e8d7588), ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206fr.bin", 0x00000, 0x40000, BAD_DUMP CRC(e3a99ca7) SHA1(387da431e6e3dd2e0c4643207e67d06cf33618c3), ROM_BIOS(2) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( ste_it )
//-------------------------------------------------

ROM_START( ste_it )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos106")
	ROM_SYSTEM_BIOS( 0, "tos106", "TOS 1.06 (STE TOS, Revision 1)" )
	ROMX_LOAD( "tos106it.bin", 0x00000, 0x40000, BAD_DUMP CRC(d3a55216) SHA1(28dc74e5e0fa56b685bbe15f9837f52684fee9fd), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( ste_se )
//-------------------------------------------------

ROM_START( ste_se )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos162", "TOS 1.62 (STE TOS, Revision 2)" )
	ROMX_LOAD( "tos162se.bin", 0x00000, 0x40000, BAD_DUMP CRC(90f124b1) SHA1(6e5454e861dbf4c46ce5020fc566c31202087b88), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206se.bin", 0x00000, 0x40000, BAD_DUMP CRC(be61906d) SHA1(ebdf5a4cf08471cd315a91683fcb24e0f029d451), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( ste_sg )
//-------------------------------------------------

ROM_START( ste_sg )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206sg.bin", 0x00000, 0x40000, BAD_DUMP CRC(8c4fe57d) SHA1(c7a9ae3162f020dcac0c2a46cf0c033f91b98644), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megaste )
//-------------------------------------------------

ROM_START( megaste )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos205", "TOS 2.05 (Mega STE TOS)" )
	ROMX_LOAD( "atari mega ste 205 018 tms27c010.bin", 0x00000, 0x20000, CRC(befac3ab) SHA1(5b49f101f15a4d1c89cfd1d7ce3fec84a5ca36d0), ROM_BIOS(0) | ROM_SKIP(1) )
	ROMX_LOAD( "atari mega ste 205 019 tms27c010.bin", 0x00001, 0x20000, CRC(ea2a136d) SHA1(c3c259293de562d2a0fac4d41f95cf3d42ad6df4), ROM_BIOS(0) | ROM_SKIP(1) )
	ROM_SYSTEM_BIOS( 1, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206.bin", 0x00000, 0x40000, BAD_DUMP CRC(3f2f840f) SHA1(ee58768bdfc602c9b14942ce5481e97dd24e7c83), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megaste_uk )
//-------------------------------------------------

ROM_START( megaste_uk )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
#if 0
	ROM_SYSTEM_BIOS( 0, "tos202", "TOS 2.02 (Mega STE TOS)" )
	ROMX_LOAD( "tos202uk.bin", 0x00000, 0x40000, NO_DUMP, ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos205", "TOS 2.05 (Mega STE TOS)" )
	ROMX_LOAD( "tos205uk.bin", 0x00000, 0x40000, NO_DUMP, ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206uk.bin", 0x00000, 0x40000, BAD_DUMP CRC(08538e39) SHA1(2400ea95f547d6ea754a99d05d8530c03f8b28e3), ROM_BIOS(2) )
#else
	ROM_SYSTEM_BIOS( 0, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206uk.bin", 0x00000, 0x40000, BAD_DUMP CRC(08538e39) SHA1(2400ea95f547d6ea754a99d05d8530c03f8b28e3), ROM_BIOS(0) )
#endif

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megaste_fr )
//-------------------------------------------------

ROM_START( megaste_fr )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos205", "TOS 2.05 (Mega STE TOS)" )
	ROMX_LOAD( "tos205fr.bin", 0x00000, 0x40000, BAD_DUMP CRC(27b83d2f) SHA1(83963b0feb0d119b2ca6f51e483e8c20e6ab79e1), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206fr.bin", 0x00000, 0x40000, BAD_DUMP CRC(e3a99ca7) SHA1(387da431e6e3dd2e0c4643207e67d06cf33618c3), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megaste_de )
//-------------------------------------------------

ROM_START( megaste_de )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos205", "TOS 2.05 (Mega STE TOS)" )
	ROMX_LOAD( "tos205de.bin", 0x00000, 0x40000, BAD_DUMP CRC(518b24e6) SHA1(084e083422f8fd9ac7a2490f19b81809c52b91b4), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206de.bin", 0x00000, 0x40000, BAD_DUMP CRC(143cd2ab) SHA1(d1da866560734289c4305f1028c36291d331d417), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megaste_es )
//-------------------------------------------------

ROM_START( megaste_es )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos205")
	ROM_SYSTEM_BIOS( 0, "tos205", "TOS 2.05 (Mega STE TOS)" )
	ROMX_LOAD( "tos205es.bin", 0x00000, 0x40000, BAD_DUMP CRC(2a426206) SHA1(317715ad8de718b5acc7e27ecf1eb833c2017c91), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megaste_it )
//-------------------------------------------------

ROM_START( megaste_it )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos205")
	ROM_SYSTEM_BIOS( 0, "tos205", "TOS 2.05 (Mega STE TOS)" )
	ROMX_LOAD( "tos205it.bin", 0x00000, 0x40000, BAD_DUMP CRC(b28bf5a1) SHA1(8e0581b442384af69345738849cf440d72f6e6ab), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( megaste_se )
//-------------------------------------------------

ROM_START( megaste_se )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos206")
	ROM_SYSTEM_BIOS( 0, "tos205", "TOS 2.05 (Mega STE TOS)" )
	ROMX_LOAD( "tos205se.bin", 0x00000, 0x40000, BAD_DUMP CRC(6d49ccbe) SHA1(c065b1a9a2e42e5e373333e99be829028902acaa), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos206", "TOS 2.06 (ST/STE TOS)" )
	ROMX_LOAD( "tos206se.bin", 0x00000, 0x40000, BAD_DUMP CRC(be61906d) SHA1(ebdf5a4cf08471cd315a91683fcb24e0f029d451), ROM_BIOS(1) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( stbook )
//-------------------------------------------------

#if 0
ROM_START( stbook )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_SYSTEM_BIOS( 0, "tos208", "TOS 2.08" )
	ROMX_LOAD( "tos208.bin", 0x00000, 0x40000, NO_DUMP, ROM_BIOS(0) )

	ROM_REGION( 0x1000, COP888_TAG, 0 )
	ROM_LOAD( "cop888c0.u703", 0x0000, 0x1000, NO_DUMP )
ROM_END
#endif


//-------------------------------------------------
//  ROM( stpad )
//-------------------------------------------------

#if 0
ROM_START( stpad )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_SYSTEM_BIOS( 0, "tos205", "TOS 2.05" )
	ROMX_LOAD( "tos205.bin", 0x00000, 0x40000, NO_DUMP, ROM_BIOS(0) )
ROM_END
#endif


//-------------------------------------------------
//  ROM( tt030 )
//-------------------------------------------------

ROM_START( tt030 )
	ROM_REGION32_BE( 0x80000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos306")
	ROM_SYSTEM_BIOS( 0, "tos306", "TOS 3.06 (TT TOS)" )
	ROMX_LOAD( "tos306.bin", 0x00000, 0x80000, BAD_DUMP CRC(e65adbd7) SHA1(b15948786278e1f2abc4effbb6d40786620acbe8), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( tt030_uk )
//-------------------------------------------------

ROM_START( tt030_uk )
	ROM_REGION32_BE( 0x80000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos306")
	ROM_SYSTEM_BIOS( 0, "tos306", "TOS 3.06 (TT TOS)" )
	ROMX_LOAD( "tos306uk.bin", 0x00000, 0x80000, BAD_DUMP CRC(75dda215) SHA1(6325bdfd83f1b4d3afddb2b470a19428ca79478b), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( tt030_de )
//-------------------------------------------------

ROM_START( tt030_de )
	ROM_REGION32_BE( 0x80000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos306")
	ROM_SYSTEM_BIOS( 0, "tos306", "TOS 3.06 (TT TOS)" )
	ROMX_LOAD( "tos306de.bin", 0x00000, 0x80000, BAD_DUMP CRC(4fcbb59d) SHA1(80af04499d1c3b8551fc4d72142ff02c2182e64a), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( tt030_fr )
//-------------------------------------------------

ROM_START( tt030_fr )
	ROM_REGION32_BE( 0x80000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos306")
	ROM_SYSTEM_BIOS( 0, "tos306", "TOS 3.06 (TT TOS)" )
	ROMX_LOAD( "tos306fr.bin", 0x00000, 0x80000, BAD_DUMP CRC(1945511c) SHA1(6bb19874e1e97dba17215d4f84b992c224a81b95), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( tt030_pl )
//-------------------------------------------------

ROM_START( tt030_pl )
	ROM_REGION32_BE( 0x80000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos306")
	ROM_SYSTEM_BIOS( 0, "tos306", "TOS 3.06 (TT TOS)" )
	ROMX_LOAD( "tos306pl.bin", 0x00000, 0x80000, BAD_DUMP CRC(4f2404bc) SHA1(d122b8ceb202b52754ff0d442b1c81f8b4de3436), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( fx1 )
//-------------------------------------------------

#if 0
ROM_START( fx1 )
	ROM_REGION16_BE( 0x40000, M68000_TAG, 0 )
	ROM_SYSTEM_BIOS( 0, "tos207", "TOS 2.07" )
	ROMX_LOAD( "tos207.bin", 0x00000, 0x40000, NO_DUMP, ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END
#endif


//-------------------------------------------------
//  ROM( falcon30 )
//-------------------------------------------------

ROM_START( falcon30 )
	ROM_REGION32_BE( 0x80000, M68000_TAG, 0 )
	ROM_DEFAULT_BIOS("tos404")
#if 0
	ROM_SYSTEM_BIOS( 0, "tos400", "TOS 4.00" )
	ROMX_LOAD( "tos400.bin", 0x00000, 0x7ffff, BAD_DUMP CRC(1fbc5396) SHA1(d74d09f11a0bf37a86ccb50c6e7f91aac4d4b11b), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos401", "TOS 4.01" )
	ROMX_LOAD( "tos401.bin", 0x00000, 0x80000, NO_DUMP, ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos402", "TOS 4.02" )
	ROMX_LOAD( "tos402.bin", 0x00000, 0x80000, BAD_DUMP CRC(63f82f23) SHA1(75de588f6bbc630fa9c814f738195da23b972cc6), ROM_BIOS(2) )
	ROM_SYSTEM_BIOS( 3, "tos404", "TOS 4.04" )
	ROMX_LOAD( "tos404.bin", 0x00000, 0x80000, BAD_DUMP CRC(028b561d) SHA1(27dcdb31b0951af99023b2fb8c370d8447ba6ebc), ROM_BIOS(2) )
#else
	ROM_SYSTEM_BIOS( 0, "tos400", "TOS 4.00" )
	ROMX_LOAD( "tos400.bin", 0x00000, 0x7ffff, BAD_DUMP CRC(1fbc5396) SHA1(d74d09f11a0bf37a86ccb50c6e7f91aac4d4b11b), ROM_BIOS(0) )
	ROM_SYSTEM_BIOS( 1, "tos402", "TOS 4.02" )
	ROMX_LOAD( "tos402.bin", 0x00000, 0x80000, BAD_DUMP CRC(63f82f23) SHA1(75de588f6bbc630fa9c814f738195da23b972cc6), ROM_BIOS(1) )
	ROM_SYSTEM_BIOS( 2, "tos404", "TOS 4.04" )
	ROMX_LOAD( "tos404.bin", 0x00000, 0x80000, BAD_DUMP CRC(028b561d) SHA1(27dcdb31b0951af99023b2fb8c370d8447ba6ebc), ROM_BIOS(2) )
#endif

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END


//-------------------------------------------------
//  ROM( falcon40 )
//-------------------------------------------------

ROM_START( falcon40 )
	ROM_REGION32_BE( 0x80000, M68000_TAG, 0 )
	ROM_SYSTEM_BIOS( 0, "tos492", "TOS 4.92" )
	ROMX_LOAD( "tos492.bin", 0x00000, 0x7d314, BAD_DUMP CRC(bc8e497f) SHA1(747a38042844a6b632dcd9a76d8525fccb5eb892), ROM_BIOS(0) )

	ROM_REGION( 0x1000, HD6301V1_TAG, 0 )
	ROM_LOAD( "keyboard.u1", 0x0000, 0x1000, CRC(0296915d) SHA1(1102f20d38f333234041c13687d82528b7cde2e1) )
ROM_END

} // anonymous namespace



//**************************************************************************
//  SYSTEM DRIVERS
//**************************************************************************

//    YEAR  NAME        PARENT    COMPAT  MACHINE   INPUT   CLASS          INIT        COMPANY  FULLNAME                 FLAGS
COMP( 1985, st,         0,        0,      st,       st,     st_state,      empty_init, "Atari", "ST (USA)",              MACHINE_NOT_WORKING )
COMP( 1985, st_uk,      st,       0,      st,       st,     st_state,      empty_init, "Atari", "ST (UK)",               MACHINE_NOT_WORKING )
COMP( 1985, st_de,      st,       0,      st,       st,     st_state,      empty_init, "Atari", "ST (Germany)",          MACHINE_NOT_WORKING )
COMP( 1985, st_es,      st,       0,      st,       st,     st_state,      empty_init, "Atari", "ST (Spain)",            MACHINE_NOT_WORKING )
COMP( 1985, st_fr,      st,       0,      st,       st,     st_state,      empty_init, "Atari", "ST (France)",           MACHINE_NOT_WORKING )
COMP( 1985, st_nl,      st,       0,      st,       st,     st_state,      empty_init, "Atari", "ST (Netherlands)",      MACHINE_NOT_WORKING )
COMP( 1985, st_se,      st,       0,      st,       st,     st_state,      empty_init, "Atari", "ST (Sweden)",           MACHINE_NOT_WORKING )
COMP( 1985, st_sg,      st,       0,      st,       st,     st_state,      empty_init, "Atari", "ST (Switzerland)",      MACHINE_NOT_WORKING )
COMP( 1987, megast,     st,       0,      megast,   st,     megast_state,  empty_init, "Atari", "MEGA ST (USA)",         MACHINE_NOT_WORKING )
COMP( 1987, megast_uk,  st,       0,      megast,   st,     megast_state,  empty_init, "Atari", "MEGA ST (UK)",          MACHINE_NOT_WORKING )
COMP( 1987, megast_de,  st,       0,      megast,   st,     megast_state,  empty_init, "Atari", "MEGA ST (Germany)",     MACHINE_NOT_WORKING )
COMP( 1987, megast_fr,  st,       0,      megast,   st,     megast_state,  empty_init, "Atari", "MEGA ST (France)",      MACHINE_NOT_WORKING )
COMP( 1987, megast_se,  st,       0,      megast,   st,     megast_state,  empty_init, "Atari", "MEGA ST (Sweden)",      MACHINE_NOT_WORKING )
COMP( 1987, megast_sg,  st,       0,      megast,   st,     megast_state,  empty_init, "Atari", "MEGA ST (Switzerland)", MACHINE_NOT_WORKING )
COMP( 1989, ste,        0,        0,      ste,      ste,    ste_state,     empty_init, "Atari", "STe (USA)",             MACHINE_NOT_WORKING )
COMP( 1989, ste_uk,     ste,      0,      ste,      ste,    ste_state,     empty_init, "Atari", "STe (UK)",              MACHINE_NOT_WORKING )
COMP( 1989, ste_de,     ste,      0,      ste,      ste,    ste_state,     empty_init, "Atari", "STe (Germany)",         MACHINE_NOT_WORKING )
COMP( 1989, ste_es,     ste,      0,      ste,      ste,    ste_state,     empty_init, "Atari", "STe (Spain)",           MACHINE_NOT_WORKING )
COMP( 1989, ste_fr,     ste,      0,      ste,      ste,    ste_state,     empty_init, "Atari", "STe (France)",          MACHINE_NOT_WORKING )
COMP( 1989, ste_it,     ste,      0,      ste,      ste,    ste_state,     empty_init, "Atari", "STe (Italy)",           MACHINE_NOT_WORKING )
COMP( 1989, ste_se,     ste,      0,      ste,      ste,    ste_state,     empty_init, "Atari", "STe (Sweden)",          MACHINE_NOT_WORKING )
COMP( 1989, ste_sg,     ste,      0,      ste,      ste,    ste_state,     empty_init, "Atari", "STe (Switzerland)",     MACHINE_NOT_WORKING )
//COMP( 1990, stbook,     ste,      0,      stbook,   stbook, stbook_state,  empty_init, "Atari", "STBook",                MACHINE_NOT_WORKING )
COMP( 1990, tt030,      0,        0,      tt030,    tt030,  ste_state,     empty_init, "Atari", "TT030 (USA)",           MACHINE_NOT_WORKING )
COMP( 1990, tt030_uk,   tt030,    0,      tt030,    tt030,  ste_state,     empty_init, "Atari", "TT030 (UK)",            MACHINE_NOT_WORKING )
COMP( 1990, tt030_de,   tt030,    0,      tt030,    tt030,  ste_state,     empty_init, "Atari", "TT030 (Germany)",       MACHINE_NOT_WORKING )
COMP( 1990, tt030_fr,   tt030,    0,      tt030,    tt030,  ste_state,     empty_init, "Atari", "TT030 (France)",        MACHINE_NOT_WORKING )
COMP( 1990, tt030_pl,   tt030,    0,      tt030,    tt030,  ste_state,     empty_init, "Atari", "TT030 (Poland)",        MACHINE_NOT_WORKING )
COMP( 1991, megaste,    ste,      0,      megaste,  st,     megaste_state, empty_init, "Atari", "MEGA STe (USA)",        MACHINE_NOT_WORKING )
COMP( 1991, megaste_uk, ste,      0,      megaste,  st,     megaste_state, empty_init, "Atari", "MEGA STe (UK)",         MACHINE_NOT_WORKING )
COMP( 1991, megaste_de, ste,      0,      megaste,  st,     megaste_state, empty_init, "Atari", "MEGA STe (Germany)",    MACHINE_NOT_WORKING )
COMP( 1991, megaste_es, ste,      0,      megaste,  st,     megaste_state, empty_init, "Atari", "MEGA STe (Spain)",      MACHINE_NOT_WORKING )
COMP( 1991, megaste_fr, ste,      0,      megaste,  st,     megaste_state, empty_init, "Atari", "MEGA STe (France)",     MACHINE_NOT_WORKING )
COMP( 1991, megaste_it, ste,      0,      megaste,  st,     megaste_state, empty_init, "Atari", "MEGA STe (Italy)",      MACHINE_NOT_WORKING )
COMP( 1991, megaste_se, ste,      0,      megaste,  st,     megaste_state, empty_init, "Atari", "MEGA STe (Sweden)",     MACHINE_NOT_WORKING )
COMP( 1992, falcon30,   0,        0,      falcon,   falcon, ste_state,     empty_init, "Atari", "Falcon030",             MACHINE_NOT_WORKING )
COMP( 1992, falcon40,   falcon30, 0,      falcon40, falcon, ste_state,     empty_init, "Atari", "Falcon040 (prototype)", MACHINE_NOT_WORKING )
//COMP( 1989, stacy,      st,       0,      stacy,    stacy,  st_state,      empty_init, "Atari", "Stacy",                 MACHINE_NOT_WORKING )
//COMP( 1991, stpad,      ste,      0,      stpad,    stpad,  st_state,      empty_init, "Atari", "STPad (prototype)",     MACHINE_NOT_WORKING )
//COMP( 1992, fx1,        0,        0,      falcon,   falcon, ste_state,     empty_init, "Atari", "FX-1 (prototype)",      MACHINE_NOT_WORKING )
