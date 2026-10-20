/*
 * spu.h - the PS1 sound processor in software: 512 KB of sound RAM, 24
 * voices playing ADPCM samples with ADSR envelopes, and the reverb.
 *
 * LIBSND/LIBSPU (libsnd.c) drive it like the hardware registers; the PC
 * side takes 44.1 kHz stereo samples from it every frame.
 */
#ifndef PSX_SPU_H
#define PSX_SPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_RAM_SIZE            0x80000
#define SPU_VOICES              24
#define SPU_SAMPLE_RATE         44100

extern uint8_t g_spu_ram[SPU_RAM_SIZE];

void spu_reset(void);

/* voice registers (addresses in bytes) */
void spu_voice_set_volume(int voice, int left, int right);     /* 0..3FFFh */
void spu_voice_set_pitch(int voice, int pitch);                 /* 1000h = 44.1 kHz */
void spu_voice_set_start(int voice, uint32_t address);
void spu_voice_set_adsr(int voice, uint16_t adsr1, uint16_t adsr2);
void spu_voice_set_reverb(int voice, int on);
void spu_key_on(uint32_t voices);       /* bit per voice */
void spu_key_off(uint32_t voices);
int spu_voice_active(int voice);        /* envelope not yet silent */
int spu_voice_releasing(int voice);

/* main volume (0..3FFFh), reverb output volume (-8000h..7FFFh) */
void spu_set_main_volume(int left, int right);
void spu_set_reverb_volume(int left, int right);
void spu_set_reverb_enabled(int on);
/* the 32 reverb registers (dAPF1 .. vRIN) and the work area start */
void spu_set_reverb_registers(const uint16_t regs[32], uint32_t base_address);
void spu_clear_reverb_area(void);

/* Mixes `frames` stereo samples (interleaved L, R) at 44.1 kHz. */
void spu_render(int16_t *out, int frames);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SPU_H */
