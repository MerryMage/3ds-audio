#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cfenv>

#include <3ds.h>

#include "audio.h"

// Very slow triangle wave, mono PCM16
void fillBuffer(s16 *audio_buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        audio_buffer[i] = rand();
    }

    DSP_FlushDataCache(audio_buffer, size);
}

void waitForKey() {
    while (aptMainLoop()) {
        gfxSwapBuffers();
        gfxFlushBuffers();
        gspWaitForVBlank();

        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown)
            break;
    }
}

int main(int argc, char **argv) {
    gfxInitDefault();

    PrintConsole botScreen;
    PrintConsole topScreen;

    consoleInit(GFX_TOP, &topScreen);
    consoleInit(GFX_BOTTOM, &botScreen);
    consoleSelect(&topScreen);

    srand(time(nullptr));

    constexpr size_t NUM_SAMPLES = 160*200;
    s16 *audio_buffer = (s16*)linearAlloc(NUM_SAMPLES * sizeof(s16));
    fillBuffer(audio_buffer, NUM_SAMPLES);

    AudioState state;
    {
        auto dspfirm = loadDspFirmFromFile();
        if (!dspfirm) {
            printf("Couldn't load firmware\n");
            goto end;
        }
        auto ret = audioInit(*dspfirm);
        if (!ret) {
            printf("Couldn't init audio\n");
            goto end;
        }
        state = *ret;
    }

    {
        float rate_multiplier = rand() % 512 / 128.f;
        printf("rate_multiplier = %f\n", rate_multiplier);

        array<s32, 160> expected_output;
        array<bool, 160> bad;
        {
            constexpr s32 scale = 1 << 16;
            u32 scaled_rate = rate_multiplier * scale;
            int fposition = -2 * scale;
            for (int i=0; i<160; i++) {
                int position = fposition >> 16;
                const s32 x0 = position+0 >= 0 ? audio_buffer[position+0] : 0;
                const s32 x1 = position+1 >= 0 ? audio_buffer[position+1] : 0;

                s32 delta = x1 - x0;
                if (delta > 0x7FFF) delta = 0x7FFF;
                if (delta < -0x8000) delta = -0x8000;

                u16 f0 = fposition & 0xFFFF;

                if (f0) {
                    expected_output[i] = x0 + ((f0 * delta) >> 16);
                    bad[i] = abs(x0 - x1) > 0x8000;
                } else {
                    expected_output[i] = x0;
                }

                fposition += scaled_rate;
            }
        }

        state.waitForSync();
        initSharedMem(state);
        state.write().dsp_configuration->mixer1_enabled_dirty = true;
        state.write().dsp_configuration->mixer1_enabled = true;
        state.write().source_configurations->config[0].gain[1][0] = 1.0;
        state.write().source_configurations->config[0].gain_1_dirty = true;
        state.notifyDsp();
        printf("init\n");

        bool entered = false;
        bool passed = true;
        {
            u16 buffer_id = 0;

            state.write().source_configurations->config[0].play_position = 0;
            state.write().source_configurations->config[0].physical_address = osConvertVirtToPhys(audio_buffer);
            state.write().source_configurations->config[0].length = NUM_SAMPLES;
            state.write().source_configurations->config[0].mono_or_stereo = DSP::HLE::SourceConfiguration::Configuration::MonoOrStereo::Mono;
            state.write().source_configurations->config[0].format = DSP::HLE::SourceConfiguration::Configuration::Format::PCM16;
            state.write().source_configurations->config[0].fade_in = false;
            state.write().source_configurations->config[0].adpcm_dirty = false;
            state.write().source_configurations->config[0].is_looping = false;
            state.write().source_configurations->config[0].buffer_id = ++buffer_id;
            state.write().source_configurations->config[0].partial_reset_flag = true;
            state.write().source_configurations->config[0].play_position_dirty = true;
            state.write().source_configurations->config[0].embedded_buffer_dirty = true;

            state.write().source_configurations->config[0].enable = true;
            state.write().source_configurations->config[0].enable_dirty = true;

            state.write().source_configurations->config[0].rate_multiplier = rate_multiplier;
            state.write().source_configurations->config[0].rate_multiplier_dirty = true;
            state.write().source_configurations->config[0].interpolation_mode = DSP::HLE::SourceConfiguration::Configuration::InterpolationMode::Linear;
            state.write().source_configurations->config[0].interpolation_related = 0;
            state.write().source_configurations->config[0].interpolation_dirty = true;

            state.notifyDsp();

            bool continue_reading = true;
            for (size_t frame_count = 0; continue_reading && frame_count < 10; frame_count++) {
                state.waitForSync();

                for (size_t i = 0; i < 160; i++) {
                    if (state.write().intermediate_mix_samples->mix1.pcm32[0][i]) {
                        entered = true;
                        printf("[intermediate] frame=%i, sample=%i\n", frame_count, i);
                        for (size_t j = 0; j < 160; j++) {
                            s32 real = (s32)state.write().intermediate_mix_samples->mix1.pcm32[0][j];
                            s32 expect = (s32)expected_output[j];
                            if (real != expect) {
                                printf("[%i] real=%08lx expect=%08lx %s\n", j, real, expect, bad[j] ? "bad" : "");
                                passed = false;
                            }
                        }
                        continue_reading = false;
                        printf("\n");
                        break;
                    }
                }

                state.notifyDsp();
            }

            printf("Done!\n");
            if (entered && passed) {
                printf("Test passed!\n");
            } else {
                printf("FAIL\n");
            }
        }
    }

end:
    audioExit(state);
    waitForKey();
    gfxExit();
    return 0;
}
