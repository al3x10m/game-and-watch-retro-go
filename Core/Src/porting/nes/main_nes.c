#include <odroid_system.h>

#include <string.h>
#include <nofrendo.h>
#include <bitmap.h>
#include <nes.h>
#include <nes_input.h>
#include <nes_state.h>
#include <nes_input.h>
#include <osd.h>
#include "main.h"
#include "gw_buttons.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "common.h"
#include "rom_manager.h"

#define APP_ID 30

// #define blit blit_nearest
// #define blit blit_normal
// #define blit blit_4to5
// #define blit blit_5to6

#ifndef blit
#define blit blit_5to6
#endif

static uint32_t pause_pressed;
static uint32_t power_pressed;

static bool fullFrame = 0;
static uint frameTime = 1000 / 60;
static uint32_t vsync_wait_ms = 0;

static bool autoload = false;


// if i counted correctly this should max be 23077
uint8_t nes_save_buffer[24000] __attribute__((section (".emulator_data")));

// TODO: Expose properly
extern int nes_state_save(uint8_t *flash_ptr, size_t size);

static bool SaveState(char *pathName)
{
    printf("Saving state...\n");

    nes_state_save(nes_save_buffer, 24000);
    store_save((uint8_t *) ACTIVE_FILE->save_address, nes_save_buffer, sizeof(nes_save_buffer));

    return 0;
}

// TODO: Expose properly
extern int nes_state_load(uint8_t* flash_ptr, size_t size);

static bool LoadState(char *pathName)
{
    nes_state_load((uint8_t *) ACTIVE_FILE->save_address, ACTIVE_FILE->save_size);
    return true;
}

int osd_init()
{
   return 0;
}

// TODO: Move to lcd.c/h
extern LTDC_HandleTypeDef hltdc;

static rgb_t *palette = NULL;
static uint16_t palette565[256];
static uint32_t palette_spaced_565[256];


void osd_setpalette(rgb_t *pal)
{
    palette = pal;
#ifdef GW_LCD_MODE_LUT8
    uint32_t clut[256];

    for (int i = 0; i < 64; i++)
    {
        uint16_t c = (pal[i].b>>3) | ((pal[i].g>>2)<<5) | ((pal[i].r>>3)<<11);

        // The upper bits are used to indicate background and transparency.
        // They need to be indexed as well.
        clut[i]        = (pal[i].b) | (pal[i].g << 8) | (pal[i].r << 16);
        clut[i | 0x40] = (pal[i].b) | (pal[i].g << 8) | (pal[i].r << 16);
        clut[i | 0x80] = (pal[i].b) | (pal[i].g << 8) | (pal[i].r << 16);
    }

    // Update the color-LUT in the LTDC peripheral
    HAL_LTDC_ConfigCLUT(&hltdc, clut, 256, 0);
    HAL_LTDC_EnableCLUT(&hltdc, 0);

    // color 13 is "black". Makes for a nice border.
    memset(framebuffer1, 13, sizeof(framebuffer1));
    memset(framebuffer2, 13, sizeof(framebuffer2));

    odroid_display_force_refresh();
#else
    for (int i = 0; i < 64; i++)
    {
        uint16_t c = (pal[i].b>>3) | ((pal[i].g>>2)<<5) | ((pal[i].r>>3)<<11);

        // The upper bits are used to indicate background and transparency.
        // They need to be indexed as well.
        palette565[i]        = c;
        palette565[i | 0x40] = c;
        palette565[i | 0x80] = c;

        uint32_t sc = ((0b1111100000000000&c)<<10) | ((0b0000011111100000&c)<<5) | ((0b0000000000011111&c));
        palette_spaced_565[i] = sc;
        palette_spaced_565[i | 0x40] = sc;
        palette_spaced_565[i | 0x80] = sc;

    }

#endif
}

static uint32_t skippedFrames = 0;

void osd_wait_for_vsync()
{
    static uint32_t skipFrames = 0;
    static uint32_t lastSyncTime = 0;
    uint32_t t0;

    uint32_t elapsed = get_elapsed_time_since(lastSyncTime);

    if (skipFrames == 0) {
        rg_app_desc_t *app = odroid_system_get_app();
        if (elapsed > frameTime) skipFrames = 1;
        if (app->speedupEnabled) skipFrames += app->speedupEnabled * 2;
    } else if (skipFrames > 0) {
        skipFrames--;
        skippedFrames++;
    }

    // Tick before submitting audio/syncing
    odroid_system_tick(!nes_getptr()->drawframe, fullFrame, elapsed);

    nes_getptr()->drawframe = (skipFrames == 0);

    // Wait until the audio buffer has been transmitted
    static uint32_t last_dma_counter = 0;
    t0 = get_elapsed_time();
    while (dma_counter == last_dma_counter) {
        __WFI();
    }
    vsync_wait_ms += get_elapsed_time_since(t0);

    last_dma_counter = dma_counter;

    lastSyncTime = get_elapsed_time();
}

void osd_audioframe(int audioSamples)
{
    if (odroid_system_get_app()->speedupEnabled)
        return;

    apu_process(audiobuffer_emulator, audioSamples); //get audio data

    size_t offset = (dma_state == DMA_TRANSFER_STATE_HF) ? 0 : audioSamples;

    // MUST shift with at least 1 place, or it will brownout.
    uint8_t volume = odroid_audio_volume_get();
    uint8_t shift = ODROID_AUDIO_VOLUME_MAX - volume + 1;

    if (volume == ODROID_AUDIO_VOLUME_MIN) {
        // mute
        for (int i = 0; i < audioSamples; i++) {
            audiobuffer_dma[i + offset] = 0;
        }
        return;
    }

    // Write to DMA buffer and lower the volume to 1/4
    for (int i = 0; i < audioSamples; i++) {
        audiobuffer_dma[i + offset] = audiobuffer_emulator[i] >> shift;
    }
}


#ifdef GW_LCD_MODE_LUT8
static inline void blit_normal(bitmap_t *bmp, uint8_t *framebuffer) {
        // LCD is 320 wide, framebuffer is only 256
    const int hpad = (WIDTH - NES_SCREEN_WIDTH) / 2;

    for (int y = 0; y < bmp->height; y++) {
        uint8_t *row = bmp->line[y];
        uint32 *dest = NULL;
        if(active_framebuffer == 0) {
            dest = &framebuffer[WIDTH * y + hpad];
        } else {
            dest = &framebuffer[WIDTH * y + hpad];
        }
        memcpy(dest, row, bmp->width);
    }
}

static inline void blit_nearest(bitmap_t *bmp, uint8_t *framebuffer) {
    int w1 = bmp->width;
    int h1 = bmp->height;
    int w2 = WIDTH;
    int h2 = h1;

    // Blit: 5581 us
    // This can still be improved quite a bit by using aligned accesses.

    int ctr = 0;
    for (int y = 0; y < h2; y++) {
        uint8_t  *src_row  = bmp->line[y];
        uint8_t *dest_row = &framebuffer[y * w2];
        int x2 = 0;
        for (int x = 0; x < w1; x++) {
            uint8_t b2 = src_row[x];
            dest_row[x2++] = b2;
            if (ctr++ == 4) {
                ctr = 0;
                dest_row[x2++] = b2;
            }
        }
    }
}
#else

__attribute__((optimize("unroll-loops")))
static inline void blit_normal(bitmap_t *bmp, uint16_t *framebuffer) {
    const int w1 = bmp->width;
    const int w2 = 320;
    const int h2 = 240;
    const int hpad = 27;

    for (int y = 0; y < h2; y++) {
        uint8_t  *src_row  = bmp->line[y];
        uint16_t *dest_row = &framebuffer[y * w2 + hpad];
        for (int x = 0; x < w1; x++) {
            dest_row[x] = palette565[src_row[x]];
        }
    }
}

__attribute__((optimize("unroll-loops")))
static inline void blit_nearest(bitmap_t *bmp, uint16_t *framebuffer) {
    int w1 = bmp->width;
    int w2 = WIDTH;
    int h2 = 240;

// #define SCALE_TO_320
#define SCALE_TO_307

#ifdef SCALE_TO_307
    const int hpad = (WIDTH - 307) / 2;
#   define SCALE_CTR 4
#else
    const int hpad = 0;
#   define SCALE_CTR 3
#endif

    // 1767 us
    PROFILING_INIT(t_blit);
    PROFILING_START(t_blit);

    for (int y = 0; y < h2; y++) {
        int ctr = 0;
        uint8_t  *src_row  = bmp->line[y];
        uint16_t *dest_row = &framebuffer[y * w2 + hpad];
        int x2 = 0;
        for (int x = 0; x < w1; x++) {
            uint16_t b2 = palette565[src_row[x]];
            dest_row[x2++] = b2;
            if (ctr++ == SCALE_CTR) {
                ctr = 0;
                dest_row[x2++] = b2;
            }
        }
    }

    PROFILING_END(t_blit);

#ifdef PROFILING_ENABLED
    printf("Blit: %d us\n", (1000000 * PROFILING_DIFF(t_blit)) / t_blit_t0.SecondFraction);
#endif
}

#define CONV(_b0) ((0b11111000000000000000000000&_b0)>>10) | ((0b000001111110000000000&_b0)>>5) | ((0b0000000000011111&_b0));

__attribute__((optimize("unroll-loops")))
static void blit_4to5(bitmap_t *bmp, uint16_t *framebuffer) {
    int w1 = bmp->width;
    int w2 = WIDTH;
    int h2 = 240;

    // 1767 us

    for (int y = 0; y < h2; y++) {
        uint8_t  *src_row  = bmp->line[y];
        uint16_t *dest_row = &framebuffer[y * w2];
        for (int x_src = 0, x_dst=0; x_src < w1; x_src+=4, x_dst+=5) {
            uint32_t b0 = palette_spaced_565[src_row[x_src]];
            uint32_t b1 = palette_spaced_565[src_row[x_src+1]];
            uint32_t b2 = palette_spaced_565[src_row[x_src+2]];
            uint32_t b3 = palette_spaced_565[src_row[x_src+3]];

            dest_row[x_dst]   = CONV(b0);
            dest_row[x_dst+1] = CONV((b0+b0+b0+b1)>>2);
            dest_row[x_dst+2] = CONV((b1+b2)>>1);
            dest_row[x_dst+3] = CONV((b2+b2+b2+b3)>>2);
            dest_row[x_dst+4] = CONV(b3);
        }
    }
}


__attribute__((optimize("unroll-loops")))
static void blit_5to6(bitmap_t *bmp, uint16_t *framebuffer) {
    int w1_adjusted = bmp->width - 4;
    int w2 = WIDTH;
    int h2 = 240;
    const int hpad = (WIDTH - 307) / 2;

    // Blit: 2015 us

    for (int y = 0; y < h2; y++) {
        uint8_t  *src_row  = bmp->line[y];
        uint16_t *dest_row = &framebuffer[y * w2 + hpad];
        int x_src = 0;
        int x_dst = 0;
        for (; x_src < w1_adjusted; x_src+=5, x_dst+=6) {
            uint32_t b0 = palette_spaced_565[src_row[x_src]];
            uint32_t b1 = palette_spaced_565[src_row[x_src+1]];
            uint32_t b2 = palette_spaced_565[src_row[x_src+2]];
            uint32_t b3 = palette_spaced_565[src_row[x_src+3]];
            uint32_t b4 = palette_spaced_565[src_row[x_src+4]];

            dest_row[x_dst]   = CONV(b0);
            dest_row[x_dst+1] = CONV((b0+b1+b1+b1)>>2);
            dest_row[x_dst+2] = CONV((b1+b2)>>1);
            dest_row[x_dst+3] = CONV((b2+b3)>>1);
            dest_row[x_dst+4] = CONV((b3+b3+b3+b4)>>2);
            dest_row[x_dst+5] = CONV(b4);
        }
        // Last column, x_src=255
        dest_row[x_dst] = palette565[src_row[x_src]];
    }
}
#endif


void osd_blitscreen(bitmap_t *bmp)
{
    static uint32_t lastFPSTime = 0;
    static uint32_t frames = 0;
    uint32_t currentTime = HAL_GetTick();
    uint32_t delta = currentTime - lastFPSTime;

    frames++;

    if (delta >= 1000) {
        int fps = (10000 * frames) / delta;
        printf("FPS: %d.%d, frames %ld, delta %ld ms, skipped %ld\n", fps / 10, fps % 10, frames, delta, skippedFrames);
        frames = 0;
        skippedFrames = 0;
        vsync_wait_ms = 0;
        lastFPSTime = currentTime;
    }

    PROFILING_INIT(t_blit);
    PROFILING_START(t_blit);

    // This takes less than 1ms
    if(active_framebuffer == 0) {
        blit(bmp, framebuffer1);
        active_framebuffer = 1;
    } else {
        blit(bmp, framebuffer2);
        active_framebuffer = 0;
    }

    PROFILING_END(t_blit);

#ifdef PROFILING_ENABLED
    printf("Blit: %d us\n", (1000000 * PROFILING_DIFF(t_blit)) / t_blit_t0.SecondFraction);
#endif

    HAL_LTDC_Reload(&hltdc, LTDC_RELOAD_VERTICAL_BLANKING);
}

static bool palette_update_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event)
{
   int pal = ppu_getopt(PPU_PALETTE_RGB);
   int max = PPU_PAL_COUNT - 1;

   if (event == ODROID_DIALOG_PREV) pal = pal > 0 ? pal - 1 : max;
   if (event == ODROID_DIALOG_NEXT) pal = pal < max ? pal + 1 : 0;

   if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
      odroid_settings_Palette_set(pal);
      ppu_setopt(PPU_PALETTE_RGB, pal);
   }

   sprintf(option->value, "%.7s", ppu_getpalette(pal)->name);
   return event == ODROID_DIALOG_ENTER;
}

void osd_getinput(void)
{
    uint16 pad0 = 0;

    uint32_t buttons = buttons_get();
    if(buttons & B_GAME) pad0 |= INP_PAD_START;
    if(buttons & B_TIME) pad0 |= INP_PAD_SELECT;
    if(buttons & B_Up)   pad0 |= INP_PAD_UP;
    if(buttons & B_Down)   pad0 |= INP_PAD_DOWN;
    if(buttons & B_Left)   pad0 |= INP_PAD_LEFT;
    if(buttons & B_Right)   pad0 |= INP_PAD_RIGHT;
    if(buttons & B_A)   pad0 |= INP_PAD_A;
    if(buttons & B_B)   pad0 |= INP_PAD_B;

    if (pause_pressed != (buttons & B_PAUSE)) {
        if (pause_pressed) {
            printf("Pause pressed %ld=>%d\n", audio_mute, !audio_mute);

            odroid_dialog_choice_t options[] = {
                    {100, "Palette", "Default", 1, &palette_update_cb},
                    // {101, "More...", "", 1, &advanced_settings_cb},
                    ODROID_DIALOG_CHOICE_LAST
            };

            odroid_overlay_game_menu(options);
            memset(framebuffer1, 0x0, sizeof(framebuffer1));
            memset(framebuffer2, 0x0, sizeof(framebuffer2));
        }
        pause_pressed = buttons & B_PAUSE;
    }

    if (power_pressed != (buttons & B_POWER)) {
        printf("Power toggle %ld=>%d\n", power_pressed, !power_pressed);
        power_pressed = buttons & B_POWER;
        if (buttons & B_POWER) {
            printf("Power PRESSED %ld\n", power_pressed);
            HAL_SAI_DMAStop(&hsai_BlockA1);

            if(!(buttons & B_PAUSE)) {
                SaveState("");
            }

            GW_EnterDeepSleep();
        }
    }

    // Enable to log button presses
#if 0
    static old_pad0;
    if (pad0 != old_pad0) {
        printf("pad0=%02x\n", pad0);
        old_pad0 = pad0;
    }
#endif

    input_update(INP_JOYPAD0, pad0);
}

size_t osd_getromdata(unsigned char **data)
{
    *data = ROM_DATA;
    return ROM_DATA_LENGTH;
}

uint osd_getromcrc()
{
   return 0x1337;
}

void osd_loadstate()
{
    if(autoload) {
        autoload = false;
        LoadState("");
    }
}


int app_main_nes(uint8_t load_state)
{
    region_t nes_region;

    memset(framebuffer1, 0x0, sizeof(framebuffer1));
    memset(framebuffer2, 0x0, sizeof(framebuffer2));
    odroid_system_init(APP_ID, AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, NULL);

    uint32_t buttons = GW_GetBootButtons();
    pause_pressed = (buttons & B_PAUSE);
    power_pressed = (buttons & B_POWER);

    autoload = load_state;

    printf("Nofrendo start!\n");

    memset(audiobuffer_dma, 0, sizeof(audiobuffer_dma));

    if (ACTIVE_FILE->region == REGION_PAL) {
        nes_region = NES_PAL;
        frameTime = 1000 / 50;
        HAL_SAI_Transmit_DMA(&hsai_BlockA1, (uint8_t *) audiobuffer_dma,  (2 * AUDIO_SAMPLE_RATE) / 50);
    } else {
        nes_region = NES_NTSC;
        frameTime = 1000 / 60;
        HAL_SAI_Transmit_DMA(&hsai_BlockA1, (uint8_t *) audiobuffer_dma, (2 * AUDIO_SAMPLE_RATE) / 60);
    }

    nofrendo_start(ACTIVE_FILE->name, nes_region, AUDIO_SAMPLE_RATE);

    return 0;
}
