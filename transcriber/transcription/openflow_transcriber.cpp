#include "common-sdl.h"
#include "whisper.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <deque>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace {

struct vad_params {
    int32_t n_threads = std::min(2u, std::max(1u, std::thread::hardware_concurrency()));
    int32_t capture_id = -1;
    std::string language = "en";
    std::string model = "models/ggml-base.en.bin";
    std::string vad_model_path;
    std::string audio_file;
    std::string dictionary_path;
    int32_t dictionary_poll_ms = 1000;
    bool send_prompt = true;
    bool bias_decoding = false;
    float bias_first_logit = 0.35f;
    float bias_continuation_logit = 0.85f;
    int32_t beam_size = 0; // 0 = whisper default (beam search default is 5)
    int logits_top_k = 50;
    float logits_prob_threshold = 20.0f; // compute softmax denom only for logits > (max - threshold); <= 0 computes full denom
    bool logits_prefix_text = false;
    int logits_flush_ms = 250;
    int logits_boosted_k = 24;
    std::string logits_log_path;
    bool log = false; // emit verbose dictionary/logits packets (stdout + file)
    bool emit_vad_events = true;
    bool use_gpu_whisper = true;
    bool debug = false;
    bool stdin_audio = false;
    bool stdin_pcm = false;

    int32_t step_ms = 200;
    float start_threshold = 0.60f;
    float stop_threshold = 0.35f;
    int32_t min_segment_ms = 250;
    int32_t max_segment_ms = 12000;
    int32_t min_silence_ms = 150;
    int32_t pre_padding_ms = 200;
    int32_t post_padding_ms = 350;
    int32_t ring_buffer_ms = 20000;
};

void print_usage(char **argv, const vad_params &p) {
    fprintf(stderr, "\nusage: %s [options]\n", argv[0]);
    fprintf(stderr, "  -h, --help                 show this help\n");
    fprintf(stderr, "  --model F                  whisper model path [%s]\n", p.model.c_str());
    fprintf(stderr, "  --lang XX                  language code [%s]\n", p.language.c_str());
    fprintf(stderr, "  --threads N                decoder threads [%d]\n", p.n_threads);
    fprintf(stderr, "  --capture-id N             SDL capture device id [%d]\n", p.capture_id);
    fprintf(stderr, "  --audio-file PATH          run offline on WAV (mono/pcm16) instead of mic capture\n");
    fprintf(stderr, "  --step N                   partial decode cadence in ms while active; -1 disables [%d]\n", p.step_ms);
    fprintf(stderr, "  --start-threshold F        VAD speech start threshold [%0.2f]\n", p.start_threshold);
    fprintf(stderr, "  --stop-threshold F         VAD speech stop threshold [%0.2f]\n", p.stop_threshold);
    fprintf(stderr, "  --min-segment-ms N         minimum segment length before emit [%d]\n", p.min_segment_ms);
    fprintf(stderr, "  --max-segment-ms N         maximum segment length before forced emit [%d]\n", p.max_segment_ms);
    fprintf(stderr, "  --min-silence-ms N         silence required before considering segment end [%d]\n", p.min_silence_ms);
    fprintf(stderr, "  --pre-padding-ms N         audio padding before speech start [%d]\n", p.pre_padding_ms);
    fprintf(stderr, "  --post-padding-ms N        audio padding after speech end [%d]\n", p.post_padding_ms);
    fprintf(stderr, "  --ring-buffer-ms N         captured ring buffer size [%d]\n", p.ring_buffer_ms);
    fprintf(stderr, "  --silero-vad PATH          Silero VAD ggml model (required)\n");
    fprintf(stderr, "  --dictionary-file PATH     dictionary file (words/phrases) used for prompt + biasing\n");
    fprintf(stderr, "  --dictionary-poll-ms N     minimum ms between dictionary file reloads [%d]\n", p.dictionary_poll_ms);
    fprintf(stderr, "  --send-prompt              pass dictionary file contents as whisper initial prompt (default)\n");
    fprintf(stderr, "  --no-send-prompt           do not pass a whisper initial prompt (dictionary still loaded)\n");
    fprintf(stderr, "  --bias-decoding            bias decoding towards dictionary tokens via logits filter callback\n");
    fprintf(stderr, "  --no-bias-decoding         disable decoding bias (default)\n");
    fprintf(stderr, "  --bias-first-logit F       add to logits for dictionary first tokens [%0.2f]\n", p.bias_first_logit);
    fprintf(stderr, "  --bias-continuation-logit F add to logits for dictionary continuation tokens [%0.2f]\n", p.bias_continuation_logit);
    fprintf(stderr, "  --beam-size N              beam size for beam search (>=2; capped at 8; 0 uses whisper default) [%d]\n", p.beam_size);
    fprintf(stderr, "  --logits-top-k N           number of tokens to emit per logits packet [%d]\n", p.logits_top_k);
    fprintf(stderr, "  --logits-prob-threshold F  softmax denom over logits > (max-F); <=0 for full denom [%0.1f]\n", p.logits_prob_threshold);
    fprintf(stderr, "  --logits-prefix-text       include prefix_text in logits packets (slower)\n");
    fprintf(stderr, "  --logits-flush-ms N        min ms between flushing logits jsonl to disk [%d]\n", p.logits_flush_ms);
    fprintf(stderr, "  --logits-boosted-k N       max boosted tokens to include per logits packet [%d]\n", p.logits_boosted_k);
    fprintf(stderr, "  --logits-log-path PATH     where to append logits jsonl [./.voice/whisper_logits.jsonl]\n");
    fprintf(stderr, "  --log                      enable verbose dictionary/logits logging (stdout + file)\n");
    fprintf(stderr, "  --no-log                   disable verbose logging (default)\n");
    fprintf(stderr, "  --no-vad-events            do not emit per-chunk VAD probability packets\n");
    fprintf(stderr, "  --cpu-only                 disable GPU backends for whisper + VAD\n");
    fprintf(stderr, "  --stdin-audio              read WAV file paths from stdin (one per line) and keep model warm\n");
    fprintf(stderr, "  --stdin-pcm                read float32 PCM from stdin (framed) and keep model warm\n");
    fprintf(stderr, "  -d, --debug                enable debug logging\n");
}

bool parse_args(int argc, char **argv, vad_params &p) {
    auto need = [&](const char *flag, int &i) -> const char * {
        if (i + 1 >= argc) {
            fprintf(stderr, "missing arg for %s\n", flag);
            exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            print_usage(argv, p);
            exit(0);
        } else if (a == "--model") {
            p.model = need(a.c_str(), i);
        } else if (a == "--lang") {
            p.language = need(a.c_str(), i);
        } else if (a == "--threads") {
            p.n_threads = std::max(1, atoi(need(a.c_str(), i)));
        } else if (a == "--capture-id") {
            p.capture_id = atoi(need(a.c_str(), i));
        } else if (a == "--audio-file" || a == "--audio_file") {
            p.audio_file = need(a.c_str(), i);
        } else if (a == "--step") {
            const int v = atoi(need(a.c_str(), i));
            p.step_ms = (v < 0) ? -1 : std::max(10, v);
        } else if (a == "--silero-vad") {
            p.vad_model_path = need(a.c_str(), i);
        } else if (a == "--dictionary-file" || a == "--dictionary_file" || a == "--prompt-file") {
            if (a == "--prompt-file") {
                fprintf(stderr, "warning: --prompt-file is deprecated; use --dictionary-file\n");
            }
            p.dictionary_path = need(a.c_str(), i);
        } else if (a == "--dictionary-poll-ms" || a == "--dictionary_poll_ms" || a == "--prompt-poll-ms") {
            if (a == "--prompt-poll-ms") {
                fprintf(stderr, "warning: --prompt-poll-ms is deprecated; use --dictionary-poll-ms\n");
            }
            p.dictionary_poll_ms = std::max(10, atoi(need(a.c_str(), i)));
        } else if (a == "--send-prompt" || a == "--send_prompt") {
            p.send_prompt = true;
        } else if (a == "--no-send-prompt" || a == "--no_send_prompt") {
            p.send_prompt = false;
        } else if (a == "--bias-decoding" || a == "--bias_decoding") {
            p.bias_decoding = true;
        } else if (a == "--no-bias-decoding" || a == "--no_bias_decoding") {
            p.bias_decoding = false;
        } else if (a == "--bias-first-logit" || a == "--bias_first_logit") {
            p.bias_first_logit = static_cast<float>(atof(need(a.c_str(), i)));
        } else if (a == "--bias-continuation-logit" || a == "--bias_continuation_logit") {
            p.bias_continuation_logit = static_cast<float>(atof(need(a.c_str(), i)));
        } else if (a == "--beam-size" || a == "--beam_size") {
            p.beam_size = std::max<int32_t>(0, atoi(need(a.c_str(), i)));
        } else if (a == "--logits-top-k" || a == "--logits_top_k") {
            p.logits_top_k = std::max(1, atoi(need(a.c_str(), i)));
        } else if (a == "--logits-prob-threshold" || a == "--logits_prob_threshold") {
            p.logits_prob_threshold = static_cast<float>(atof(need(a.c_str(), i)));
        } else if (a == "--logits-prefix-text" || a == "--logits_prefix_text") {
            p.logits_prefix_text = true;
        } else if (a == "--logits-flush-ms" || a == "--logits_flush_ms") {
            p.logits_flush_ms = std::max(0, atoi(need(a.c_str(), i)));
        } else if (a == "--logits-boosted-k" || a == "--logits_boosted_k") {
            p.logits_boosted_k = std::max(0, atoi(need(a.c_str(), i)));
        } else if (a == "--logits-log-path" || a == "--logits_log_path") {
            p.logits_log_path = need(a.c_str(), i);
        } else if (a == "--log") {
            p.log = true;
        } else if (a == "--no-log" || a == "--no_log") {
            p.log = false;
        } else if (a == "--no-vad-events" || a == "--no_vad_events") {
            p.emit_vad_events = false;
        } else if (a == "--stdin-audio") {
            p.stdin_audio = true;
        } else if (a == "--stdin-pcm") {
            p.stdin_pcm = true;
        } else if (a == "--start-threshold") {
            p.start_threshold = std::clamp(static_cast<float>(atof(need(a.c_str(), i))), 0.0f, 1.0f);
        } else if (a == "--stop-threshold") {
            p.stop_threshold = std::clamp(static_cast<float>(atof(need(a.c_str(), i))), 0.0f, 1.0f);
        } else if (a == "--min-segment-ms") {
            p.min_segment_ms = std::max(0, atoi(need(a.c_str(), i)));
        } else if (a == "--max-segment-ms") {
            p.max_segment_ms = std::max(1000, atoi(need(a.c_str(), i)));
        } else if (a == "--min-silence-ms") {
            p.min_silence_ms = std::max(0, atoi(need(a.c_str(), i)));
        } else if (a == "--pre-padding-ms") {
            p.pre_padding_ms = std::max(0, atoi(need(a.c_str(), i)));
        } else if (a == "--post-padding-ms") {
            p.post_padding_ms = std::max(0, atoi(need(a.c_str(), i)));
        } else if (a == "--ring-buffer-ms") {
            p.ring_buffer_ms = std::max(2000, atoi(need(a.c_str(), i)));
        } else if (a == "--cpu-only") {
            p.use_gpu_whisper = false;
        } else if (a == "-d" || a == "--debug") {
            p.debug = true;
        } else {
            fprintf(stderr, "unknown argument '%s'\n", a.c_str());
            return false;
        }
    }
    return true;
}

inline bool is_control_piece(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i + 1 < s.size() && s[i] == '<' && s[i + 1] == '|') return true;
    if (i + 1 < s.size() && s[i] == '[' && s[i + 1] == '_') return true;
    return false;
}

std::vector<std::string> split_dictionary_entries(const std::string &raw) {
    std::vector<std::string> out;
    out.reserve(256);

    std::string cur;
    cur.reserve(64);

    auto flush = [&]() {
        if (cur.empty()) return;
        // trim
        size_t b = 0;
        while (b < cur.size() && std::isspace(static_cast<unsigned char>(cur[b]))) ++b;
        size_t e = cur.size();
        while (e > b && std::isspace(static_cast<unsigned char>(cur[e - 1]))) --e;
        if (e > b) out.push_back(cur.substr(b, e - b));
        cur.clear();
    };

    for (char c : raw) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();

    std::unordered_set<std::string> seen;
    seen.reserve(out.size() * 2 + 8);
    std::vector<std::string> uniq;
    uniq.reserve(out.size());
    for (auto &s : out) {
        if (s.empty()) continue;
        if (seen.insert(s).second) {
            uniq.push_back(std::move(s));
        }
    }
    return uniq;
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static bool read_wav_mono_f32(const std::string &path, std::vector<float> &out, int &sample_rate_out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        fprintf(stderr, "error: failed to open audio file '%s'\n", path.c_str());
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) {
        fprintf(stderr, "error: audio file '%s' is empty\n", path.c_str());
        return false;
    }
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> buf((size_t) size);
    f.read(reinterpret_cast<char *>(buf.data()), size);
    if (!f.good()) {
        fprintf(stderr, "error: failed to read audio file '%s'\n", path.c_str());
        return false;
    }

    if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "error: '%s' is not a RIFF/WAVE file\n", path.c_str());
        return false;
    }

    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    size_t data_off = 0;
    size_t data_size = 0;

    size_t off = 12;
    while (off + 8 <= buf.size()) {
        const char *tag = reinterpret_cast<const char *>(buf.data() + off);
        const uint32_t chunk_sz = read_u32_le(buf.data() + off + 4);
        const size_t chunk_data_off = off + 8;
        if (chunk_data_off + chunk_sz > buf.size()) break;

        if (std::memcmp(tag, "fmt ", 4) == 0 && chunk_sz >= 16) {
            audio_format = read_u16_le(buf.data() + chunk_data_off + 0);
            num_channels = read_u16_le(buf.data() + chunk_data_off + 2);
            sample_rate = read_u32_le(buf.data() + chunk_data_off + 4);
            bits_per_sample = read_u16_le(buf.data() + chunk_data_off + 14);
        } else if (std::memcmp(tag, "data", 4) == 0) {
            data_off = chunk_data_off;
            data_size = chunk_sz;
        }

        off = chunk_data_off + chunk_sz;
        if (off & 1) off++; // align to word boundary
    }

    if (!data_off || !data_size) {
        fprintf(stderr, "error: '%s' has no data chunk\n", path.c_str());
        return false;
    }
    if (!sample_rate || !num_channels) {
        fprintf(stderr, "error: '%s' missing fmt chunk\n", path.c_str());
        return false;
    }
    if (audio_format != 1 && audio_format != 3) {
        fprintf(stderr, "error: '%s' unsupported WAV format %u (only PCM=1 or float=3)\n", path.c_str(), (unsigned) audio_format);
        return false;
    }

    const size_t frame_bytes = (size_t) num_channels * (size_t) (bits_per_sample / 8);
    if (frame_bytes == 0) {
        fprintf(stderr, "error: '%s' invalid bits_per_sample=%u\n", path.c_str(), (unsigned) bits_per_sample);
        return false;
    }
    const size_t n_frames = data_size / frame_bytes;
    out.clear();
    out.reserve(n_frames);

    const uint8_t *data = buf.data() + data_off;
    for (size_t i = 0; i < n_frames; ++i) {
        double sum = 0.0;
        const uint8_t *frame = data + i * frame_bytes;
        for (uint16_t ch = 0; ch < num_channels; ++ch) {
            const uint8_t *p = frame + ch * (bits_per_sample / 8);
            if (audio_format == 1 && bits_per_sample == 16) {
                int16_t s;
                std::memcpy(&s, p, sizeof(s));
                sum += (double) s / 32768.0;
            } else if (audio_format == 1 && bits_per_sample == 32) {
                int32_t s;
                std::memcpy(&s, p, sizeof(s));
                sum += (double) s / 2147483648.0;
            } else if (audio_format == 3 && bits_per_sample == 32) {
                float s;
                std::memcpy(&s, p, sizeof(s));
                sum += (double) s;
            } else {
                fprintf(stderr,
                        "error: '%s' unsupported WAV encoding format=%u bits=%u\n",
                        path.c_str(),
                        (unsigned) audio_format,
                        (unsigned) bits_per_sample);
                return false;
            }
        }
        out.push_back((float) (sum / std::max<int>(1, (int) num_channels)));
    }

    sample_rate_out = (int) sample_rate;
    return true;
}

static std::vector<float> resample_linear(const std::vector<float> &in, int sr_in, int sr_out) {
    if (sr_in <= 0 || sr_out <= 0 || in.empty() || sr_in == sr_out) return in;
    const double ratio = (double) sr_out / (double) sr_in;
    const size_t n_out = (size_t) std::max<int64_t>(1, (int64_t) std::llround((double) in.size() * ratio));
    std::vector<float> out;
    out.resize(n_out);
    for (size_t i = 0; i < n_out; ++i) {
        const double pos = (double) i / ratio;
        const size_t i0 = (size_t) std::floor(pos);
        const size_t i1 = std::min(i0 + 1, in.size() - 1);
        const double t = pos - (double) i0;
        out[i] = (float) ((1.0 - t) * (double) in[i0] + t * (double) in[i1]);
    }
    return out;
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
    SileroVadRunner(const std::string &model_path,
                    int sample_rate,
                    bool use_gpu,
                    int n_threads)
        : sample_rate_(sample_rate),
          context_(nullptr, VadContextDeleter{}) {
        if (sample_rate_ != WHISPER_SAMPLE_RATE) {
            throw std::runtime_error("Silero VAD expects 16 kHz audio");
        }

        whisper_vad_context_params ctx_params = whisper_vad_default_context_params();
        ctx_params.n_threads = std::max(1, n_threads);
        ctx_params.use_gpu = use_gpu;

        context_.reset(whisper_vad_init_from_file_with_params(model_path.c_str(), ctx_params));
        if (!context_) {
            throw std::runtime_error("Failed to initialize Silero VAD context");
        }

        chunk_size_ = expected_chunk_size();
        std::vector<float> probe(chunk_size_, 0.0f);
        if (!whisper_vad_detect_speech(context_.get(), probe.data(), static_cast<int>(probe.size()))) {
            throw std::runtime_error("Failed to probe Silero VAD");
        }
    }

    size_t chunk_size() const {
        return chunk_size_;
    }

    float infer(const float *samples, size_t n_samples) {
        if (!samples || n_samples == 0) {
            throw std::runtime_error("Silero VAD received invalid audio chunk");
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
            throw std::runtime_error("Silero VAD probabilities pointer was null");
        }
        return probs[n_probs - 1];
    }

private:
    size_t expected_chunk_size() const {
        return 512;
    }

    int sample_rate_;
    size_t chunk_size_ = 0;
    std::unique_ptr<whisper_vad_context, VadContextDeleter> context_;
};

std::string escape_json(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '\"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

struct logits_log_writer {
    std::mutex mu;
    std::ofstream file;
    bool enabled = false;
    int flush_ms = 250;
    std::chrono::steady_clock::time_point last_flush = std::chrono::steady_clock::now();
};

struct bias_decode_context {
    int segment_index = -1;
    int partial_seq = -1;
    bool is_final = false;

    const std::vector<std::vector<whisper_token>> * dict_token_seqs = nullptr;
    const std::vector<whisper_token> * dict_first_tokens = nullptr;
    const std::unordered_set<int> * dict_first_token_ids = nullptr;
    int dict_entries = 0;
    int dict_first_tokens_total = 0;
    bool enabled = false;
    float bias_first_logit = 0.35f;
    float bias_continuation_logit = 0.85f;
    int logits_top_k = 50;
    float logits_prob_threshold = 20.0f;
    bool logits_prefix_text = false;
    int logits_boosted_k = 24;

    logits_log_writer * writer = nullptr;
    bool emit_stdout_packets = true;
};

static void whisper_logits_filter_cb(
        whisper_context * ctx,
        whisper_state * /* state */,
        const whisper_token_data * tokens,
        int n_tokens,
        float * logits,
        void * user_data) {
    if (!ctx || !logits || !user_data) return;
    auto * bctx = reinterpret_cast<bias_decode_context *>(user_data);
    if (!bctx->enabled) return;

    const int n_vocab = whisper_n_vocab(ctx);
    if (n_vocab <= 0) return;
    const int token_beg = (int) whisper_token_beg(ctx);

    const float kFirstTokenBias = bctx->bias_first_logit;
    const float kContinuationBias = bctx->bias_continuation_logit;

    std::unordered_map<int, float> boosted_cont;
    boosted_cont.reserve(16);
    int boosted_first_total = 0;

    auto add_bias = [&](int token_id, float bias) {
        if (token_id < 0 || token_id >= n_vocab) return;
        if (token_beg > 0 && token_id >= token_beg) return; // don't bias timestamp/control range
        if (!std::isfinite(logits[token_id])) return;
        logits[token_id] += bias;
    };

    // Boost next tokens when the current beam ends with a dictionary prefix.
    if (bctx->dict_token_seqs) {
        for (const auto &seq : *bctx->dict_token_seqs) {
            if (seq.size() < 2) continue;
            const int max_l = std::min(n_tokens, (int)seq.size() - 1);
            for (int l = max_l; l >= 1; --l) {
                bool match = true;
                for (int j = 0; j < l; ++j) {
                    if (tokens[n_tokens - l + j].id != seq[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    const int next_id = (int) seq[l];
                    add_bias(next_id, kContinuationBias);
                    boosted_cont[next_id] += kContinuationBias;
                    break;
                }
            }
        }
    }

    // If we're currently matching any dictionary prefix, don't also boost dictionary starts for
    // other entries. This prevents unrelated dictionary words from being kept "hot" once a beam is
    // already on a dictionary path.
    if (boosted_cont.empty() && bctx->dict_first_tokens) {
        for (whisper_token tid : *bctx->dict_first_tokens) {
            add_bias((int)tid, kFirstTokenBias);
            boosted_first_total++;
        }
    }

    const bool want_log_packets = bctx->emit_stdout_packets || (bctx->writer && bctx->writer->enabled);
    if (!want_log_packets) {
        return;
    }

    const int top_k = std::max(1, bctx->logits_top_k);

    // Compute top-k probabilities (softmax denom optionally thresholded for speed).
    float max_logit = -INFINITY;
    for (int i = 0; i < n_vocab; ++i) {
        const float v = logits[i];
        if (std::isfinite(v) && v > max_logit) max_logit = v;
    }
    if (!std::isfinite(max_logit)) return;

    double sum_exp = 0.0;
    const float prob_thr = bctx->logits_prob_threshold;
    if (prob_thr <= 0.0f) {
        for (int i = 0; i < n_vocab; ++i) {
            const float v = logits[i];
            if (!std::isfinite(v)) continue;
            sum_exp += std::exp((double)v - (double)max_logit);
        }
    } else {
        const float min_v = max_logit - prob_thr;
        for (int i = 0; i < n_vocab; ++i) {
            const float v = logits[i];
            if (!std::isfinite(v)) continue;
            if (v < min_v) continue;
            sum_exp += std::exp((double)v - (double)max_logit);
        }
    }
    if (!(sum_exp > 0.0)) return;

    struct top_item { int id; float logit; };
    std::vector<top_item> top;
    top.reserve((size_t) top_k);
    for (int i = 0; i < n_vocab; ++i) {
        const float v = logits[i];
        if (!std::isfinite(v)) continue;
        if ((int)top.size() < top_k) {
            top.push_back({i, v});
            if ((int)top.size() == top_k) {
                std::make_heap(top.begin(), top.end(), [](const top_item &a, const top_item &b) { return a.logit > b.logit; }); // min-heap by logit
            }
            continue;
        }
        if (v <= top.front().logit) continue;
        std::pop_heap(top.begin(), top.end(), [](const top_item &a, const top_item &b) { return a.logit > b.logit; });
        top.back() = {i, v};
        std::push_heap(top.begin(), top.end(), [](const top_item &a, const top_item &b) { return a.logit > b.logit; });
    }
    std::sort(top.begin(), top.end(), [](const top_item &a, const top_item &b) { return a.logit > b.logit; });

    auto fnv1a_step = [](uint64_t h, uint32_t v) -> uint64_t {
        h ^= (uint64_t)v;
        h *= 1099511628211ULL;
        return h;
    };

    const uint64_t fnv_offset = 14695981039346656037ULL;
    uint64_t prefix_hash = fnv_offset;
    uint64_t prefix_prev_hash = fnv_offset;
    for (int i = 0; i < n_tokens; ++i) {
        if (i == n_tokens - 1) {
            prefix_prev_hash = prefix_hash;
        }
        const uint32_t tid = (uint32_t) tokens[i].id;
        prefix_hash = fnv1a_step(prefix_hash, tid);
    }
    if (n_tokens == 0) {
        prefix_prev_hash = prefix_hash;
    }

    std::ostringstream prefix_hash_hex;
    prefix_hash_hex << std::hex << std::setw(16) << std::setfill('0') << prefix_hash;
    std::ostringstream prefix_prev_hash_hex;
    prefix_prev_hash_hex << std::hex << std::setw(16) << std::setfill('0') << prefix_prev_hash;

    std::string prefix_text;
    if (bctx->logits_prefix_text) {
        prefix_text.reserve(128);
        const int max_prefix_tokens = std::min(n_tokens, 48);
        for (int i = std::max(0, n_tokens - max_prefix_tokens); i < n_tokens; ++i) {
            const char * tok = whisper_token_to_str(ctx, tokens[i].id);
            if (!tok) continue;
            std::string piece = tok;
            if (is_control_piece(piece)) continue;
            prefix_text += piece;
            if (prefix_text.size() > 256) {
                prefix_text.erase(0, prefix_text.size() - 256);
            }
        }
    }

    std::ostringstream packet;
    packet.setf(std::ios::fixed);
    packet << "{\"event\":\"logits\""
           << ",\"segment_index\":" << bctx->segment_index
           << ",\"partial_seq\":" << bctx->partial_seq
           << ",\"final\":" << (bctx->is_final ? "true" : "false")
           << ",\"decode_step\":" << n_tokens
           << ",\"prefix_len\":" << n_tokens
           << ",\"prefix_hash\":\"" << prefix_hash_hex.str() << "\""
           << ",\"prefix_prev_hash\":\"" << prefix_prev_hash_hex.str() << "\""
           << ",\"prefix_text\":\"" << escape_json(prefix_text) << "\""
           << ",\"prob_mode\":\"" << (prob_thr <= 0.0f ? "full" : "threshold") << "\""
           << ",\"prob_threshold\":" << prob_thr
           << ",\"bias_first_logit\":" << kFirstTokenBias
           << ",\"bias_continuation_logit\":" << kContinuationBias
           << ",\"dict_entries\":" << bctx->dict_entries
           << ",\"dict_first_tokens\":" << bctx->dict_first_tokens_total
           << ",\"boosted_first_total\":" << boosted_first_total
           << ",\"boosted_cont_count\":" << (int) boosted_cont.size();

    if (n_tokens > 0) {
        const int last_id = tokens[n_tokens - 1].id;
        const char * last_tok = whisper_token_to_str(ctx, last_id);
        packet << ",\"prefix_last_id\":" << last_id
               << ",\"prefix_last_text\":\"" << escape_json(last_tok ? last_tok : "") << "\"";
    }

    // boosted tokens (for debugging dictionary bias):
    // - first: dictionary first-token boosts that appear in the current top-k list
    // - continuation: tokens boosted due to current prefix match (may or may not be in top-k)
    {
        const int boosted_k = std::max(0, bctx->logits_boosted_k);
        std::unordered_set<int> emitted_ids;
        emitted_ids.reserve((size_t)boosted_k * 2 + 8);
        int emitted = 0;
        packet << ",\"boosted\":[";

        auto emit_item = [&](int tid, const char *kind, float bias, bool in_top) {
            if (boosted_k <= 0) return;
            if (emitted >= boosted_k) return;
            if (!emitted_ids.insert(tid).second) return;
            if (emitted) packet << ",";
            const char * tok = whisper_token_to_str(ctx, tid);
            const float logit_after = logits[tid];
            const float logit_before = logit_after - bias;
            packet << "{\"id\":" << tid
                   << ",\"text\":\"" << escape_json(tok ? tok : "") << "\""
                   << ",\"bias\":" << bias
                   << ",\"in_top\":" << (in_top ? "true" : "false")
                   << ",\"logit_before\":" << logit_before
                   << ",\"logit_after\":" << logit_after
                   << ",\"kind\":\"" << kind << "\"}";
            emitted++;
        };

        if (boosted_k > 0) {
            // first boosts that are currently in top-k
            if (bctx->dict_first_token_ids && kFirstTokenBias != 0.0f) {
                for (size_t i = 0; i < top.size() && emitted < boosted_k; ++i) {
                    const int tid = top[i].id;
                    if (bctx->dict_first_token_ids->find(tid) == bctx->dict_first_token_ids->end()) continue;
                    emit_item(tid, "first", kFirstTokenBias, true);
                }
            }

            // continuation boosts, prefer ones in top-k
            for (size_t i = 0; i < top.size() && emitted < boosted_k; ++i) {
                const int tid = top[i].id;
                const auto it = boosted_cont.find(tid);
                if (it == boosted_cont.end()) continue;
                emit_item(tid, "continuation", it->second, true);
            }

            // continuation boosts not in top-k
            for (const auto &kv : boosted_cont) {
                if (emitted >= boosted_k) break;
                emit_item(kv.first, "continuation", kv.second, false);
            }
        }

        packet << "]";
    }

    packet << ",\"top\":[";

    for (size_t i = 0; i < top.size(); ++i) {
        if (i) packet << ",";
        const int tid = top[i].id;
        const float v = top[i].logit;
        const double p = std::exp((double)v - (double)max_logit) / sum_exp;
        const char * tok = whisper_token_to_str(ctx, tid);
        std::string tok_s = tok ? tok : "";
        packet << "{\"id\":" << tid
               << ",\"text\":\"" << escape_json(tok_s) << "\""
               << ",\"logit\":" << v
               << ",\"prob\":" << p
               << "}";
    }
    packet << "]}\n";

    const std::string line = packet.str();

    if (bctx->emit_stdout_packets) {
        fputs(line.c_str(), stdout);
    }

    if (bctx->writer && bctx->writer->enabled) {
        std::lock_guard<std::mutex> lock(bctx->writer->mu);
        bctx->writer->file << line;
        if (bctx->writer->flush_ms >= 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - bctx->writer->last_flush).count();
            if (elapsed >= bctx->writer->flush_ms) {
                bctx->writer->file.flush();
                bctx->writer->last_flush = now;
            }
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    // Make stdout line-buffered even when piped (helps avoid per-line fflush overhead).
    setvbuf(stdout, nullptr, _IOLBF, 0);

    ggml_backend_load_all();

    vad_params params;
    if (!parse_args(argc, argv, params)) {
        return 1;
    }

    if (params.vad_model_path.empty()) {
        fprintf(stderr, "error: --silero-vad path required\n");
        return 1;
    }

    if (params.stop_threshold > params.start_threshold) {
        fprintf(stderr, "warning: stop threshold higher than start threshold, clamping\n");
        params.stop_threshold = params.start_threshold;
    }

    if (!std::filesystem::exists(params.model)) {
        fprintf(stderr, "error: whisper model not found at '%s'\n", params.model.c_str());
        return 1;
    }
    if (!std::filesystem::exists(params.vad_model_path)) {
        fprintf(stderr, "error: silero VAD model not found at '%s'\n", params.vad_model_path.c_str());
        return 1;
    }
    if (params.language != "auto" && whisper_lang_id(params.language.c_str()) == -1) {
        fprintf(stderr, "error: unknown language '%s'\n", params.language.c_str());
        return 1;
    }

    const int sample_rate = WHISPER_SAMPLE_RATE;
    const bool enable_partials = params.step_ms >= 0;
    const int64_t step_samples = enable_partials
        ? std::max<int64_t>(1, (int64_t)params.step_ms * sample_rate / 1000)
        : 0;
    const size_t pre_padding_samples = static_cast<size_t>(std::max<int64_t>(0, (int64_t)params.pre_padding_ms * sample_rate / 1000));
    const size_t post_padding_samples = static_cast<size_t>(std::max<int64_t>(0, (int64_t)params.post_padding_ms * sample_rate / 1000));
    const size_t min_silence_samples = static_cast<size_t>(std::max<int64_t>(0, (int64_t)params.min_silence_ms * sample_rate / 1000));
    const size_t min_segment_samples = static_cast<size_t>(std::max<int64_t>(0, (int64_t)params.min_segment_ms * sample_rate / 1000));
    const size_t max_segment_samples = static_cast<size_t>(std::max<int64_t>(static_cast<int64_t>(sample_rate), (int64_t)params.max_segment_ms * sample_rate / 1000));

    const bool use_stdin_audio = params.stdin_audio;
    const bool use_stdin_pcm = params.stdin_pcm;
    const bool use_mic_capture = params.audio_file.empty() && !use_stdin_audio && !use_stdin_pcm;
    audio_async audio(std::max(params.ring_buffer_ms, params.max_segment_ms + params.post_padding_ms + 2000));
    if (use_mic_capture) {
        if (!audio.init(params.capture_id, sample_rate)) {
            fprintf(stderr, "audio.init() failed\n");
            return 1;
        }
        audio.resume();
    }

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = params.use_gpu_whisper;
    cparams.dtw_token_timestamps = true;
    cparams.dtw_aheads_preset = WHISPER_AHEADS_BASE_EN;

	whisper_context *ctx = whisper_init_from_file_with_params(params.model.c_str(), cparams);
	if (!ctx) {
		fprintf(stderr, "failed to initialize whisper context\n");
        return 2;
    }

    std::unique_ptr<SileroVadRunner> vad;
    size_t vad_chunk_samples = 0;
    try {
        vad = std::make_unique<SileroVadRunner>(params.vad_model_path,
                                                sample_rate,
                                                false,
                                                params.n_threads);
        vad_chunk_samples = vad->chunk_size();
    } catch (const std::exception &ex) {
        fprintf(stderr, "error: failed to initialize Silero VAD: %s\n", ex.what());
        whisper_free(ctx);
		return 1;
	}

	const bool log_stdout_packets = params.log || params.debug;
	const bool enable_dictionary_file = params.log;
	const bool enable_logits_file = params.log || !params.logits_log_path.empty();
	const bool verbose_dictionary_packets = params.log || params.debug;

	logits_log_writer logits_writer;
	std::string logits_log_path;
	logits_writer.flush_ms = params.logits_flush_ms;
	if (enable_logits_file) {
		try {
			if (!params.logits_log_path.empty()) {
				logits_log_path = std::filesystem::absolute(params.logits_log_path).string();
			} else {
				logits_log_path = std::filesystem::absolute(".voice/whisper_logits.jsonl").string();
			}
			const auto parent = std::filesystem::path(logits_log_path).parent_path();
			if (!parent.empty()) {
				std::filesystem::create_directories(parent);
			}
			logits_writer.file.open(logits_log_path, std::ios::out | std::ios::app);
			logits_writer.enabled = logits_writer.file.good();
			if (!logits_writer.enabled) {
				fprintf(stderr, "warning: failed to open '%s' for append\n", logits_log_path.c_str());
			}
		} catch (const std::exception &ex) {
			fprintf(stderr, "warning: failed to initialize logits log writer: %s\n", ex.what());
		}
	}

	const std::string cwd = std::filesystem::current_path().string();
	fprintf(stderr,
		"vad ready: cwd='%s' dict='%s' send_prompt=%d bias_decoding=%d bias_first=%.3f bias_cont=%.3f logits_log='%s'\n",
		cwd.c_str(),
		params.dictionary_path.c_str(),
		params.send_prompt ? 1 : 0,
		params.bias_decoding ? 1 : 0,
		params.bias_first_logit,
		params.bias_continuation_logit,
		logits_writer.enabled ? logits_log_path.c_str() : "");

    printf("{\"event\":\"ready\",\"cwd\":\"%s\",\"dictionary_file\":\"%s\",\"send_prompt\":%s,\"bias_decoding\":%s,\"bias_first_logit\":%.6f,\"bias_continuation_logit\":%.6f,\"logits_log_path\":\"%s\",\"logits_log_enabled\":%s}\n",
           escape_json(cwd).c_str(),
           escape_json(params.dictionary_path).c_str(),
           params.send_prompt ? "true" : "false",
           params.bias_decoding ? "true" : "false",
           params.bias_first_logit,
           params.bias_continuation_logit,
           escape_json(logits_log_path).c_str(),
           logits_writer.enabled ? "true" : "false");
    fflush(stdout);

    std::deque<float> pending_samples;
    std::deque<float> pre_roll;
    std::vector<float> chunk_buffer;
    std::vector<float> current_segment;
    double segment_prob_sum = 0.0;
    int segment_prob_count = 0;
    bool in_segment = false;
    int64_t segment_start_sample = 0;
    int64_t last_voice_sample = 0;
    int64_t processed_samples_total = 0;
    int64_t last_fetch_time_ms = 0;
    int segment_index = 0;
    int active_segment_index = -1;
    int partial_sequence = 0;
    int64_t last_partial_emit_sample = 0;

    const int fetch_window_ms = std::min<int>(params.ring_buffer_ms, params.max_segment_ms + params.post_padding_ms + 2000);

    auto reset_segment_state = [&]() {
        pending_samples.clear();
        pre_roll.clear();
        chunk_buffer.clear();
        current_segment.clear();
        segment_prob_sum = 0.0;
        segment_prob_count = 0;
        in_segment = false;
        segment_start_sample = 0;
        last_voice_sample = 0;
        processed_samples_total = 0;
        last_fetch_time_ms = 0;
        segment_index = 0;
        active_segment_index = -1;
        partial_sequence = 0;
        last_partial_emit_sample = 0;
    };

    std::string dictionary_cache;
    std::vector<std::vector<whisper_token>> dictionary_token_seqs;
    std::vector<std::string> dictionary_entry_texts;
    std::vector<whisper_token> dictionary_first_tokens;
    std::unordered_set<int> dictionary_first_token_ids;
    auto last_dictionary_reload = std::chrono::steady_clock::time_point::min();
    auto last_dictionary_write_time = std::filesystem::file_time_type::min();
    int last_dictionary_entries_raw = 0;
    int last_dictionary_total_tokens = 0;
	std::string last_dictionary_error;

	auto emit_dictionary_event = [&](int segment_idx, int partial_seq, bool is_final, bool attempted, bool reloaded) {
		std::ostringstream packet;
		packet << "{\"event\":\"dictionary\""
               << ",\"dictionary_file\":\"" << escape_json(params.dictionary_path) << "\""
               << ",\"segment_index\":" << segment_idx
               << ",\"partial_seq\":" << partial_seq
               << ",\"final\":" << (is_final ? "true" : "false")
               << ",\"attempted\":" << (attempted ? "true" : "false")
               << ",\"reloaded\":" << (reloaded ? "true" : "false")
               << ",\"ok\":" << (last_dictionary_error.empty() ? "true" : "false")
               << ",\"error\":\"" << escape_json(last_dictionary_error) << "\""
               << ",\"dict_entries_raw\":" << last_dictionary_entries_raw
               << ",\"dict_entries\":" << (int) dictionary_token_seqs.size()
               << ",\"dict_first_tokens\":" << (int) dictionary_first_tokens.size()
			   << ",\"dict_total_tokens\":" << last_dictionary_total_tokens
			   << ",\"dict_cache_bytes\":" << (int) dictionary_cache.size();

		if (verbose_dictionary_packets) {
			// Sample a few parsed & tokenized entries (proves the C++ actually has them).
			const int kMaxWords = 40;
			packet << ",\"words\":[";
			const int n_sample = std::min<int>(
					kMaxWords,
					std::min<int>((int) dictionary_entry_texts.size(), (int) dictionary_token_seqs.size()));
			for (int i = 0; i < n_sample; ++i) {
				if (i) packet << ",";
				packet << "{\"text\":\"" << escape_json(dictionary_entry_texts[i]) << "\"";
				packet << ",\"tokens\":[";
				const auto &seq = dictionary_token_seqs[i];
				for (size_t j = 0; j < seq.size(); ++j) {
					if (j) packet << ",";
					const int tid = (int) seq[j];
					const char *tok = whisper_token_to_str(ctx, tid);
					packet << "{\"id\":" << tid << ",\"text\":\"" << escape_json(tok ? tok : "") << "\"}";
				}
				packet << "]}";
			}
			packet << "]}";
		} else {
			packet << ",\"words\":[]}";
		}

		const std::string line = packet.str() + "\n";

		// stdout
		fputs(line.c_str(), stdout);

		// file
		if (enable_dictionary_file && logits_writer.enabled) {
			std::lock_guard<std::mutex> lock(logits_writer.mu);
			logits_writer.file << line;
			if (logits_writer.flush_ms >= 0) {
				const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - logits_writer.last_flush).count();
                if (elapsed >= logits_writer.flush_ms) {
                    logits_writer.file.flush();
                    logits_writer.last_flush = now;
                }
            }
        }
    };

	auto reload_dictionary_if_needed = [&](int segment_idx, int partial_seq, bool is_final, bool force) {
        if (params.dictionary_path.empty()) {
            last_dictionary_error = "dictionary_file not set";
            last_dictionary_entries_raw = 0;
            last_dictionary_total_tokens = 0;
            dictionary_cache.clear();
            dictionary_token_seqs.clear();
            dictionary_entry_texts.clear();
            dictionary_first_tokens.clear();
            dictionary_first_token_ids.clear();
            emit_dictionary_event(segment_idx, partial_seq, is_final, true, true);
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_dictionary_reload).count();
        const bool should_reload = force || elapsed_ms >= params.dictionary_poll_ms;
        if (!should_reload) return;
        last_dictionary_reload = now;

        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(params.dictionary_path, ec);
        if (ec) {
            last_dictionary_error = ec.message();
            last_dictionary_entries_raw = 0;
            last_dictionary_total_tokens = 0;
            dictionary_cache.clear();
            dictionary_token_seqs.clear();
            dictionary_entry_texts.clear();
            dictionary_first_tokens.clear();
            dictionary_first_token_ids.clear();
            emit_dictionary_event(segment_idx, partial_seq, is_final, true, true);
            return;
        }

        const bool changed = (mtime != last_dictionary_write_time);
        if (!force && !changed) {
            // Still emit a status line occasionally, so the UI can show what the transcriber thinks it has.
            emit_dictionary_event(segment_idx, partial_seq, is_final, true, false);
            return;
        }

        std::ifstream dict_file(params.dictionary_path);
        if (!dict_file.good()) {
            last_dictionary_error = "failed to open dictionary_file";
            last_dictionary_entries_raw = 0;
            last_dictionary_total_tokens = 0;
            dictionary_cache.clear();
            dictionary_token_seqs.clear();
            dictionary_entry_texts.clear();
            dictionary_first_tokens.clear();
            dictionary_first_token_ids.clear();
            emit_dictionary_event(segment_idx, partial_seq, is_final, true, true);
            return;
        }

        std::ostringstream ss;
        ss << dict_file.rdbuf();
        dictionary_cache = ss.str();
        last_dictionary_write_time = mtime;

        dictionary_token_seqs.clear();
        dictionary_entry_texts.clear();
        dictionary_first_tokens.clear();
        dictionary_first_token_ids.clear();

        const auto entries = split_dictionary_entries(dictionary_cache);
        last_dictionary_entries_raw = (int) entries.size();
        dictionary_token_seqs.reserve(entries.size());
        dictionary_entry_texts.reserve(entries.size());

        std::unordered_set<int> first_seen;
        first_seen.reserve(entries.size() * 2 + 8);

        int total_tokens = 0;
        last_dictionary_error.clear();

        for (const auto &entry : entries) {
            if (entry.empty()) continue;

            // Tokenize both variants (with and without a leading space). Whisper sometimes produces
            // either representation depending on context; supporting both makes continuation-bias
            // much more reliable.
            std::vector<std::string> variants;
            variants.reserve(2);
            variants.push_back(entry);
            if (entry.front() != ' ') {
                variants.push_back(" " + entry);
            }

            for (const auto &text : variants) {
                const int n_needed = whisper_token_count(ctx, text.c_str());
                if (n_needed <= 0) continue;

                std::vector<whisper_token> seq((size_t)n_needed);
                const int n_got = whisper_tokenize(ctx, text.c_str(), seq.data(), (int)seq.size());
                if (n_got <= 0) continue;
                seq.resize((size_t)n_got);
                total_tokens += n_got;

                if (!seq.empty()) {
                    const int first = (int)seq.front();
                    if (first_seen.insert(first).second) {
                        dictionary_first_tokens.push_back(seq.front());
                        dictionary_first_token_ids.insert(first);
                    }
                }
                dictionary_entry_texts.push_back(entry);
                dictionary_token_seqs.push_back(std::move(seq));
            }
        }

        last_dictionary_total_tokens = total_tokens;

        if (params.debug) {
            fprintf(stderr,
                    "dictionary reload: %zu raw entries, %zu tokenized entries, %zu unique first tokens, %d total tokens (send_prompt=%d bias_decoding=%d)\n",
                    entries.size(),
                    dictionary_token_seqs.size(),
                    dictionary_first_tokens.size(),
                    total_tokens,
                    params.send_prompt ? 1 : 0,
                    params.bias_decoding ? 1 : 0);
        }

        emit_dictionary_event(segment_idx, partial_seq, is_final, true, true);
	};

	bool warned_beam_size_clamp = false;
	auto emit_transcription = [&](const std::vector<float> &audio_segment,
	                                  int segment_idx,
	                                  int64_t segment_start_sample,
	                                  bool is_final,
	                                  double avg_prob_now,
	                                  int partial_seq) {
        if (audio_segment.empty()) {
            return;
        }

        std::string prompt_trimmed;
        whisper_full_params wparams = whisper_full_default_params(
                params.bias_decoding ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
        wparams.print_progress = false;
        wparams.print_special = false;
        wparams.print_realtime = false;
        wparams.print_timestamps = true;
        wparams.no_context = true;
        wparams.single_segment = false;
        wparams.max_tokens = 0;
        wparams.language = params.language.c_str();
        wparams.n_threads = params.n_threads;
        wparams.token_timestamps = true;
        wparams.thold_pt = 0.01f;
        wparams.entropy_thold = 2.40f;
        wparams.logprob_thold = -1.0f;
        wparams.no_speech_thold = 0.0f;

        reload_dictionary_if_needed(segment_idx, partial_seq, is_final, false);

        if (params.send_prompt && !dictionary_cache.empty()) {
            prompt_trimmed = dictionary_cache;
            if (prompt_trimmed.size() > 4096) {
                prompt_trimmed.resize(4096);
            }
            wparams.initial_prompt = prompt_trimmed.c_str();
        } else {
            wparams.initial_prompt = nullptr;
        }

		bias_decode_context bctx;
		if (params.bias_decoding) {
			// whisper.cpp currently uses a fixed-size decoder array (WHISPER_MAX_DECODERS = 8).
			// Passing a larger beam_size causes whisper_full() to fail with:
			//   "too many decoders requested (...), max = 8"
			constexpr int kWhisperMaxDecoders = 8;

			bctx.segment_index = segment_idx;
			bctx.partial_seq = partial_seq;
			bctx.is_final = is_final;
			if (!dictionary_token_seqs.empty()) {
                bctx.dict_token_seqs = &dictionary_token_seqs;
            }
            if (!dictionary_first_tokens.empty()) {
                bctx.dict_first_tokens = &dictionary_first_tokens;
            }
            if (!dictionary_first_token_ids.empty()) {
                bctx.dict_first_token_ids = &dictionary_first_token_ids;
            }
            bctx.dict_entries = last_dictionary_entries_raw;
            bctx.dict_first_tokens_total = (int) dictionary_first_tokens.size();
            bctx.enabled = true;
            bctx.bias_first_logit = params.bias_first_logit;
            bctx.bias_continuation_logit = params.bias_continuation_logit;
			bctx.logits_top_k = params.logits_top_k;
			bctx.logits_prob_threshold = params.logits_prob_threshold;
			bctx.logits_prefix_text = params.logits_prefix_text;
			bctx.logits_boosted_k = params.logits_boosted_k;
			bctx.writer = logits_writer.enabled ? &logits_writer : nullptr;
			bctx.emit_stdout_packets = log_stdout_packets;

			wparams.logits_filter_callback = whisper_logits_filter_cb;
			wparams.logits_filter_callback_user_data = &bctx;
			const int requested_beam = params.beam_size > 0 ? params.beam_size : wparams.beam_search.beam_size;
			const int clamped_beam = std::clamp(requested_beam, 2, kWhisperMaxDecoders);
			if (requested_beam != clamped_beam && !warned_beam_size_clamp) {
				fprintf(stderr, "warning: clamping --beam-size %d to %d (whisper max decoders)\n",
				        requested_beam, clamped_beam);
				warned_beam_size_clamp = true;
			}
			wparams.beam_search.beam_size = clamped_beam;
		}

        if (whisper_full(ctx, wparams, audio_segment.data(), audio_segment.size()) != 0) {
            fprintf(stderr, "whisper_full failed on segment %d (final=%d)\n", segment_idx, is_final ? 1 : 0);
            return;
        }

        struct Piece {
            std::string text;
            int64_t t0_ms;
            int64_t t1_ms;
            bool leading_space;
        };

        std::vector<Piece> pieces;
        std::string full_text;

        const int64_t segment_start_ms = (segment_start_sample * 1000LL) / sample_rate;
        const int64_t segment_end_ms = segment_start_ms + ((int64_t)audio_segment.size() * 1000LL) / sample_rate;
        const int64_t duration_ms = std::max<int64_t>(0, segment_end_ms - segment_start_ms);

        const int n_segments = whisper_full_n_segments(ctx);
        for (int s = 0; s < n_segments; ++s) {
            const int n_tok = whisper_full_n_tokens(ctx, s);
            for (int i = 0; i < n_tok; ++i) {
                auto td = whisper_full_get_token_data(ctx, s, i);
                const char *pc = whisper_token_to_str(ctx, td.id);
                if (!pc) continue;
                std::string piece = pc;
                if (is_control_piece(piece)) continue;

                bool leading = (!piece.empty() && std::isspace(static_cast<unsigned char>(piece[0])));
                int64_t t0 = td.t0 >= 0 ? segment_start_ms + (int64_t)td.t0 * 10 : -1;
                int64_t t1 = td.t1 >= 0 ? segment_start_ms + (int64_t)td.t1 * 10 : -1;

                pieces.push_back({piece, t0, t1, leading});
                full_text += piece;
            }
        }

        printf("{\"event\":\"segment\",\"segment_index\":%d,\"start_ms\":%lld,\"end_ms\":%lld,\"duration_ms\":%lld,\"avg_vad\":%.6f,\"final\":%s,\"partial_seq\":%d,\"text\":\"%s\",\"tokens\":[",
               segment_idx,
               (long long)segment_start_ms,
               (long long)segment_end_ms,
               (long long)duration_ms,
               avg_prob_now,
               is_final ? "true" : "false",
               partial_seq,
               escape_json(full_text).c_str());

        for (size_t i = 0; i < pieces.size(); ++i) {
            if (i) printf(",");
            const auto &p = pieces[i];
            printf("{\"text\":\"%s\",\"t0_ms\":%lld,\"t1_ms\":%lld,\"leading_space\":%s}",
                   escape_json(p.text).c_str(),
                   (long long)p.t0_ms,
                   (long long)p.t1_ms,
                   p.leading_space ? "true" : "false");
        }

        printf("]}\n");
        fflush(stdout);
    };

    // Emit an initial dictionary status line so the UI can confirm what the transcriber loaded,
    // even before the first decode happens.
    reload_dictionary_if_needed(-1, -1, false, true);

    auto flush_segment = [&](bool forced_flush) {
        if (!in_segment || current_segment.empty()) {
            current_segment.clear();
            segment_prob_sum = 0.0;
            segment_prob_count = 0;
            in_segment = false;
            return;
        }

        size_t keep_samples = current_segment.size();
        if (!forced_flush) {
            int64_t wanted_end_sample = last_voice_sample + static_cast<int64_t>(post_padding_samples);
            if (wanted_end_sample < segment_start_sample) {
                wanted_end_sample = segment_start_sample;
            }
            size_t desired = static_cast<size_t>(std::max<int64_t>(0, wanted_end_sample - segment_start_sample));
            if (desired > current_segment.size()) desired = current_segment.size();
            keep_samples = desired;
        }

        if (keep_samples < min_segment_samples) {
            if (params.debug) {
                fprintf(stderr, "discarding short segment (%zu samples)\n", keep_samples);
            }
            current_segment.clear();
            segment_prob_sum = 0.0;
            segment_prob_count = 0;
            in_segment = false;
            pre_roll.clear();
            return;
        }

        std::vector<float> audio_segment(current_segment.begin(), current_segment.begin() + keep_samples);
        std::vector<float> leftover;
        if (keep_samples < current_segment.size()) {
            leftover.assign(current_segment.begin() + static_cast<std::ptrdiff_t>(keep_samples), current_segment.end());
        }

        const double avg_prob = segment_prob_count > 0 ? (segment_prob_sum / segment_prob_count) : 0.0;

        emit_transcription(audio_segment,
                           active_segment_index >= 0 ? active_segment_index : segment_index,
                           segment_start_sample,
                           true,
                           avg_prob,
                           partial_sequence);

        pre_roll.clear();
        for (float sample : leftover) {
            pre_roll.push_back(sample);
            if (pre_roll.size() > pre_padding_samples) {
                pre_roll.pop_front();
            }
        }

        current_segment.clear();
        segment_prob_sum = 0.0;
        segment_prob_count = 0;
        in_segment = false;
        partial_sequence = 0;
        last_partial_emit_sample = 0;
        active_segment_index = -1;
        ++segment_index;
        segment_start_sample = processed_samples_total;
        last_voice_sample = processed_samples_total;
    };

    auto process_pending_chunks = [&]() {
        while (pending_samples.size() >= vad_chunk_samples) {
            auto it_end = pending_samples.begin();
            std::advance(it_end, vad_chunk_samples);
            chunk_buffer.assign(pending_samples.begin(), it_end);
            pending_samples.erase(pending_samples.begin(), it_end);

            float prob = 0.0f;
            try {
                prob = vad->infer(chunk_buffer.data(), chunk_buffer.size());
            } catch (const std::exception &ex) {
                fprintf(stderr, "VAD inference failed: %s\n", ex.what());
                continue;
            }

            processed_samples_total += (int64_t) vad_chunk_samples;
            int64_t chunk_end_ms = (processed_samples_total * 1000LL) / sample_rate;

            if (params.emit_vad_events) {
                printf("{\"event\":\"vad\",\"audio_time_ms\":%lld,\"prob\":%.6f,\"vad_chunk_samples\":%zu,\"vad_sample_rate\":%d}\n",
                       (long long)chunk_end_ms,
                       prob,
                       vad_chunk_samples,
                       sample_rate);
            }

            if (!in_segment && prob >= params.start_threshold) {
                if (params.debug) {
                    fprintf(stderr, "segment %d start at %lld ms (prob=%.3f)\n", segment_index, (long long)chunk_end_ms, prob);
                }
                current_segment.assign(pre_roll.begin(), pre_roll.end());
                segment_start_sample = processed_samples_total - static_cast<int64_t>(pre_roll.size()) - static_cast<int64_t>(vad_chunk_samples);
                if (segment_start_sample < 0) segment_start_sample = 0;
                active_segment_index = segment_index;
                partial_sequence = 0;
                last_partial_emit_sample = segment_start_sample;
                current_segment.insert(current_segment.end(), chunk_buffer.begin(), chunk_buffer.end());
                pre_roll.clear();

                last_voice_sample = processed_samples_total;
                segment_prob_sum = prob;
                segment_prob_count = 1;
                in_segment = true;
                continue;
            }

            if (in_segment) {
                current_segment.insert(current_segment.end(), chunk_buffer.begin(), chunk_buffer.end());
                segment_prob_sum += prob;
                segment_prob_count += 1;
                if (prob >= params.stop_threshold) {
                    last_voice_sample = processed_samples_total;
                }

                const int64_t current_segment_end_sample = segment_start_sample + static_cast<int64_t>(current_segment.size());
                if (enable_partials &&
                    current_segment.size() >= min_segment_samples &&
                    current_segment_end_sample - last_partial_emit_sample >= step_samples) {
                    const double avg_prob_now = segment_prob_count > 0 ? (segment_prob_sum / segment_prob_count) : 0.0;
                    emit_transcription(current_segment,
                                       active_segment_index >= 0 ? active_segment_index : segment_index,
                                       segment_start_sample,
                                       false,
                                       avg_prob_now,
                                       partial_sequence);
                    last_partial_emit_sample = current_segment_end_sample;
                    ++partial_sequence;
                }

                int64_t segment_samples = processed_samples_total - segment_start_sample;
                int64_t silence_samples = processed_samples_total - last_voice_sample;

                bool over_max = segment_samples >= static_cast<int64_t>(max_segment_samples);
                bool enough_silence = silence_samples >= static_cast<int64_t>(min_silence_samples);
                bool has_post = silence_samples >= static_cast<int64_t>(post_padding_samples);

                if (over_max) {
                    if (params.debug) {
                        fprintf(stderr, "segment %d forced flush (max length)\n", segment_index);
                    }
                    flush_segment(true);
                } else if (enough_silence && has_post) {
                    if (params.debug) {
                        fprintf(stderr, "segment %d flush after silence (prob=%.3f)\n", segment_index, prob);
                    }
                    flush_segment(false);
                }
            } else {
                for (float sample : chunk_buffer) {
                    pre_roll.push_back(sample);
                    if (pre_roll.size() > pre_padding_samples) {
                        pre_roll.pop_front();
                    }
                }
            }
        }
    };

    if (use_mic_capture) {
        while (sdl_poll_events()) {
            std::vector<float> window_pcm;
            int64_t audio_time_ms = 0;
            audio.get(fetch_window_ms, window_pcm, audio_time_ms);

            if (audio_time_ms <= last_fetch_time_ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            int64_t delta_ms = audio_time_ms - last_fetch_time_ms;
            size_t new_samples = static_cast<size_t>((delta_ms * sample_rate) / 1000);
            if (new_samples > window_pcm.size()) {
                new_samples = window_pcm.size();
            }
            if (new_samples > 0) {
                const size_t start = window_pcm.size() - new_samples;
                for (size_t i = start; i < window_pcm.size(); ++i) {
                    pending_samples.push_back(window_pcm[i]);
                }
            }
            last_fetch_time_ms = audio_time_ms;

            process_pending_chunks();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        flush_segment(true);
        audio.pause();
    } else if (use_stdin_audio) {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) {
                continue;
            }
            if (line == "__quit__") {
                break;
            }

            reset_segment_state();

            std::vector<float> offline_pcm;
            int sr_in = 0;
            if (!read_wav_mono_f32(line, offline_pcm, sr_in)) {
                fprintf(stderr, "error: failed to open audio file '%s'\n", line.c_str());
                continue;
            }
            if (sr_in != sample_rate) {
                offline_pcm = resample_linear(offline_pcm, sr_in, sample_rate);
            }

            printf("{\"event\":\"job_start\",\"path\":\"%s\"}\n", escape_json(line).c_str());
            fflush(stdout);

            for (float s : offline_pcm) {
                pending_samples.push_back(s);
            }
            const size_t rem = pending_samples.size() % vad_chunk_samples;
            if (rem) {
                const size_t pad = vad_chunk_samples - rem;
                for (size_t i = 0; i < pad; ++i) pending_samples.push_back(0.0f);
            }
            process_pending_chunks();
            flush_segment(true);

            printf("{\"event\":\"job_end\",\"path\":\"%s\"}\n", escape_json(line).c_str());
            fflush(stdout);
        }
    } else if (use_stdin_pcm) {
        auto reset_state_and_emit = [&]() {
            reset_segment_state();
        };

        auto read_exact = [&](void *dst, size_t n) -> bool {
            return fread(dst, 1, n, stdin) == n;
        };

        while (true) {
            uint8_t tag = 0;
            if (!read_exact(&tag, 1)) {
                break;
            }
            if (tag == 'Q') {
                break;
            }
            if (tag == 'B') {
                reset_state_and_emit();
                printf("{\"event\":\"job_start\"}\n");
                fflush(stdout);
                continue;
            }
            if (tag == 'E') {
                flush_segment(true);
                printf("{\"event\":\"job_end\"}\n");
                fflush(stdout);
                continue;
            }
            if (tag == 'J') {
                uint32_t n = 0;
                if (!read_exact(&n, sizeof(uint32_t))) {
                    break;
                }
                if (n == 0) {
                    continue;
                }
                std::vector<float> samples(n);
                if (!read_exact(samples.data(), n * sizeof(float))) {
                    break;
                }
                for (float s : samples) {
                    pending_samples.push_back(s);
                }
                process_pending_chunks();
                continue;
            }
        }
    } else {
        std::vector<float> offline_pcm;
        int sr_in = 0;
        if (!read_wav_mono_f32(params.audio_file, offline_pcm, sr_in)) {
            whisper_free(ctx);
            return 1;
        }
        if (sr_in != sample_rate) {
            offline_pcm = resample_linear(offline_pcm, sr_in, sample_rate);
        }
        if (params.debug) {
            fprintf(stderr,
                    "offline audio: '%s' -> %zu samples @ %d Hz\n",
                    params.audio_file.c_str(),
                    offline_pcm.size(),
                    sample_rate);
        }
        for (float s : offline_pcm) {
            pending_samples.push_back(s);
        }
        const size_t rem = pending_samples.size() % vad_chunk_samples;
        if (rem) {
            const size_t pad = vad_chunk_samples - rem;
            for (size_t i = 0; i < pad; ++i) pending_samples.push_back(0.0f);
        }
        process_pending_chunks();
        flush_segment(true);
    }

    whisper_free(ctx);
    return 0;
}
