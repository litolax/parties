#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <rnnoise.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RNNoise operates on 480-sample frames (10 ms at 48 kHz). */
#define RNNOISE_FRAME_SIZE 480
#define SAMPLE_RATE        48000
#define CHANNELS           1

typedef struct {
    DenoiseState *rnnoise;
    /* Ring buffer for passing denoised audio from capture callback to playback callback. */
    ma_pcm_rb    ring;
} UserData;

/*
 * Duplex callback: miniaudio delivers captured samples in pInput and expects
 * us to fill pOutput with playback samples, all in f32 / 48 kHz / mono.
 *
 * We process full 480-sample RNNoise frames.  Any leftover samples that don't
 * fill a complete frame are kept in the ring buffer for the next callback.
 */
static void data_callback(ma_device *pDevice, void *pOutput, const void *pInput,
                           ma_uint32 frameCount)
{
    UserData *ud        = (UserData *)pDevice->pUserData;
    const float *in     = (const float *)pInput;
    float *out          = (float *)pOutput;
    ma_uint32 remaining = frameCount;

    /* --- Write captured frames through RNNoise into the ring buffer --- */
    while (remaining > 0) {
        ma_uint32 toProcess = (remaining >= RNNOISE_FRAME_SIZE) ? RNNOISE_FRAME_SIZE : remaining;

        if (toProcess < RNNOISE_FRAME_SIZE) {
            /* Not enough for a full RNNoise frame; push raw samples so we
               don't lose them – they will be heard unprocessed. */
            void *pWrite;
            ma_pcm_rb_acquire_write(&ud->ring, &toProcess, &pWrite);
            if (toProcess > 0) {
                memcpy(pWrite, in, toProcess * sizeof(float));
                ma_pcm_rb_commit_write(&ud->ring, toProcess);
            }
            break;
        }

        /* RNNoise expects input scaled to [-32768, 32768]. */
        float buf[RNNOISE_FRAME_SIZE];
        for (ma_uint32 i = 0; i < RNNOISE_FRAME_SIZE; i++)
            buf[i] = in[i] * 32768.0f;

        rnnoise_process_frame(ud->rnnoise, buf, buf);

        /* Scale back to [-1, 1]. */
        for (ma_uint32 i = 0; i < RNNOISE_FRAME_SIZE; i++)
            buf[i] /= 32768.0f;

        ma_uint32 frames = RNNOISE_FRAME_SIZE;
        void *pWrite;
        ma_pcm_rb_acquire_write(&ud->ring, &frames, &pWrite);
        if (frames > 0) {
            memcpy(pWrite, buf, frames * sizeof(float));
            ma_pcm_rb_commit_write(&ud->ring, frames);
        }

        in        += RNNOISE_FRAME_SIZE;
        remaining -= RNNOISE_FRAME_SIZE;
    }

    /* --- Read denoised frames from the ring buffer into the output --- */
    ma_uint32 toRead = frameCount;
    void *pRead;
    ma_pcm_rb_acquire_read(&ud->ring, &toRead, &pRead);
    if (toRead > 0) {
        memcpy(out, pRead, toRead * sizeof(float));
        ma_pcm_rb_commit_read(&ud->ring, toRead);
    }
    /* Silence any remaining output if ring buffer didn't have enough. */
    if (toRead < frameCount)
        memset(out + toRead, 0, (frameCount - toRead) * sizeof(float));
}

int main(void)
{
    UserData ud;
    memset(&ud, 0, sizeof(ud));

    /* Create RNNoise denoiser. */
    ud.rnnoise = rnnoise_create(NULL);
    if (!ud.rnnoise) {
        fprintf(stderr, "Failed to create RNNoise state\n");
        return 1;
    }

    /* Initialise ring buffer – 50 ms worth of headroom. */
    ma_result result = ma_pcm_rb_init(ma_format_f32, CHANNELS,
                                       SAMPLE_RATE / 20, /* ~50 ms */
                                       NULL, NULL, &ud.ring);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Failed to init ring buffer: %d\n", result);
        rnnoise_destroy(ud.rnnoise);
        return 1;
    }

    /* Configure a full-duplex device (capture + playback). */
    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.capture.format   = ma_format_f32;
    config.capture.channels = CHANNELS;
    config.playback.format  = ma_format_f32;
    config.playback.channels = CHANNELS;
    config.sampleRate       = SAMPLE_RATE;
    config.dataCallback     = data_callback;
    config.pUserData        = &ud;

    ma_device device;
    result = ma_device_init(NULL, &config, &device);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Failed to initialise audio device: %d\n", result);
        ma_pcm_rb_uninit(&ud.ring);
        rnnoise_destroy(ud.rnnoise);
        return 1;
    }

    result = ma_device_start(&device);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Failed to start audio device: %d\n", result);
        ma_device_uninit(&device);
        ma_pcm_rb_uninit(&ud.ring);
        rnnoise_destroy(ud.rnnoise);
        return 1;
    }

    printf("Mic -> RNNoise -> Speaker  (f32, %d Hz, %d ch)\n", SAMPLE_RATE, CHANNELS);
    printf("Press Enter to quit...\n");
    getchar();

    ma_device_uninit(&device);
    ma_pcm_rb_uninit(&ud.ring);
    rnnoise_destroy(ud.rnnoise);

    return 0;
}
