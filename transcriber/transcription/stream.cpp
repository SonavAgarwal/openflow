// transcriber_simple.cpp
#include "common-sdl.h"
#include "whisper.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct streaming_params {
    int32_t n_threads = std::max(1u, std::thread::hardware_concurrency());
    int32_t step_ms = 150;
    int32_t length_ms = 3000;
    int32_t capture_id = -1;
    int32_t min_decode_ms = 200;
    std::string language = "en";
    std::string model = "models/ggml-base.en.bin";
    bool use_gpu = true;
    bool debug = false;
    std::string vad_model_path;
};

static void print_usage(char **argv, const streaming_params &p) {
    fprintf(stderr, "\nusage: %s [options]\n", argv[0]);
    fprintf(stderr, "  -h, --help            show this help\n");
    fprintf(stderr, "  --model F             model path [%s]\n", p.model.c_str());
    fprintf(stderr, "  --step N              step size in ms [%d]\n", p.step_ms);
    fprintf(stderr, "  --length N            window length in ms [%d]\n", p.length_ms);
    fprintf(stderr, "  --min-decode N        minimum audio ms before decode [%d]\n", p.min_decode_ms);
    fprintf(stderr, "  --lang XX             language code (en, auto, ...) [%s]\n", p.language.c_str());
    fprintf(stderr, "  --threads N           decoder threads [%d]\n", p.n_threads);
    fprintf(stderr, "  -d,  --debug          debug prints [%d]\n", p.debug);
    fprintf(stderr, "  --silero-vad PATH     Silero VAD ggml model (enables speech probability output)\n");
    fprintf(stderr, "\nOutputs NDJSON with raw tokens + absolute timestamps every step.\n");
}

static bool parse_args(int argc, char **argv, streaming_params &p) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *flag) { if (i+1 >= argc) { fprintf(stderr,"missing arg for %s\n",flag); exit(2);} return argv[++i]; };
        if (a == "-h" || a == "--help") {
            print_usage(argv, p);
            exit(0);
        } else if (a == "--model") {
            p.model = need(a.c_str());
        } else if (a == "--step") {
            p.step_ms = std::max(1, atoi(need(a.c_str())));
        } else if (a == "--length") {
            p.length_ms = std::max(100, atoi(need(a.c_str())));
        } else if (a == "--min-decode") {
            p.min_decode_ms = std::max(1, atoi(need(a.c_str())));
        } else if (a == "--lang") {
            p.language = need(a.c_str());
        } else if (a == "--threads") {
            p.n_threads = std::max(1, atoi(need(a.c_str())));
        } else if (a == "-d" || a == "--debug") {
            p.debug = true;
        } else if (a == "--silero-vad") {
            p.vad_model_path = need(a.c_str());
        } else {
            fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
            return false;
        }
    }
    return true;
}

static inline bool is_control_piece(const std::string &s) {
    if (s.size() >= 2 && s[0] == '<' && s[1] == '|') return true; // <|...|>
    if (s.size() >= 3 && s[0] == '[' && s[1] == '_') return true; // [_...]
    return false;
}

struct VadContextDeleter {
    void operator()(whisper_vad_context *ctx) const {
        if (ctx) {
            whisper_vad_free(ctx);
        }
    }
};

class SileroVadRunner {
public:
    SileroVadRunner(const std::string &model_path, int sample_rate, bool use_gpu, int n_threads)
        : sample_rate_(sample_rate),
          context_(nullptr, VadContextDeleter{}) {
        if (sample_rate_ != WHISPER_SAMPLE_RATE) {
            throw std::runtime_error("Silero VAD expects 16 kHz audio");
        }

        struct whisper_vad_context_params ctx_params = whisper_vad_default_context_params();
        ctx_params.n_threads = std::max(1, n_threads);
        (void)use_gpu;
        ctx_params.use_gpu = false;

        context_.reset(whisper_vad_init_from_file_with_params(model_path.c_str(), ctx_params));
        if (!context_) {
            throw std::runtime_error("Failed to initialize Silero VAD context");
        }

        chunk_size_ = expected_chunk_size();
        std::vector<float> probe(chunk_size_, 0.0f);
        if (!whisper_vad_detect_speech(context_.get(), probe.data(), static_cast<int>(probe.size()))) {
            throw std::runtime_error("Failed to probe Silero VAD probability window");
        }
        if (whisper_vad_n_probs(context_.get()) != 1) {
            throw std::runtime_error("Silero VAD returned unexpected probability count during probe");
        }
    }

    size_t chunk_size() const { return chunk_size_; }

    float infer(const float *samples, size_t n_samples) {
        if (!samples) {
            throw std::runtime_error("Silero VAD received null audio chunk");
        }
        if (n_samples == 0) {
            throw std::runtime_error("Silero VAD received empty audio chunk");
        }

        if (!whisper_vad_detect_speech(context_.get(), samples, static_cast<int>(n_samples))) {
            throw std::runtime_error("Silero VAD failed to process audio chunk");
        }

        int n_probs = whisper_vad_n_probs(context_.get());
        if (n_probs <= 0) {
            throw std::runtime_error("Silero VAD returned no probabilities");
        }

        float *probs = whisper_vad_probs(context_.get());
        if (!probs) {
            throw std::runtime_error("Silero VAD returned invalid probabilities");
        }

        return probs[n_probs - 1];
    }

    float infer(const float *samples) {
        return infer(samples, chunk_size_);
    }

private:
    size_t expected_chunk_size() const {
        // Silero 16k models operate on 512-sample frames (32 ms at 16 kHz).
        return 512;
    }

    int sample_rate_;
    size_t chunk_size_ = 0;
    std::unique_ptr<whisper_vad_context, VadContextDeleter> context_;
};

int main(int argc, char **argv) {
    ggml_backend_load_all();

    streaming_params params;
    if (!parse_args(argc, argv, params)) return 1;

    audio_async audio(params.length_ms);
    if (!audio.init(params.capture_id, WHISPER_SAMPLE_RATE)) {
        fprintf(stderr, "audio.init() failed\n");
        return 1;
    }
    audio.resume();

    std::unique_ptr<SileroVadRunner> vad;
    size_t vad_chunk_samples = 0;
    std::filesystem::path vad_model_path;
    bool want_vad = false;
    if (!params.vad_model_path.empty()) {
        vad_model_path = params.vad_model_path;
        if (!std::filesystem::exists(vad_model_path)) {
            fprintf(stderr, "error: Silero VAD model not found at '%s'\n", params.vad_model_path.c_str());
            return 1;
        }
        want_vad = true;
    }

    if (params.language != "auto" && whisper_lang_id(params.language.c_str()) == -1) {
        fprintf(stderr, "error: unknown language '%s'\n", params.language.c_str());
        return 1;
    }

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = params.use_gpu;
    cparams.dtw_token_timestamps = true;
    cparams.dtw_aheads_preset = WHISPER_AHEADS_BASE_EN;

    whisper_context *ctx = whisper_init_from_file_with_params(params.model.c_str(), cparams);
    if (!ctx) {
        fprintf(stderr, "failed to initialize whisper context\n");
        return 2;
    }

    // Signal readiness once whisper has been initialized.
    printf("{\"event\":\"ready\"}\n");
    fflush(stdout);

    if (want_vad) {
        try {
            vad = std::make_unique<SileroVadRunner>(vad_model_path.string(),
                                                    WHISPER_SAMPLE_RATE,
                                                    params.use_gpu,
                                                    params.n_threads);
            vad_chunk_samples = vad->chunk_size();
            fprintf(stderr, "Silero VAD initialized (chunk=%zu samples)\n", vad_chunk_samples);
        } catch (const std::exception &ex) {
            fprintf(stderr, "error: failed to initialize Silero VAD: %s\n", ex.what());
            return 1;
        }
    }

    int64_t last_decode_audio_ms = 0;

    while (sdl_poll_events()) {
        std::vector<float> window_pcm;
        int64_t audio_time_ms = 0;
        audio.get(params.length_ms, window_pcm, audio_time_ms);

        if ((audio_time_ms - last_decode_audio_ms) < params.step_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        last_decode_audio_ms = audio_time_ms;

        const int need_samples = (int)((int64_t)params.min_decode_ms * WHISPER_SAMPLE_RATE / 1000);
        if ((int)window_pcm.size() < need_samples) continue;

        bool has_vad_prob = false;
        float vad_prob = 0.0f;
        if (vad && window_pcm.size() >= vad_chunk_samples) {
            const float *chunk_ptr = window_pcm.data() + window_pcm.size() - vad_chunk_samples;
            vad_prob = vad->infer(chunk_ptr, vad_chunk_samples);
            has_vad_prob = true;
        }

        // Compute absolute start of this window (ms since start of capture)
        const int64_t window_pcm_ms = (int64_t)window_pcm.size() * 1000LL / WHISPER_SAMPLE_RATE;
        const int64_t window_start_ms = std::max<int64_t>(0, audio_time_ms - window_pcm_ms);

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_progress = false;
        wparams.print_special = false;
        wparams.print_realtime = false;
        wparams.print_timestamps = false;
        wparams.no_context = true;
        wparams.single_segment = true;
        wparams.max_tokens = 120;
        wparams.language = params.language.c_str();
        wparams.n_threads = params.n_threads;
        wparams.token_timestamps = true;
        wparams.thold_pt = 0.01f;
        wparams.entropy_thold = 2.40f;
        wparams.logprob_thold = -1.0f;
        wparams.no_speech_thold = 0.0f;

        if (whisper_full(ctx, wparams, window_pcm.data(), window_pcm.size()) != 0) {
            fprintf(stderr, "whisper_full failed\n");
            break;
        }

        // Collect raw tokens (with leading-space flag) and ABSOLUTE token times
        struct Piece {
            std::string text;
            int64_t t0_ms;
            int64_t t1_ms;
            bool leading_space;
        };
        std::vector<Piece> pieces;

        const int n_segments = whisper_full_n_segments(ctx);
        for (int s = 0; s < n_segments; ++s) {
            const int64_t seg_base_ms = (int64_t)whisper_full_get_segment_t0(ctx, s) * 10; // 10ms units → ms
            const int n_tok = whisper_full_n_tokens(ctx, s);
            for (int i = 0; i < n_tok; ++i) {
                auto td = whisper_full_get_token_data(ctx, s, i);
                const char *pc = whisper_token_to_str(ctx, td.id);
                if (!pc) continue;
                std::string piece = pc;
                if (is_control_piece(piece)) continue;

                // determine leading-space
                bool leading_space = (!piece.empty() && std::isspace((unsigned char)piece[0]));

                // absolute token times
                int64_t t0 = (td.t0 >= 0) ? ((int64_t)td.t0 * 10) : -1;
                int64_t t1 = (td.t1 >= 0) ? ((int64_t)td.t1 * 10) : -1;
                t0 = (t0 < 0) ? -1 : (t0 + seg_base_ms + window_start_ms);
                t1 = (t1 < 0) ? -1 : (t1 + seg_base_ms + window_start_ms);

                pieces.push_back({piece, t0, t1, leading_space});
            }
        }

        // emit NDJSON
        auto esc = [](const std::string &s) {
            std::string o;
            o.reserve(s.size() + 8);
            for (char c : s) {
                switch (c) {
                case '\\':
                    o += "\\\\";
                    break;
                case '\"':
                    o += "\\\"";
                    break;
                case '\n':
                    o += "\\n";
                    break;
                case '\r':
                    o += "\\r";
                    break;
                case '\t':
                    o += "\\t";
                    break;
                default:
                    o.push_back(c);
                    break;
                }
            }
            return o;
        };

        // Downsample the PCM window to a compact waveform envelope for visualization.
        static const int WAVEFORM_BINS = 120;
        std::vector<float> waveform;
        waveform.reserve(WAVEFORM_BINS);
        const size_t total_samples = window_pcm.size();
        const size_t samples_per_bin = std::max<size_t>(1, total_samples / WAVEFORM_BINS);

        float max_abs_sample = 0.0f;
        for (float sample : window_pcm) {
            max_abs_sample = std::max(max_abs_sample, std::fabs(sample));
        }

        for (int b = 0; b < WAVEFORM_BINS; ++b) {
            const size_t start = static_cast<size_t>(b) * samples_per_bin;
            if (start >= total_samples) break;
            const size_t end = std::min(total_samples, start + samples_per_bin);
            float peak = 0.0f;
            for (size_t i = start; i < end; ++i) {
                peak = std::max(peak, std::fabs(window_pcm[i]));
            }
            waveform.push_back(peak);
        }

        printf("{\"event\":\"data\",\"audio_time_ms\":%lld,\"window_start_ms\":%lld,\"step_ms\":%d,\"length_ms\":%d,\"waveform_stride\":%zu,\"waveform_max\":%.6f",
               (long long)audio_time_ms, (long long)window_start_ms, params.step_ms, params.length_ms,
               samples_per_bin, max_abs_sample);
        if (has_vad_prob) {
            printf(",\"vad_prob\":%.6f,\"vad_chunk_samples\":%zu,\"vad_sample_rate\":%d",
                   vad_prob, vad_chunk_samples, WHISPER_SAMPLE_RATE);
        }
        printf(",\"waveform\":[");

        for (size_t i = 0; i < waveform.size(); ++i) {
            if (i) printf(",");
            printf("%.6f", waveform[i]);
        }
        printf("],\"tokens\":[");

        for (size_t i = 0; i < pieces.size(); ++i) {
            if (i) printf(",");
            const auto &p = pieces[i];
            printf("{\"text\":\"%s\",\"t0_ms\":%lld,\"t1_ms\":%lld,\"leading_space\":%s}",
                   esc(p.text).c_str(),
                   (long long)p.t0_ms, (long long)p.t1_ms,
                   p.leading_space ? "true" : "false");
        }
        printf("]}\n");
        fflush(stdout);
    }

    audio.pause();
    whisper_free(ctx);
    return 0;
}
