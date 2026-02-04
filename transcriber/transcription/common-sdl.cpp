#include "common-sdl.h"

#include <cstdio>
#include <cstring> // memcpy

audio_async::audio_async(int len_ms) {
    m_len_ms = len_ms;
    m_running = false;
}

audio_async::~audio_async() {
    if (m_dev_id_in) {
        SDL_CloseAudioDevice(m_dev_id_in);
    }
}

bool audio_async::init(int capture_id, int sample_rate) {
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetHintWithPriority(SDL_HINT_AUDIO_RESAMPLING_MODE, "medium", SDL_HINT_OVERRIDE);

    {
        int nDevices = SDL_GetNumAudioDevices(SDL_TRUE);
        fprintf(stderr, "%s: found %d capture devices:\n", __func__, nDevices);
        for (int i = 0; i < nDevices; i++) {
            fprintf(stderr, "%s:    - Capture device #%d: '%s'\n", __func__, i, SDL_GetAudioDeviceName(i, SDL_TRUE));
        }
    }

    SDL_AudioSpec capture_spec_requested;
    SDL_AudioSpec capture_spec_obtained;

    SDL_zero(capture_spec_requested);
    SDL_zero(capture_spec_obtained);

    capture_spec_requested.freq = sample_rate;
    capture_spec_requested.format = AUDIO_F32;
    capture_spec_requested.channels = 1;
    capture_spec_requested.samples = 1024;
    capture_spec_requested.callback = [](void *userdata, uint8_t *stream, int len) {
        audio_async *audio = (audio_async *)userdata;
        audio->callback(stream, len);
    };
    capture_spec_requested.userdata = this;

    if (capture_id >= 0) {
        fprintf(stderr, "%s: attempt to open capture device %d : '%s' ...\n", __func__, capture_id, SDL_GetAudioDeviceName(capture_id, SDL_TRUE));
        m_dev_id_in = SDL_OpenAudioDevice(SDL_GetAudioDeviceName(capture_id, SDL_TRUE), SDL_TRUE, &capture_spec_requested, &capture_spec_obtained, 0);
    } else {
        fprintf(stderr, "%s: attempt to open default capture device ...\n", __func__);
        m_dev_id_in = SDL_OpenAudioDevice(nullptr, SDL_TRUE, &capture_spec_requested, &capture_spec_obtained, 0);
    }

    if (!m_dev_id_in) {
        fprintf(stderr, "%s: couldn't open an audio device for capture: %s!\n", __func__, SDL_GetError());
        m_dev_id_in = 0;
        return false;
    } else {
        fprintf(stderr, "%s: obtained spec for input device (SDL Id = %d):\n", __func__, m_dev_id_in);
        fprintf(stderr, "%s:     - sample rate:       %d\n", __func__, capture_spec_obtained.freq);
        fprintf(stderr, "%s:     - format:            %d (required: %d)\n", __func__, capture_spec_obtained.format, capture_spec_requested.format);
        fprintf(stderr, "%s:     - channels:          %d (required: %d)\n", __func__, capture_spec_obtained.channels, capture_spec_requested.channels);
        fprintf(stderr, "%s:     - samples per frame: %d\n", __func__, capture_spec_obtained.samples);
    }

    m_sample_rate = capture_spec_obtained.freq;

    m_audio.resize((m_sample_rate * m_len_ms) / 1000);

    // Initialize timeline
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_total_samples = 0;
        m_audio_pos = 0;
        m_audio_len = 0;
    }

    return true;
}

bool audio_async::resume() {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to resume!\n", __func__);
        return false;
    }

    if (m_running) {
        fprintf(stderr, "%s: already running!\n", __func__);
        return false;
    }

    // Reset timeline at the start of a new capture session
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_total_samples = 0;
        m_audio_pos = 0;
        m_audio_len = 0;
    }

    SDL_PauseAudioDevice(m_dev_id_in, 0);
    m_running = true;
    return true;
}

bool audio_async::pause() {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to pause!\n", __func__);
        return false;
    }

    if (!m_running) {
        fprintf(stderr, "%s: already paused!\n", __func__);
        return false;
    }

    SDL_PauseAudioDevice(m_dev_id_in, 1);
    m_running = false;
    return true;
}

bool audio_async::clear() {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to clear!\n", __func__);
        return false;
    }

    if (!m_running) {
        fprintf(stderr, "%s: not running!\n", __func__);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_audio_pos = 0;
        m_audio_len = 0;
        // Note: we intentionally do NOT reset m_total_samples here so the timeline keeps advancing.
    }

    return true;
}

// callback to be called by SDL
void audio_async::callback(uint8_t *stream, int len) {
    if (!m_running) {
        return;
    }

    // How many samples arrived in this callback:
    size_t samples_in = static_cast<size_t>(len) / sizeof(float);

    // We'll write at most the ring buffer size worth of newest samples:
    size_t n_samples = samples_in;
    if (n_samples > m_audio.size()) {
        // Keep only the latest portion; drop the oldest part of this callback
        stream += (len - (m_audio.size() * sizeof(float)));
        n_samples = m_audio.size();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Copy into ring buffer
        if (m_audio_pos + n_samples > m_audio.size()) {
            const size_t n0 = m_audio.size() - m_audio_pos;
            memcpy(&m_audio[m_audio_pos], stream, n0 * sizeof(float));
            memcpy(&m_audio[0], stream + n0 * sizeof(float), (n_samples - n0) * sizeof(float));
        } else {
            memcpy(&m_audio[m_audio_pos], stream, n_samples * sizeof(float));
        }

        m_audio_pos = (m_audio_pos + n_samples) % m_audio.size();
        m_audio_len = std::min(m_audio_len + n_samples, m_audio.size());

        // Advance the timeline by ALL samples that actually arrived (even if buffer truncated)
        m_total_samples += samples_in;
    }
}

void audio_async::get(int ms, std::vector<float> &result, int64_t &current_time_ms) {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to get audio from!\n", __func__);
        return;
    }

    if (!m_running) {
        fprintf(stderr, "%s: not running!\n", __func__);
        return;
    }

    result.clear();

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (ms <= 0) {
            ms = m_len_ms;
        }

        size_t n_samples = (static_cast<size_t>(m_sample_rate) * static_cast<size_t>(ms)) / 1000;
        if (n_samples > m_audio_len) {
            n_samples = m_audio_len;
        }

        result.resize(n_samples);

        int s0 = static_cast<int>(m_audio_pos) - static_cast<int>(n_samples);
        if (s0 < 0) {
            s0 += static_cast<int>(m_audio.size());
        }

        if (static_cast<size_t>(s0) + n_samples > m_audio.size()) {
            const size_t n0 = m_audio.size() - static_cast<size_t>(s0);
            memcpy(result.data(), &m_audio[static_cast<size_t>(s0)], n0 * sizeof(float));
            memcpy(&result[n0], &m_audio[0], (n_samples - n0) * sizeof(float));
        } else {
            memcpy(result.data(), &m_audio[static_cast<size_t>(s0)], n_samples * sizeof(float));
        }

        // Compute timeline in ms from total samples captured since resume()
        if (m_sample_rate > 0) {
            current_time_ms = static_cast<int64_t>((m_total_samples * 1000ULL) / static_cast<uint64_t>(m_sample_rate));
        } else {
            current_time_ms = 0;
        }
    }
}

bool sdl_poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT: {
            return false;
        }
        default:
            break;
        }
    }
    return true;
}
