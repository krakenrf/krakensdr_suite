// kraken_whisper - local speech-to-text for kraken_doa's Voice to Text
// (src/voice2text.cpp), built on whisper.cpp (MIT, github.com/ggml-org/whisper.cpp).
// Free and fully offline: the model files live in transcribe/models/.
//
//   kraken_whisper --serve --model M.bin [--vad V.bin] [--threads N] [--language L]
//       live: jobs on stdin, results on stdout (include/voice2text_wire.hpp)
//   kraken_whisper --file clip.wav --model M.bin [--vad V.bin] [--threads N]
//                  [--language L] [--prompt "words"] [--beam N] [--no-vad]
//                  [--partial | --stream STEP_MS]
//       offline test: transcribes a WAV (16-bit PCM) and prints the result;
//       --partial: one Live text decode of the whole file instead,
//       --stream: Live text as kraken_doa runs it (a partial decode of the
//       audio so far every STEP_MS, the words two partials agree on), then the
//       final transcript
//   kraken_whisper --quantize IN.bin OUT.bin q8_0|q5_0|q5_1
//       an f16 model -> 8 / 5-bit weights (quantize.cpp)
//
// One job at a time (kraken_doa queues them). Each job: the utterance ->
// Silero VAD (when its model is given): only the speech parts, padded, joined
// by short silences -> whisper -> text, cleaned of non-speech annotations
// ("[BLANK_AUDIO]", "(static)") and of the loops and stock phrases whisper
// invents on noise. A PARTIAL job (Live text) is the transmission so far,
// decoded once, quickly: no fallbacks, no retries. The process runs at nice 10
// (Windows: the below-normal priority class), below the receiver.

#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "whisper.h"
#include "voice2text_wire.hpp"
#ifdef _WIN32
#include "utils/process.hpp"   // kproc::lower_own_priority (no setpriority / nice on Windows)
#endif

namespace {

namespace wire = v2t::wire;
constexpr int SR = wire::SAMPLE_RATE;

struct Opts {
    std::string language = "auto";   // multilingual models only ("auto" = detect)
    std::string prompt;              // vocabulary / style hint (callsigns, place names)
    int threads = 2;
    bool vad = true;
    float vad_threshold = 0.5f;
    bool translate = false;          // multilingual models: output English
    int beam = 0;                    // 0 = greedy, else beam search width
};

struct Result {
    float conf = 0, no_speech = 0;
    int speech_ms = -1;
    std::string lang, text, dropped;
};

whisper_context* g_ctx = nullptr;
// The decoding state (encoder output, cross-attention cache, ...), one at a
// time (two at once took base.en 254 -> 322 MB, small.en 657 -> 789 MB). A
// full-window FINAL decode needs a PRISTINE one - no sized encode since it was
// made (whisper.cpp zeroes its caches), so the padding its decoder reads past
// the 1500 frames is zeros, as before (encoder_window() says why): when the
// current one isn't, a new one (4-24 ms here; the only swap - so a busy mix of
// long and short transmissions costs one per long final). Everything else uses
// whatever state there is: a sized decode reads only frames it wrote itself,
// and a full-window PARTIAL may read a sized decode's leftovers in that
// padding - a preview, redone a second later
whisper_state* g_state = nullptr;
bool g_pristine = false;

whisper_state* state_for(bool pristine) {
    if (pristine && g_state && !g_pristine) {
        whisper_free_state(g_state);
        g_state = nullptr;
    }
    if (!g_state) {
        g_state = whisper_init_state(g_ctx);
        g_pristine = true;
    }
    return g_state;
}
whisper_vad_context* g_vad = nullptr;
bool g_multilingual = false;

void quiet_log(enum ggml_log_level level, const char* text, void*) {
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "[BLANK_AUDIO]", "(static)", "*beep*", music notes: what whisper writes
// for non-speech - not words anyone said
std::string strip_annotations(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        const char close = c == '[' ? ']' : c == '(' ? ')' : c == '*' ? '*' : 0;
        if (close) {
            const size_t e = s.find(close, i + 1);
            if (e != std::string::npos && e - i < 60) { i = e + 1; continue; }
        }
        if (!s.compare(i, 3, "\xE2\x99\xAA") || !s.compare(i, 3, "\xE2\x99\xAB")) { i += 3; continue; }   // ♪ ♫
        o += c;
        i++;
    }
    // squeeze the spaces left behind
    std::string r;
    for (char c : o)
        if (!(c == ' ' && (r.empty() || r.back() == ' '))) r += c;
    return trim(r);
}

// letters and digits, lower case, single spaces (UTF-8 letters kept as they are)
std::string lower_alnum(const std::string& s) {
    std::string o;
    for (unsigned char c : s)
        if (c >= 0x80 || std::isalnum(c)) o += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
        else if (c == ' ' && !o.empty() && o.back() != ' ') o += ' ';
    return trim(o);
}

// whisper's loops on noise / clipped audio: the same sentence or word 4+
// times in a row is kept once ("Okay. Okay. Okay. Okay." -> "Okay.")
std::string collapse_repeats(const std::string& s) {
    // units: sentences (ending at . ! ? before a space - not "146.52"), then words
    auto split = [](const std::string& t, bool sentences) {
        std::vector<std::string> u;
        std::string cur;
        for (size_t i = 0; i < t.size(); i++) {
            const char c = t[i];
            cur += c;
            const bool end = sentences ? ((c == '.' || c == '!' || c == '?') && (i + 1 == t.size() || t[i + 1] == ' '))
                                       : c == ' ';
            if (end) {
                if (!trim(cur).empty()) u.push_back(trim(cur));
                cur.clear();
            }
        }
        if (!trim(cur).empty()) u.push_back(trim(cur));
        return u;
    };
    std::string text = s;
    for (bool sentences : {true, false}) {
        auto u = split(text, sentences);
        std::vector<std::string> out;
        for (size_t i = 0; i < u.size();) {
            size_t j = i + 1;
            const std::string k = lower_alnum(u[i]);
            while (j < u.size() && !k.empty() && lower_alnum(u[j]) == k) j++;
            out.push_back(u[i]);
            if (j - i < 4)
                for (size_t m = i + 1; m < j; m++) out.push_back(u[m]);   // up to 3 times can be real speech
            i = j;
        }
        text.clear();
        for (const auto& x : out) text += (text.empty() ? "" : " ") + x;
    }
    return text;
}

// Phrases whisper produces from nothing (its training data's subtitle
// credits) - never on a radio channel
bool is_credit_phrase(const std::string& norm) {
    static const char* const P[] = {"thanks for watching", "thank you for watching", "please subscribe",
                                    "subtitles by", "subtitled by", "captions by", "amaraorg", "amara org",
                                    "transcribed by", "translated by", "see you in the next video",
                                    "like and subscribe"};
    for (const char* p : P)
        if (norm.find(p) != std::string::npos) return true;
    return false;
}
// ... and ones that are also real speech: only dropped when whisper itself
// doubts there was speech
bool is_filler_phrase(const std::string& norm) {
    static const char* const P[] = {"thank you", "thanks", "you", "bye", "okay", "oh", "so", "the end", "hmm"};
    for (const char* p : P)
        if (norm == p) return true;
    return false;
}

size_t word_count(const std::string& s) {
    size_t n = 0;
    bool in = false;
    for (unsigned char c : s) {
        const bool w = c >= 0x80 || std::isalnum(c);
        if (w && !in) n++;
        in = w;
    }
    return n;
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> w;
    std::string cur;
    for (char c : s) {
        if (c == ' ') {
            if (!cur.empty()) w.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

std::string join_words(const std::vector<std::string>& w, size_t from, size_t to) {
    std::string s;
    for (size_t i = from; i < to && i < w.size(); i++) s += (s.empty() ? "" : " ") + w[i];
    return s;
}

// A Live text decode often ends where the token cap stopped a loop ("ask not
// ask not ask not ... ask"): a run of 4+ copies of the same phrase (up to 8
// words; compared as lower_alnum) is kept once, and a cut-off copy after it
// at the very end is dropped. (Finals keep collapse_repeats alone: a loop
// there is decoded again with the full window)
std::string collapse_phrase_loops(const std::string& s) {
    const std::vector<std::string> w = split_words(s);
    std::vector<std::string> k(w.size());
    for (size_t i = 0; i < w.size(); i++) k[i] = lower_alnum(w[i]);
    std::vector<std::string> out;
    for (size_t i = 0; i < w.size();) {
        size_t unit = 0, copies = 0;
        for (size_t n = 1; n <= 8 && i + 2 * n <= w.size(); n++) {
            size_t c = 1;
            while (i + (c + 1) * n <= w.size() && std::equal(k.begin() + static_cast<long>(i + c * n),
                                                             k.begin() + static_cast<long>(i + (c + 1) * n),
                                                             k.begin() + static_cast<long>(i)))
                c++;
            if (c >= 4 && c * n > copies * unit) {
                unit = n;
                copies = c;
            }
        }
        if (!unit) {
            out.push_back(w[i++]);
            continue;
        }
        out.insert(out.end(), w.begin() + static_cast<long>(i), w.begin() + static_cast<long>(i + unit));
        i += copies * unit;
        size_t m = 0;
        while (i + m < w.size() && m < unit && k[i + m] == k[i - unit + m]) m++;
        if (i + m == w.size()) break;   // the rest is the start of one more copy
    }
    return join_words(out, 0, out.size());
}

// The user's vocabulary as whisper's prompt. Whisper reads a prompt as the
// text said BEFORE, so a bare list ("Houston, Roger, over") read like an
// unfinished sentence and greedy decoding often ended after one word ("the" -
// seen on 2 of 4 test transmissions): a list becomes a finished sentence
// ("Glossary: Houston, Roger, over."), which decoded every one in full
std::string prompt_text(const std::string& p) {
    const std::string t = trim(p);
    if (t.empty()) return t;
    const char last = t.back();
    if (last == '.' || last == '!' || last == '?') return t;
    return "Glossary: " + t + ".";
}

struct Decoded {
    bool ok = false;
    bool capped = false;      // the token cap ended it (a loop, most likely)
    std::string text, lang;   // text as whisper wrote it (not cleaned yet)
    float conf = 0, no_speech = 0;
};

// whisper runs of the current job, for --file's report
struct Runs {
    int sized = 0, full = 0, guard = 0;
} g_runs;

// The encoder window (audio_ctx, in frames of 20 ms). whisper encodes 30 s
// (1500 frames) whatever the input's length: for 5-13 s transmissions the
// encoder was ~90 % of a job. Measured 2026-11-20 (14 radio clips + JFK,
// base.en / small.en):
// - the input's length EXACTLY: whisper often never wrote end-of-text - decodes
//   ran to its token limit, then 5 temperature fallbacks: slower than 30 s;
// - with 1 s more fewer loops, but texts depended on the clips decoded BEFORE:
//   with flash attention (whisper.cpp's default) the decoder reads the cross-
//   attention cache in steps of 256 frames, unmasked - past audio_ctx it read
//   what an earlier encode left there (zeros in a fresh state);
// - so: 1 s more, rounded UP to a multiple of 256 (5.1, 10.2, 15.4, 20.5 or
//   25.6 s): every frame read is this input's own - the same text in any order
//   and with any thread count -, and no decode looped (the extra silence helps
//   whisper end). CPU s per job, 2 threads: base.en 2.91 -> 0.93, small.en
//   10.8 -> 3.65; word error rate the same or lower. A full window decodes in
//   a state no sized encode has written into (state_for), so its padding is
//   zeros, as before.
// The window must cover the whole input: whisper seeks on by 30 s whatever
// audio_ctx is and drops the rest silently. 0 = the full window (inputs from
// 24.6 s on)
int encoder_window(size_t samples) {
    const int need = static_cast<int>(std::ceil(static_cast<double>(samples) * 50.0 / SR)) + 50;
    const int win = (need + 255) / 256 * 256;
    return win < whisper_model_n_audio_ctx(g_ctx) ? win : 0;
}

// The loop guard: at most this many text tokens per decode - 6 a second of
// input (English; 10 for a multilingual model) + 16, far more than anyone says
// (fast radio speech: ~4). With 1 s more but not rounded, base.en looped on 2
// of the 15 clips (~4-7 s of CPU each); rounded, no English model's decode
// reached the cap (2026-11-20: 8 model / thread setups, 2-3 runs each), the
// multilingual base's once (a clip it took for German) - the cap is the backstop
// At most whisper_n_text_ctx/2 - 5 (219): whisper.cpp's decode loop stops at
// n_text_ctx/2 - 4 tokens (a decoder still going is marked failed there and the
// temperature fallbacks run instead) - a cap above that never fires (a
// multilingual model's from 20.3 s of input on)
int token_cap(size_t samples) {
    const double s = static_cast<double>(samples) / SR;
    const int cap = static_cast<int>(std::ceil((g_multilingual ? 10.0 : 6.0) * s)) + 16;
    return std::min(cap, whisper_n_text_ctx(g_ctx) / 2 - 5);
}

struct TokenCap {
    int max = 0;
    std::atomic<bool> hit{false};
};

// whisper_full's logits filter (called per decoder, from several threads with
// best_of / beam search): at the cap, end-of-text is forced - and from then on
// at once, in whisper's temperature fallbacks too (they would only loop again)
void cap_filter(whisper_context* ctx, whisper_state*, const whisper_token_data*, int n_tokens, float* logits, void* user) {
    auto* c = static_cast<TokenCap*>(user);
    if (n_tokens < c->max && !c->hit.load(std::memory_order_relaxed)) return;
    c->hit.store(true, std::memory_order_relaxed);
    const int nv = whisper_n_vocab(ctx);
    for (int i = 0; i < nv; i++) logits[i] = -INFINITY;
    logits[whisper_token_eot(ctx)] = 0.0f;
}

// One whisper_full. audio_ctx 0 = the full window; cap 0 = no token cap.
// partial: greedy, no temperature fallback (Live text redoes it a second later).
// one_decoder: greedy with best_of 1 (the retry after whisper.cpp ran out of
// memory for several decoders)
Decoded decode_once(const std::vector<float>& speech, const Opts& o, const std::string& prompt, int audio_ctx, int cap,
                    bool partial, bool one_decoder = false) {
    Decoded d;
    const bool beam = o.beam > 1 && !partial && !one_decoder;
    whisper_full_params p = whisper_full_default_params(beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    p.n_threads = std::clamp(o.threads, 1, 8);
    p.no_context = true;         // utterances are independent (other radios, other channels)
    p.no_timestamps = true;
    p.print_special = p.print_progress = p.print_realtime = p.print_timestamps = false;
    p.suppress_nst = true;       // no "(music)" / "[laughs]" tokens
    const std::string lang = g_multilingual ? (o.language.empty() ? "auto" : o.language) : "en";
    p.language = lang.c_str();
    p.translate = g_multilingual && o.translate;
    if (!prompt.empty()) {
        p.initial_prompt = prompt.c_str();
        p.carry_initial_prompt = true;   // every 30 s window of a long transmission
    }
    if (beam) p.beam_search.beam_size = std::clamp(o.beam, 2, 8);
    if (one_decoder) p.greedy.best_of = 1;
    // (a multilingual model's language detection, "auto", encodes with the
    // window of this STATE's previous decode - whisper.cpp applies audio_ctx
    // only after it: 5.1-30 s of this input, depending on the job before)
    p.audio_ctx = audio_ctx;
    if (partial) p.temperature_inc = 0.0f;
    TokenCap tc;
    if (cap > 0) {
        tc.max = cap;
        p.logits_filter_callback = cap_filter;
        p.logits_filter_callback_user_data = &tc;
    }
    (audio_ctx > 0 ? g_runs.sized : g_runs.full)++;
    whisper_state* st = state_for(audio_ctx == 0 && !partial);
    if (!st) return d;
    const int rc = whisper_full_with_state(g_ctx, st, p, speech.data(), static_cast<int>(speech.size()));
    if (rc == -7) {
        // no memory for the self-attention cache of several decoders (beam
        // search, best_of in a temperature fallback): whisper.cpp has FREED the
        // state itself - forget it (a new one next time) and decode once more
        // with one decoder
        g_state = nullptr;
        if (one_decoder) return d;
        static auto said = std::chrono::steady_clock::time_point();
        const auto now = std::chrono::steady_clock::now();
        if (said == std::chrono::steady_clock::time_point() || now - said > std::chrono::minutes(10)) {
            said = now;
            fprintf(stderr, "out of memory for %s - decoding with one decoder\n",
                    beam ? "beam search" : "a temperature fallback");
        }
        return decode_once(speech, o, prompt, audio_ctx, cap, partial, true);
    }
    if (audio_ctx > 0) g_pristine = false;
    if (rc != 0) return d;
    d.ok = true;
    d.capped = tc.hit.load();
    const int nseg = whisper_full_n_segments_from_state(st);
    const whisper_token eot = whisper_token_eot(g_ctx);
    double psum = 0;
    int pn = 0;
    for (int i = 0; i < nseg; i++) {
        const float nsp = whisper_full_get_segment_no_speech_prob_from_state(st, i);
        double seg_p = 0, seg_lp = 0;
        int seg_n = 0;
        const int nt = whisper_full_n_tokens_from_state(st, i);
        for (int j = 0; j < nt; j++) {
            const whisper_token_data td = whisper_full_get_token_data_from_state(st, i, j);
            if (td.id >= eot) continue;   // special tokens
            seg_p += td.p;
            seg_lp += td.plog;
            seg_n++;
        }
        // whisper's own silence rule: likely no speech AND an unsure decode
        if (seg_n > 0 && nsp > 0.6f && seg_lp / seg_n < -1.0) continue;
        psum += seg_p;
        pn += seg_n;
        d.no_speech = std::max(d.no_speech, nsp);
        d.text += whisper_full_get_segment_text_from_state(st, i);
    }
    d.conf = pn ? static_cast<float>(psum / pn) : 0.0f;
    if (g_multilingual) {
        const int id = whisper_full_lang_id_from_state(st);
        const char* ls = id >= 0 ? whisper_lang_str(id) : nullptr;
        d.lang = ls ? ls : "";
    } else {
        d.lang = "en";
    }
    return d;
}

// A final transcript's decode: the sized window with the token cap; when the
// cap ends it (a loop), the same input again with the full window and no cap -
// exactly the decode of before 2026-11-20. So the cap can never cut a
// transcript short: a cap too low only costs CPU
Decoded decode(const std::vector<float>& speech, const Opts& o, const std::string& prompt) {
    const int win = encoder_window(speech.size());
    if (win > 0) {
        Decoded d = decode_once(speech, o, prompt, win, token_cap(speech.size()), false);
        if (d.ok && !d.capped) return d;
        g_runs.guard++;
    }
    return decode_once(speech, o, prompt, 0, 0, false);
}

// Silero VAD: only the speech parts of `pcm` -> `speech`, each padded, joined
// by 150 ms of silence (`joins`: the middle of each gap). false = less than
// 200 ms of speech (r->dropped "no speech": whisper need not run). Without the
// VAD model (or when it fails) `speech` is everything. Then padded to whisper's
// ~1 s minimum
bool speech_parts(const std::vector<float>& pcm, const Opts& o, std::vector<float>* speech, std::vector<size_t>* joins,
                  Result* r) {
    speech->clear();
    joins->clear();
    if (g_vad && o.vad) {
        whisper_vad_params vp = whisper_vad_default_params();
        vp.threshold = std::clamp(o.vad_threshold, 0.05f, 0.95f);
        vp.min_speech_duration_ms = 200;
        vp.min_silence_duration_ms = 300;
        vp.speech_pad_ms = 200;    // radio speech starts / ends abruptly: keep the word edges
        whisper_vad_segments* seg = whisper_vad_segments_from_samples(g_vad, vp, pcm.data(), static_cast<int>(pcm.size()));
        if (seg) {
            const int ns = whisper_vad_segments_n_segments(seg);
            r->speech_ms = 0;
            const std::vector<float> gap(SR * 15 / 100, 0.0f);   // 150 ms between the parts
            for (int i = 0; i < ns; i++) {
                // centiseconds
                long a = std::lround(whisper_vad_segments_get_segment_t0(seg, i) * SR / 100.0);
                long b = std::lround(whisper_vad_segments_get_segment_t1(seg, i) * SR / 100.0);
                a = std::clamp<long>(a, 0, static_cast<long>(pcm.size()));
                b = std::clamp<long>(b, a, static_cast<long>(pcm.size()));
                if (b <= a) continue;
                if (!speech->empty()) {
                    joins->push_back(speech->size() + gap.size() / 2);
                    speech->insert(speech->end(), gap.begin(), gap.end());
                }
                speech->insert(speech->end(), pcm.begin() + a, pcm.begin() + b);
                r->speech_ms += static_cast<int>((b - a) * 1000 / SR);
            }
            whisper_vad_free_segments(seg);
            if (r->speech_ms < 200) {
                r->dropped = "no speech";
                return false;
            }
        } else {
            *speech = pcm;   // VAD failed: whisper gets everything
        }
    } else {
        *speech = pcm;
    }
    // whisper wants at least ~1 s
    if (speech->size() < static_cast<size_t>(SR * 11 / 10)) speech->resize(SR * 11 / 10, 0.0f);
    return true;
}

// the decoded text -> r: cleaned, or dropped (and why)
void keep_text(const Decoded& d, Result* r) {
    r->conf = d.conf;
    r->no_speech = d.no_speech;
    r->lang = d.lang;
    const std::string text = collapse_repeats(strip_annotations(trim(d.text)));
    const std::string norm = lower_alnum(text);
    if (norm.empty()) r->dropped = "no words";
    else if (is_credit_phrase(norm)) r->dropped = "noise phrase";
    else if (is_filler_phrase(norm) && (r->no_speech > 0.35f || r->conf < 0.5f)) r->dropped = "noise phrase";
    else r->text = text;
}

Result transcribe(const std::vector<float>& pcm, const Opts& o) {
    Result r;
    g_runs = Runs();
    std::vector<float> speech;
    std::vector<size_t> joins;   // where in `speech` a VAD part ends (the middle of the gap after it)
    if (!speech_parts(pcm, o, &speech, &joins, &r)) return r;

    const std::string prompt = prompt_text(o.prompt);
    Decoded d = decode(speech, o, prompt);
    if (!d.ok) {
        r.dropped = "whisper error";
        return r;
    }
    // even so, a prompt can end a decode early: far fewer words than the
    // speech holds -> once more without it, the longer text wins
    const double speech_s = (r.speech_ms >= 0 ? r.speech_ms : static_cast<double>(speech.size()) * 1000 / SR) / 1000;
    if (!prompt.empty() && speech_s >= 2.5 && word_count(d.text) < speech_s * 0.4) {
        Decoded d2 = decode(speech, o, "");
        if (d2.ok && word_count(d2.text) > word_count(d.text)) d = std::move(d2);
    }
    // whisper sometimes ends a decode after its first sentence ("Roger the
    // register." of 11 s of speech; timestamps don't make it go on): such a
    // short result -> the speech's two halves (split between VAD parts, at
    // the gap nearest the middle) decoded on their own, the longer text wins
    if (speech_s >= 4.0 && word_count(d.text) < speech_s * 0.6 && !joins.empty()) {
        size_t at = joins[0];
        for (size_t j : joins)
            if (std::labs(static_cast<long>(j) - static_cast<long>(speech.size() / 2)) <
                std::labs(static_cast<long>(at) - static_cast<long>(speech.size() / 2)))
                at = j;
        if (at >= static_cast<size_t>(SR) && speech.size() - at >= static_cast<size_t>(SR)) {
            const std::string pr = word_count(d.text) < speech_s * 0.4 ? "" : prompt;
            Decoded a = decode(std::vector<float>(speech.begin(), speech.begin() + static_cast<long>(at)), o, pr);
            Decoded b = decode(std::vector<float>(speech.begin() + static_cast<long>(at), speech.end()), o, pr);
            if (a.ok && b.ok) {
                const size_t na = word_count(a.text), nb = word_count(b.text);
                if (na + nb > word_count(d.text)) {
                    d.text = trim(a.text) + " " + trim(b.text);
                    d.conf = static_cast<float>((a.conf * na + b.conf * nb) / static_cast<double>(na + nb));
                    d.no_speech = std::max(a.no_speech, b.no_speech);
                }
            }
        }
    }
    keep_text(d, &r);
    return r;
}

// Live text: the transmission so far, decoded ONCE - the same VAD, prompt,
// language and clean-up as a final transcript, but greedy, no temperature
// fallback, no retries, and the token cap simply ends it (without these
// guards whisper looped / invented words on half-spoken audio: 7-10 s
// decodes; with them 0.2-0.6 s - 2026-11-20). The final transcript at the
// transmission's end is made as before and replaces it
Result transcribe_partial(const std::vector<float>& pcm, const Opts& o) {
    Result r;
    g_runs = Runs();
    std::vector<float> speech;
    std::vector<size_t> joins;
    if (!speech_parts(pcm, o, &speech, &joins, &r)) return r;
    const Decoded d = decode_once(speech, o, prompt_text(o.prompt), encoder_window(speech.size()),
                                  token_cap(speech.size()), true);
    if (!d.ok) {
        r.dropped = "whisper error";
        return r;
    }
    if (d.capped) g_runs.guard++;
    Decoded dd = d;
    dd.text = collapse_phrase_loops(d.text);
    keep_text(dd, &r);
    return r;
}

// --- live: jobs on stdin ----------------------------------------------------

int g_out = 1;

void write_all(const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n > 0) {
        ssize_t w = ::write(g_out, c, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            _exit(0);   // kraken_doa went away
        }
        c += w;
        n -= static_cast<size_t>(w);
    }
}

void send(uint32_t type, const std::string& payload) {
    const uint32_t hdr[2] = {type, static_cast<uint32_t>(payload.size())};
    write_all(hdr, 8);
    if (!payload.empty()) write_all(payload.data(), payload.size());
}

bool read_all(int fd, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n > 0) {
        ssize_t r = ::read(fd, c, n);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        c += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

void set_option(Opts& o, const std::string& kv) {
    const size_t eq = kv.find('=');
    const std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
    if (k == "language") o.language = v.substr(0, 16);
    else if (k == "prompt") o.prompt = v.substr(0, 600);
    else if (k == "threads") o.threads = std::clamp(atoi(v.c_str()), 1, 8);
    else if (k == "vad") o.vad = v == "1";
    else if (k == "vad_threshold") o.vad_threshold = static_cast<float>(std::clamp(atof(v.c_str()), 0.05, 0.95));
    else if (k == "translate") o.translate = v == "1";
    else if (k == "beam") o.beam = std::clamp(atoi(v.c_str()), 0, 8);
}

int serve(Opts o, const std::string& model) {
    // stdout carries the protocol: keep a private copy, point fd 1 at stderr
    // so nothing printed by the libraries can corrupt it
    g_out = dup(1);
    dup2(2, 1);
#ifdef _WIN32
    const char* const seps = "/\\";   // a Windows path may use either (KRAKEN_WHISPER_MODELS=C:\models)
#else
    const char* const seps = "/";
#endif
    std::string ready = "model=" + model.substr(model.find_last_of(seps) + 1);
    ready += std::string("\0type=", 6) + whisper_model_type_readable(g_ctx);
    ready += std::string("\0multilingual=", 14) + (g_multilingual ? "1" : "0");
    ready += std::string("\0vad=", 5) + (g_vad ? "1" : "0");
    ready += std::string("\0version=", 9) + whisper_version();
    // the system line the sidebar box shows: which processor whisper runs on first
#ifdef KRAKEN_WHISPER_GPU
    ready += std::string("\0system=", 8) + "GPU on | " + whisper_print_system_info();
#else
    ready += std::string("\0system=", 8) + whisper_print_system_info();
#endif
    ready += std::string("\0live=1", 7);   // takes PARTIAL (Live text)
    send(wire::READY, ready);
    std::vector<char> buf;
    std::vector<float> pcm;
    for (;;) {
        uint32_t hdr[2];
        if (!read_all(0, hdr, 8)) return 0;
        if (hdr[1] > wire::MAX_PAYLOAD) {
            send(wire::ERROR, "message too long");
            return 2;
        }
        buf.resize(hdr[1]);
        if (hdr[1] && !read_all(0, buf.data(), hdr[1])) return 0;
        if (hdr[0] == wire::OPTION) {
            set_option(o, std::string(buf.begin(), buf.end()));
        } else if ((hdr[0] == wire::JOB || hdr[0] == wire::PARTIAL) && buf.size() >= 4) {
            uint32_t id;
            memcpy(&id, buf.data(), 4);
            const size_t n = (buf.size() - 4) / 2;
            pcm.resize(n);
            const char* src = buf.data() + 4;
            for (size_t i = 0; i < n; i++) {
                int16_t v;
                memcpy(&v, src + 2 * i, 2);
                pcm[i] = v / 32768.0f;
            }
            const bool partial = hdr[0] == wire::PARTIAL;
            const auto t0 = std::chrono::steady_clock::now();
            const Result r = partial ? transcribe_partial(pcm, o) : transcribe(pcm, o);
            const uint32_t ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::steady_clock::now() - t0).count());
            const int32_t sp = r.speech_ms;
            if (partial) {
                std::string out(12, '\0');
                memcpy(&out[0], &id, 4);
                memcpy(&out[4], &ms, 4);
                memcpy(&out[8], &sp, 4);
                out += r.text;   // "" = no speech yet / nothing kept
                send(wire::PARTIAL_RESULT, out);
                continue;
            }
            std::string out(20, '\0');
            memcpy(&out[0], &id, 4);
            memcpy(&out[4], &r.conf, 4);
            memcpy(&out[8], &r.no_speech, 4);
            memcpy(&out[12], &ms, 4);
            memcpy(&out[16], &sp, 4);
            out += r.lang;
            out += '\0';
            out += r.text;
            out += '\0';
            out += r.dropped;
            send(wire::RESULT, out);
        }
    }
}

// --- offline: a WAV file ----------------------------------------------------

bool read_wav(const std::string& path, std::vector<float>* out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { *err = "cannot open " + path; return false; }
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) { *err = "not a WAV file"; return false; }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        uint32_t len;
        memcpy(&len, d.data() + p + 4, 4);
        const char* body = d.data() + p + 8;
        if (!memcmp(d.data() + p, "fmt ", 4) && len >= 16) {
            memcpy(&fmt, body, 2);
            memcpy(&ch, body + 2, 2);
            memcpy(&rate, body + 4, 4);
            memcpy(&bits, body + 14, 2);
        } else if (!memcmp(d.data() + p, "data", 4)) {
            if (fmt != 1 || bits != 16 || ch < 1) { *err = "need 16-bit PCM"; return false; }
            const size_t frames = std::min<size_t>(len, d.size() - (p + 8)) / (2u * ch);
            std::vector<float> mono(frames);
            for (size_t i = 0; i < frames; i++) {
                int16_t v;
                memcpy(&v, body + 2 * ch * i, 2);   // first channel
                mono[i] = v / 32768.0f;
            }
            // other rates: linear resampling (tests only - kraken_doa sends 16 kHz)
            if (rate != static_cast<uint32_t>(SR) && rate > 0) {
                const size_t n = static_cast<size_t>(frames * static_cast<double>(SR) / rate);
                out->resize(n);
                for (size_t i = 0; i < n; i++) {
                    const double x = i * static_cast<double>(rate) / SR;
                    const size_t k = static_cast<size_t>(x);
                    const double fr = x - k;
                    const float a = mono[std::min(k, frames - 1)], b = mono[std::min(k + 1, frames - 1)];
                    (*out)[i] = static_cast<float>(a + (b - a) * fr);
                }
            } else {
                *out = std::move(mono);
            }
            return true;
        }
        p += 8 + len + (len & 1);
    }
    *err = "no audio in the WAV file";
    return false;
}

// CPU seconds of this process so far (-1: not measured here)
double cpu_seconds() {
#ifdef _WIN32
    return -1;
#else
    rusage u{};
    if (getrusage(RUSAGE_SELF, &u) != 0) return -1;
    return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
           static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
#endif
}

// the job's whisper runs: "1 sized", "1 sized + 1 full (loop guard 1)"
std::string runs_text() {
    std::string s;
    if (g_runs.sized) s += std::to_string(g_runs.sized) + " sized";
    if (g_runs.full) s += (s.empty() ? "" : " + ") + std::to_string(g_runs.full) + " full";
    if (g_runs.guard) s += " (loop guard " + std::to_string(g_runs.guard) + ")";
    return s.empty() ? "none" : s;
}

void print_result(const char* what, const std::vector<float>& pcm, const Result& r, double wall, double cpu) {
    printf("%s: audio %.1f s, speech %d ms, processed in %.2f s", what, pcm.size() / static_cast<double>(SR), r.speech_ms,
           wall);
    if (cpu >= 0) printf(" (CPU %.2f s)", cpu);
    printf(", windows %s, lang %s, conf %.2f, no_speech %.2f%s%s\n", runs_text().c_str(), r.lang.c_str(), r.conf,
           r.no_speech, r.dropped.empty() ? "" : ", dropped: ", r.dropped.c_str());
    printf("%s\n", r.text.c_str());
}

// --stream: Live text as kraken_doa schedules it (voice2text.cpp) - the first
// partial decode after 1 s of audio, then one every STEP (or as soon as the
// previous one is done, when it took longer), each of all the audio so far, as
// if the audio arrived in real time; the words two partials in a row agree on
// are confirmed (LocalAgreement-2: compared without case / punctuation, only
// ever more), the rest of the latest is pending. At the end the final
// transcript, as a JOB makes it
int stream_sim(const std::vector<float>& pcm, const Opts& o, int step_ms) {
    const double step = std::max(100, step_ms) / 1000.0, total = pcm.size() / static_cast<double>(SR);
    printf("Live text: %.1f s of audio, a partial every %.1f s - time, decode, [confirmed] pending\n", total, step);
    std::vector<std::string> prev, confirmed;
    double free_at = 0, last = -1, sum = 0, worst = 0;
    int n = 0;
    const double c0 = cpu_seconds();
    for (;;) {
        const double t = last < 0 ? std::max(1.0, step) : std::max(last + std::max(step, 0.5), free_at);
        if (t > total) break;
        const std::vector<float> buf(pcm.begin(), pcm.begin() + static_cast<long>(t * SR));
        const auto t0 = std::chrono::steady_clock::now();
        const Result r = transcribe_partial(buf, o);
        const double w = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const std::vector<std::string> cur = split_words(r.text);
        size_t l = 0;
        while (l < prev.size() && l < cur.size() && lower_alnum(prev[l]) == lower_alnum(cur[l])) l++;
        bool agrees = cur.size() >= confirmed.size();
        for (size_t i = 0; agrees && i < confirmed.size(); i++) agrees = lower_alnum(confirmed[i]) == lower_alnum(cur[i]);
        if (agrees && l > confirmed.size())
            confirmed.insert(confirmed.end(), cur.begin() + static_cast<long>(confirmed.size()),
                             cur.begin() + static_cast<long>(l));
        printf("  %5.1f s  %4.2f s  [%s] %s%s%s\n", t, w, join_words(confirmed, 0, confirmed.size()).c_str(),
               join_words(cur, confirmed.size(), cur.size()).c_str(), r.dropped.empty() ? "" : "  - ", r.dropped.c_str());
        prev = cur;
        last = t;
        free_at = t + w;
        sum += w;
        worst = std::max(worst, w);
        n++;
    }
    const double c1 = cpu_seconds();
    const auto t0 = std::chrono::steady_clock::now();
    const Result f = transcribe(pcm, o);
    const double fw = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double c2 = cpu_seconds();
    print_result("final", pcm, f, fw, c2 >= 0 ? c2 - c1 : -1);
    // confirmed words the final changed: word edits between the confirmed
    // words and as many of the final's
    const std::vector<std::string> fin = split_words(f.text);
    const size_t m = confirmed.size(), k = std::min(m, fin.size());
    std::vector<size_t> dp(k + 1);
    for (size_t j = 0; j <= k; j++) dp[j] = j;
    for (size_t i = 1; i <= m; i++) {
        size_t diag = dp[0];
        dp[0] = i;
        for (size_t j = 1; j <= k; j++) {
            const size_t up = dp[j];
            const size_t same = lower_alnum(confirmed[i - 1]) == lower_alnum(fin[j - 1]) ? 0 : 1;
            dp[j] = std::min({dp[j] + 1, dp[j - 1] + 1, diag + same});
            diag = up;
        }
    }
    printf("partials %d, %.2f s mean, %.2f s max", n, n ? sum / n : 0.0, worst);
    if (c0 >= 0) printf(", CPU %.2f s = %.2f cores while the audio lasted", c1 - c0, (c1 - c0) / std::max(0.1, total));
    printf("; %zu words confirmed before the final (of its %zu), %zu edits by it\n", m, fin.size(), dp[k]);
    return 0;
}

void usage() {
    fprintf(stderr,
            "usage: kraken_whisper --serve --model M.bin [--vad V.bin] [--threads N] [--language L]\n"
            "       kraken_whisper --file clip.wav --model M.bin [--vad V.bin] [--threads N] [--language L]\n"
            "                      [--prompt \"words\"] [--beam N] [--no-vad] [--translate] [--partial | --stream STEP_MS]\n"
            "       kraken_whisper --quantize IN.bin OUT.bin q8_0|q5_0|q5_1\n");
}

}  // namespace

// quantize.cpp
int quantize_model(const std::string& in_path, const std::string& out_path, const std::string& type);

int main(int argc, char** argv) {
    std::string model, vad, file;
    bool serve_mode = false, partial = false;
    int stream_ms = 0;
    Opts o;
    if (argc >= 2 && std::string(argv[1]) == "--quantize") {
        if (argc != 5) { usage(); return 1; }
#ifdef _WIN32
        kproc::lower_own_priority();
#else
        setpriority(PRIO_PROCESS, 0, 10);   // below the receiver, as the transcribing
#endif
        return quantize_model(argv[2], argv[3], argv[4]);
    }
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); exit(1); }
            return argv[++i];
        };
        if (a == "--serve") serve_mode = true;
        else if (a == "--model") model = next();
        else if (a == "--vad") vad = next();
        else if (a == "--file") file = next();
        else if (a == "--threads") o.threads = std::clamp(atoi(next().c_str()), 1, 8);
        else if (a == "--language") o.language = next();
        else if (a == "--prompt") o.prompt = next();
        else if (a == "--beam") o.beam = std::clamp(atoi(next().c_str()), 0, 8);
        else if (a == "--no-vad") o.vad = false;
        else if (a == "--translate") o.translate = true;
        else if (a == "--partial") partial = true;
        else if (a == "--stream") stream_ms = std::max(1, atoi(next().c_str()));
        else { usage(); return 1; }
    }
    if (model.empty() || (!serve_mode && file.empty()) || (partial && stream_ms)) { usage(); return 1; }
    // below the receiver: the Pi's cores are shared with the DSP pipeline
#ifdef _WIN32
    kproc::lower_own_priority();   // BELOW_NORMAL_PRIORITY_CLASS
#else
    setpriority(PRIO_PROCESS, 0, 10);
#endif
    whisper_log_set(quiet_log, nullptr);

    whisper_context_params cp = whisper_context_default_params();
    // the GPU only when the build asked for it (setup.sh WHISPER_GPU -> CMake
    // KRAKEN_WHISPER_GPU: Metal on Apple Silicon by default); CPU otherwise.
    // nice 10 above doesn't reach the GPU - but there whisper leaves the CPU
    // cores to the receiver's DSP
#ifdef KRAKEN_WHISPER_GPU
    cp.use_gpu = true;
#else
    cp.use_gpu = false;
#endif
    cp.flash_attn = true;   // (whisper.cpp's default; encoder_window() relies on how it reads)
    g_ctx = whisper_init_from_file_with_params_no_state(model.c_str(), cp);
    if (g_ctx) {
        if (!state_for(false)) {
            whisper_free(g_ctx);
            g_ctx = nullptr;
        }
    }
    if (!g_ctx) {
        const std::string e = "cannot load the model " + model;
        if (serve_mode) {
            g_out = dup(1);
            send(wire::ERROR, e);
        }
        fprintf(stderr, "%s\n", e.c_str());
        return 2;
    }
    g_multilingual = whisper_is_multilingual(g_ctx) != 0;
    if (!vad.empty()) {
        whisper_vad_context_params vcp = whisper_vad_default_context_params();
        vcp.n_threads = 1;
        vcp.use_gpu = false;
        g_vad = whisper_vad_init_from_file_with_params(vad.c_str(), vcp);
        if (!g_vad) fprintf(stderr, "cannot load the VAD model %s - transcribing without it\n", vad.c_str());
    }

    int rc = 0;
    if (serve_mode) {
        rc = serve(o, model);
    } else {
        std::vector<float> pcm;
        std::string err;
        if (!read_wav(file, &pcm, &err)) {
            fprintf(stderr, "%s\n", err.c_str());
            rc = 1;
        } else if (stream_ms > 0) {
            rc = stream_sim(pcm, o, stream_ms);
        } else {
            const double c0 = cpu_seconds();
            const auto t0 = std::chrono::steady_clock::now();
            const Result r = partial ? transcribe_partial(pcm, o) : transcribe(pcm, o);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double c1 = cpu_seconds();
            print_result(partial ? "partial" : "final", pcm, r, s, c0 >= 0 && c1 >= 0 ? c1 - c0 : -1);
        }
    }
    if (g_vad) whisper_vad_free(g_vad);
    if (g_state) whisper_free_state(g_state);
    whisper_free(g_ctx);
    return rc;
}
