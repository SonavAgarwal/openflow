#pragma once

#include <SDL.h>
#include <SDL_audio.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

//
// SDL Audio capture
//

class audio_async {
public:
    audio_async(int len_ms);
    ~audio_async();

    bool init(int capture_id, int sample_rate);

    // Start capturing audio via the SDL callback.
    // Keeps last len_ms milliseconds of audio in a circular buffer.
    bool resume();
    bool pause();
    bool clear();

    // Callback to be called by SDL
    void callback(uint8_t *stream, int len);

    // Get audio data from the circular buffer.
    // If ms <= 0, returns up to len_ms of the most recent audio.
    // Also returns the current timeline position (in ms) since the most recent resume().
    void get(int ms, std::vector<float> &audio, int64_t &current_time_ms);

    // Back-compat overload: returns audio only; drops the time value.
    inline void get(int ms, std::vector<float> &audio) {
        int64_t _unused = 0;
        get(ms, audio, _unused);
    }

private:
    SDL_AudioDeviceID m_dev_id_in = 0;

    int m_len_ms = 0;
    int m_sample_rate = 0;

    std::atomic_bool m_running;
    std::mutex m_mutex;

    std::vector<float> m_audio; // circular buffer
    size_t m_audio_pos = 0;     // next write index
    size_t m_audio_len = 0;     // number of valid samples in buffer (<= m_audio.size())

    // Total samples captured since the most recent resume().
    // Guarded by m_mutex whenever modified or read together with buffer state.
    uint64_t m_total_samples = 0;
};

// Return false if need to quit
bool sdl_poll_events();
