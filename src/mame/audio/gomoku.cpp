// license:BSD-3-Clause
// copyright-holders:David Haywood
/***************************************************************************

    Gomoku sound driver (quick hack of the Wiping sound driver)

    used by gomoku.cpp

***************************************************************************/

#include "emu.h"
#include "audio/gomoku.h"

constexpr int DEFGAIN = 48;


// device type definition
DEFINE_DEVICE_TYPE(GOMOKU_SOUND, gomoku_sound_device, "gomoku_sound", "Gomoku Narabe Renju Custom Sound")


//**************************************************************************
//  LIVE DEVICE
//**************************************************************************

//-------------------------------------------------
//  gomoku_sound_device - constructor
//-------------------------------------------------

gomoku_sound_device::gomoku_sound_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, GOMOKU_SOUND, tag, owner, clock)
	, device_sound_interface(mconfig, *this)
	, m_sound_rom(*this, DEVICE_SELF)
	, m_sound_enable(0)
	, m_stream(nullptr)
	, m_mixer_table(nullptr)
	, m_mixer_lookup(nullptr)
	, m_mixer_buffer(nullptr)
	, m_mixer_buffer_2(nullptr)
{
	std::fill(std::begin(m_soundregs1), std::end(m_soundregs1), 0);
	std::fill(std::begin(m_soundregs2), std::end(m_soundregs2), 0);
}


//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void gomoku_sound_device::device_start()
{
	sound_channel *voice;
	int ch;

	// get stream channels
	m_stream = stream_alloc(0, 1, clock());

	// allocate a pair of buffers to mix into - 1 second's worth should be more than enough
	m_mixer_buffer = std::make_unique<short[]>(2 * clock());
	m_mixer_buffer_2 = m_mixer_buffer.get() + clock(); // this is never used?

	// build the mixer table
	make_mixer_table(8, DEFGAIN);

	// start with sound enabled, many games don't have a sound enable register
	m_sound_enable = 1;

	// reset all the voices
	for (ch = 0, voice = std::begin(m_channel_list); voice < std::end(m_channel_list); ch++, voice++)
	{
		voice->channel = ch;
		voice->frequency = 0;
		voice->counter = 0;
		voice->volume = 0;
		voice->oneshotplaying = 0;
	}

	save_item(NAME(m_soundregs1));
	save_item(NAME(m_soundregs2));
	save_pointer(NAME(m_mixer_buffer), 2 * clock());
	// save_item(NAME(m_sound_enable)); // set to 1 at device start and never updated?
	save_item(STRUCT_MEMBER(m_channel_list, channel));
	save_item(STRUCT_MEMBER(m_channel_list, frequency));
	save_item(STRUCT_MEMBER(m_channel_list, counter));
	save_item(STRUCT_MEMBER(m_channel_list, volume));
	save_item(STRUCT_MEMBER(m_channel_list, oneshotplaying));
}


//-------------------------------------------------
//  sound_stream_update - handle a stream update in mono
//-------------------------------------------------

void gomoku_sound_device::sound_stream_update(sound_stream &stream, stream_sample_t **inputs, stream_sample_t **outputs, int samples)
{
	stream_sample_t *buffer = outputs[0];
	sound_channel *voice;
	short *mix;
	int ch;

	// if no sound, we're done
	if (m_sound_enable == 0)
	{
		memset(buffer, 0, samples * sizeof(*buffer));
		return;
	}

	// zap the contents of the mixer buffer
	memset(m_mixer_buffer.get(), 0, samples * sizeof(short));

	// loop over each voice and add its contribution
	for (ch = 0, voice = std::begin(m_channel_list); voice < std::end(m_channel_list); ch++, voice++)
	{
		int f = 16 * voice->frequency;
		int v = voice->volume;

		// only update if we have non-zero volume and frequency
		if (v && f)
		{
			int w_base;
			int c = voice->counter;

			if (ch < 3)
				w_base = 0x20 * (m_soundregs1[0x06 + (ch * 8)] & 0x0f);
			else
				w_base = 0x100 * (m_soundregs2[0x1d] & 0x0f);

			mix = m_mixer_buffer.get();

			// add our contribution
			for (int i = 0; i < samples; i++)
			{
				c += f;

				if (ch < 3)
				{
					int offs = w_base | ((c >> 16) & 0x1f);

					// use full byte, first the high 4 bits, then the low 4 bits
					if (c & 0x8000)
						*mix++ += ((m_sound_rom[offs] & 0x0f) - 8) * v;
					else
						*mix++ += (((m_sound_rom[offs]>>4) & 0x0f) - 8) * v;
				}
				else
				{
					int offs = (w_base + (c >> 16)) & 0x0fff;

					if (m_sound_rom[offs] == 0xff)
					{
						voice->oneshotplaying = 0;
					}

					if (voice->oneshotplaying)
					{
						// use full byte, first the high 4 bits, then the low 4 bits
						if (c & 0x8000)
							*mix++ += ((m_sound_rom[offs] & 0x0f) - 8) * v;
						else
							*mix++ += (((m_sound_rom[offs]>>4) & 0x0f) - 8) * v;
					}
				}

				// update the counter for this voice
				voice->counter = c;
			}
		}
	}

	// mix it down
	mix = m_mixer_buffer.get();
	for (int i = 0; i < samples; i++)
		*buffer++ = m_mixer_lookup[*mix++];
}


// build a table to divide by the number of voices; gain is specified as gain*16
void gomoku_sound_device::make_mixer_table(int voices, int gain)
{
	int count = voices * 128;

	// allocate memory
	m_mixer_table = std::make_unique<int16_t[]>(256 * voices);

	// find the middle of the table
	m_mixer_lookup = m_mixer_table.get() + (128 * voices);

	// fill in the table - 16 bit case
	for (int i = 0; i < count; i++)
	{
		int val = i * gain * 16 / voices;
		if (val > 32767) val = 32767;
		m_mixer_lookup[ i] = val;
		m_mixer_lookup[-i] = -val;
	}
}


/********************************************************************************/

void gomoku_sound_device::sound1_w(offs_t offset, uint8_t data)
{
	sound_channel *voice;
	int base;
	int ch;

	// update the streams
	m_stream->update();

	// set the register
	m_soundregs1[offset] = data;

	// recompute all the voice parameters
	for (ch = 0, base = 0, voice = m_channel_list; voice < m_channel_list + 3; ch++, voice++, base += 8)
	{
		voice->channel = ch;
		voice->frequency = m_soundregs1[0x02 + base] & 0x0f;
		voice->frequency = voice->frequency * 16 + ((m_soundregs1[0x01 + base]) & 0x0f);
		voice->frequency = voice->frequency * 16 + ((m_soundregs1[0x00 + base]) & 0x0f);
	}
}

void gomoku_sound_device::sound2_w(offs_t offset, uint8_t data)
{
	sound_channel *voice;
	int base;
	int ch;

	// update the streams
	m_stream->update();

	// set the register
	m_soundregs2[offset] = data;

	// recompute all the voice parameters
	for (ch = 0, base = 0, voice = m_channel_list; voice < m_channel_list + 3; ch++, voice++, base += 8)
	{
		voice->channel = ch;
		voice->volume = m_soundregs2[0x06 + base] & 0x0f;
		voice->oneshotplaying = 0;
	}

	if (offset == 0x1d)
	{
		voice = &m_channel_list[3];
		voice->channel = 3;

		// oneshot frequency is hand tune...
		if ((m_soundregs2[0x1d] & 0x0f) < 0x0c)
			voice->frequency = 3000 / 16;           // ichi, ni, san, yon, go
		else
			voice->frequency = 8000 / 16;           // shoot

		voice->volume = 8;
		voice->counter = 0;

		if (m_soundregs2[0x1d] & 0x0f)
			voice->oneshotplaying = 1;
		else
			voice->oneshotplaying = 0;
	}
}
