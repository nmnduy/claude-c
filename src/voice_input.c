/*
 * voice_input.c - Voice Input Implementation
 *
 * Record microphone audio and transcribe via OpenAI Whisper API
 * Based on mic_to_openai_transcribe.c from cc-mic2text project
 */

#define _CRT_SECURE_NO_WARNINGS
#include "voice_input.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <curl/curl.h>
#include <float.h>

#ifdef HAVE_PORTAUDIO
#include <portaudio.h>
#endif

// ===== Recording config =====
#define SAMPLE_RATE   16000
#define NUM_CHANNELS  1
#define FRAMES_PER_BUFFER  512
#define BITS_PER_SAMPLE 16

#ifdef HAVE_PORTAUDIO
// Simple dynamic buffer for PCM samples
typedef struct {
    int16_t *data;
    size_t frames;     // number of frames currently stored
    size_t capacity;   // capacity in frames
    pthread_mutex_t mu;
    bool recording;
} AudioBuffer;

static void ab_init(AudioBuffer *ab) {
    ab->data = NULL;
    ab->frames = 0;
    ab->capacity = 0;
    pthread_mutex_init(&ab->mu, NULL);
    ab->recording = true;
}

static void ab_free(AudioBuffer *ab) {
    free(ab->data);
    ab->data = NULL;
    pthread_mutex_destroy(&ab->mu);
}

static void ab_append(AudioBuffer *ab, const int16_t *frames, size_t nframes) {
    pthread_mutex_lock(&ab->mu);
    if (ab->frames + nframes > ab->capacity) {
        size_t newcap = ab->capacity ? ab->capacity : 16384; // start with ~1 sec
        while (ab->frames + nframes > newcap) newcap *= 2;
        ab->data = (int16_t*)realloc(ab->data, newcap * NUM_CHANNELS * sizeof(int16_t));
        if (!ab->data) {
            LOG_ERROR("Failed to allocate audio buffer");
            pthread_mutex_unlock(&ab->mu);
            return;
        }
        ab->capacity = newcap;
    }
    memcpy(ab->data + ab->frames * NUM_CHANNELS, frames, nframes * NUM_CHANNELS * sizeof(int16_t));
    ab->frames += nframes;
    pthread_mutex_unlock(&ab->mu);
}

// PortAudio callback: record mic into AudioBuffer
static int pa_record_cb(const void *input, void *output, unsigned long frameCount,
                        const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData) {
    (void)output; (void)timeInfo; (void)statusFlags;
    AudioBuffer *ab = (AudioBuffer*)userData;
    if (!ab->recording) return paComplete;
    if (input) {
        ab_append(ab, (const int16_t*)input, (size_t)frameCount);
    }
    return paContinue;
}

// Background thread: wait for ENTER to stop recording
static void *stdin_waiter(void *userData) {
    AudioBuffer *ab = (AudioBuffer*)userData;
    fprintf(stderr, "\nRecording... press ENTER to stop.\n");
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
    pthread_mutex_lock(&ab->mu);
    ab->recording = false;
    pthread_mutex_unlock(&ab->mu);
    return NULL;
}

// Write a simple PCM S16LE WAV file
static int write_wav(const char *path, const int16_t *samples, size_t frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t byteRate = SAMPLE_RATE * NUM_CHANNELS * (BITS_PER_SAMPLE/8);
    uint16_t blockAlign = NUM_CHANNELS * (BITS_PER_SAMPLE/8);
    uint32_t dataSize = (uint32_t)(frames * NUM_CHANNELS * (BITS_PER_SAMPLE/8));
    uint32_t riffSize = 36 + dataSize;

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtSize = 16; fwrite(&fmtSize, 4, 1, f);
    uint16_t audioFormat = 1; fwrite(&audioFormat, 2, 1, f); // PCM
    uint16_t numChannels = NUM_CHANNELS; fwrite(&numChannels, 2, 1, f);
    uint32_t sampleRate = SAMPLE_RATE; fwrite(&sampleRate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    uint16_t bitsPerSample = BITS_PER_SAMPLE; fwrite(&bitsPerSample, 2, 1, f);
    // data chunk
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);
    fwrite(samples, 1, dataSize, f);
    fclose(f);
    return 0;
}

static void pcm_stats(const int16_t *samples, size_t frames, int32_t *out_max_abs, double *out_mean_abs) {
    size_t total_samples = frames * NUM_CHANNELS;
    int32_t max_abs = 0;
    double sum_abs = 0.0;

    for (size_t i = 0; i < total_samples; ++i) {
        int32_t sample = samples[i];
        int32_t abs_val = (sample < 0) ? -sample : sample;
        if (abs_val > max_abs) max_abs = abs_val;
        sum_abs += (double)abs_val;
    }

    if (out_max_abs) *out_max_abs = max_abs;
    if (out_mean_abs) {
        *out_mean_abs = total_samples ? (sum_abs / (double)total_samples) : 0.0;
    }
}

// libcurl write callback to capture response body in memory
typedef struct {
    char *data;
    size_t size;
} MemBuf;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    MemBuf *mb = (MemBuf*)userdata;
    char *newdata = (char*)realloc(mb->data, mb->size + total + 1);
    if (!newdata) return 0;
    mb->data = newdata;
    memcpy(mb->data + mb->size, ptr, total);
    mb->size += total;
    mb->data[mb->size] = '\0';
    return total;
}

// Upload the WAV to OpenAI transcriptions endpoint; return response body string (caller frees)
static char* transcribe_file(const char *api_key, const char *model, const char *file_path, const char *response_format) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("curl init failed");
        return NULL;
    }

    MemBuf mb = {0};
    struct curl_slist *headers = NULL;

    char authHeader[512];
    snprintf(authHeader, sizeof(authHeader), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, authHeader);

    curl_mime *form = NULL;
    curl_mimepart *field = NULL;

    form = curl_mime_init(curl);

    field = curl_mime_addpart(form);
    curl_mime_name(field, "file");
    curl_mime_filedata(field, file_path);
    curl_mime_type(field, "audio/wav");

    field = curl_mime_addpart(form);
    curl_mime_name(field, "model");
    curl_mime_data(field, model, CURL_ZERO_TERMINATED);

    if (response_format && *response_format) {
        field = curl_mime_addpart(form);
        curl_mime_name(field, "response_format");
        curl_mime_data(field, response_format, CURL_ZERO_TERMINATED);
    }

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/audio/transcriptions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        LOG_ERROR("curl perform failed: %s", curl_easy_strerror(res));
        curl_mime_free(form);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(mb.data);
        return NULL;
    }

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code < 200 || code >= 300) {
        LOG_ERROR("OpenAI API returned HTTP %ld: %s", code, mb.data ? mb.data : "<no body>");
        free(mb.data);
        mb.data = NULL;
    }

    curl_mime_free(form);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return mb.data; // caller frees
}

// ============================================================================
// Public API (PortAudio-enabled)
// ============================================================================

int voice_input_init(void) {
    // Check for API key
    const char *api_key = getenv("OPENAI_API_KEY");
    if (!api_key || !*api_key) {
        LOG_WARN("OPENAI_API_KEY not set - voice input disabled");
        return -1;
    }

    // Initialize PortAudio
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        LOG_ERROR("PortAudio initialization failed: %s", Pa_GetErrorText(err));
        return -1;
    }

    // Check for default input device
    PaDeviceIndex device = Pa_GetDefaultInputDevice();
    if (device == paNoDevice) {
        LOG_ERROR("No default audio input device found");
        Pa_Terminate();
        return -1;
    }

    LOG_INFO("Voice input initialized successfully");
    Pa_Terminate(); // We'll re-init when needed
    return 0;
}

bool voice_input_available(void) {
    const char *api_key = getenv("OPENAI_API_KEY");
    if (!api_key || !*api_key) {
        LOG_WARN("Voice input unavailable: OPENAI_API_KEY not set");
        return false;
    }

    // Try to initialize PortAudio
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        LOG_WARN("Voice input unavailable: PortAudio initialization failed: %s", Pa_GetErrorText(err));
        return false;
    }

    PaDeviceIndex device = Pa_GetDefaultInputDevice();
    Pa_Terminate();

    if (device == paNoDevice) {
        LOG_WARN("Voice input unavailable: No default audio input device found");
        return false;
    }

    return true;
}

int voice_input_record_and_transcribe(char **transcription_out) {
    if (!transcription_out) return -1;
    *transcription_out = NULL;

    const char *api_key = getenv("OPENAI_API_KEY");
    if (!api_key || !*api_key) {
        LOG_ERROR("OPENAI_API_KEY not set");
        return -1;
    }

    const char *model = getenv("OPENAI_TRANSCRIBE_MODEL");
    if (!model || !*model) model = "whisper-1";

    // Initialize PortAudio
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        LOG_ERROR("Pa_Initialize failed: %s", Pa_GetErrorText(err));
        return -1;
    }

    PaStreamParameters inputParams = {0};
    inputParams.device = Pa_GetDefaultInputDevice();
    if (inputParams.device == paNoDevice) {
        LOG_ERROR("No default input device");
        Pa_Terminate();
        return -1;
    }
    inputParams.channelCount = NUM_CHANNELS;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = NULL;

    AudioBuffer ab;
    ab_init(&ab);

    PaStream *stream = NULL;
    err = Pa_OpenStream(&stream, &inputParams, NULL, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, pa_record_cb, &ab);
    if (err != paNoError) {
        LOG_ERROR("Pa_OpenStream failed: %s", Pa_GetErrorText(err));
        Pa_Terminate();
        ab_free(&ab);
        return -1;
    }

    pthread_t waiter;
    pthread_create(&waiter, NULL, stdin_waiter, &ab);

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        LOG_ERROR("Pa_StartStream failed: %s", Pa_GetErrorText(err));
        pthread_mutex_lock(&ab.mu);
        ab.recording = false;
        pthread_mutex_unlock(&ab.mu);
        pthread_join(waiter, NULL);
        Pa_CloseStream(stream);
        Pa_Terminate();
        ab_free(&ab);
        return -1;
    }

    // Wait for recording to finish
    while (1) {
        pthread_mutex_lock(&ab.mu);
        bool rec = ab.recording;
        pthread_mutex_unlock(&ab.mu);
        if (!rec) break;
        Pa_Sleep(50);
    }

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();

    pthread_join(waiter, NULL);

    if (ab.frames == 0) {
        LOG_WARN("No audio recorded");
        ab_free(&ab);
        return -2;
    }

    // Check if audio is silent
    int32_t recording_max_abs = 0;
    double recording_mean_abs = 0.0;
    pcm_stats(ab.data, ab.frames, &recording_max_abs, &recording_mean_abs);
    double recording_sec = (double)ab.frames / SAMPLE_RATE;
    LOG_DEBUG("Recording stats: frames=%zu duration=%.2fs max_amp=%d mean_abs=%.1f",
              ab.frames, recording_sec, recording_max_abs, recording_mean_abs);

    // Avoid direct floating-point equality; treat near-zero mean as silence
    if (recording_max_abs == 0 && recording_mean_abs <= DBL_EPSILON) {
        LOG_WARN("Recording appears completely silent");
        ab_free(&ab);
        return -3;
    }

    // Save WAV file
    const char *wav_path = ".voice_recording.wav";
    if (write_wav(wav_path, ab.data, ab.frames) != 0) {
        LOG_ERROR("Failed to write WAV file");
        ab_free(&ab);
        return -1;
    }
    LOG_DEBUG("Saved %s (%.2fs). Uploading...", wav_path, recording_sec);

    // Transcribe
    const char *response_format = "text";
    char *resp = transcribe_file(api_key, model, wav_path, response_format);
    ab_free(&ab);

    if (!resp) {
        LOG_ERROR("Transcription request failed");
        return -1;
    }

    // Remove trailing newline if present
    size_t len = strlen(resp);
    if (len > 0 && resp[len-1] == '\n') {
        resp[len-1] = '\0';
    }

    *transcription_out = resp;
    LOG_INFO("Transcription successful: %zu chars", strlen(resp));
    return 0;
}

void voice_input_cleanup(void) {
    // Nothing to cleanup - PortAudio is initialized/terminated per-use
}

void voice_input_print_status(void) {
    const char *api_key = getenv("OPENAI_API_KEY");
    bool has_api_key = (api_key && *api_key);
    
    // Try to initialize PortAudio
    PaError err = Pa_Initialize();
    bool has_portaudio = (err == paNoError);
    
    if (has_portaudio) {
        PaDeviceIndex device = Pa_GetDefaultInputDevice();
        Pa_Terminate();
        
        if (device == paNoDevice) {
            LOG_WARN("Voice input: No microphone detected");
            fprintf(stderr, "⚠ Voice input unavailable: No microphone detected\n");
            fprintf(stderr, "  Connect a microphone to use the /voice command\n");
            return;
        }
    } else {
        LOG_WARN("Voice input: PortAudio not available - %s", Pa_GetErrorText(err));
        fprintf(stderr, "⚠ Voice input unavailable: PortAudio not installed\n");
        fprintf(stderr, "  Install with: brew install portaudio (macOS)\n");
        fprintf(stderr, "            or: sudo apt-get install portaudio19-dev (Ubuntu)\n");
    }
    
    if (!has_api_key) {
        LOG_WARN("Voice input: OPENAI_API_KEY not set");
        fprintf(stderr, "⚠ Voice input unavailable: OPENAI_API_KEY not set\n");
        fprintf(stderr, "  Set with: export OPENAI_API_KEY=\"your-key\"\n");
        return;
    }
    
    // If we get here, everything is available
    if (has_portaudio && has_api_key) {
        LOG_INFO("Voice input available - use /voice command");
    }
}

#else  // !HAVE_PORTAUDIO

// ============================================================================
// Public API (stubs when PortAudio is not available)
// ============================================================================

int voice_input_init(void) {
    LOG_WARN("Voice input disabled: PortAudio not available at build time");
    return -1;
}

bool voice_input_available(void) {
    return false;
}

int voice_input_record_and_transcribe(char **transcription_out) {
    (void)transcription_out;
    LOG_ERROR("Voice input not built: missing PortAudio dependency");
    return -1;
}

void voice_input_cleanup(void) {
    // no-op
}

void voice_input_print_status(void) {
    fprintf(stderr, "⚠ Voice input unavailable: PortAudio not detected at build time\n");
    fprintf(stderr, "  Enable with: make VOICE=1 (requires PortAudio dev headers)\n");
    fprintf(stderr, "  Or install and rebuild: brew install portaudio (macOS)\n");
    fprintf(stderr, "                        sudo apt-get install portaudio19-dev (Ubuntu)\n");
}

#endif  // HAVE_PORTAUDIO
