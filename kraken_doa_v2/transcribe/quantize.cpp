// kraken_whisper --quantize IN.bin OUT.bin q8_0: a whisper.cpp model (f16 / f32
// weights) -> 8-bit (q8_0) or 5-bit (q5_0, q5_1) weights. Part of the helper
// binary, not a program of its own: Docker and the Windows package ship only
// transcribe/build/kraken_whisper[.exe], and kraken_doa / setup.sh make the
// derived catalogue models (base.en-q8_0, small.en-q8_0 - whisper.cpp
// publishes no .en q8_0 files) with it.
//
// Ported from whisper.cpp v1.9.4: examples/quantize/quantize.cpp
// (whisper_model_quantize) and examples/common-ggml.cpp
// (ggml_common_quantize_0). Changes: every read is checked (a truncated or
// foreign file fails instead of writing garbage), tensor shapes are bounded
// (no overflow, no allocation beyond ~4 MB, nothing larger than the rest of
// the file), the vocabulary is copied without the fixed 129-byte buffer, an
// already quantized input and OUT = IN (by file identity) are refused, the
// tensor names must be exactly whisper's set (a file cut at a tensor boundary
// looks complete), OUT is flushed to the disk and removed on any failure, no
// per-tensor output.
// The tensors quantized, the ones skipped and the bytes written are the
// example's: the result is byte for byte what its whisper-quantize writes.
//
// Original code: MIT License
//
// Copyright (c) 2023-2026 The ggml authors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>   // GetFileInformationByHandle: same_file
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <set>
#include <string>
#include <vector>

#include "ggml.h"

namespace {

struct In {
    FILE* f = nullptr;
    bool ok = true;
    int64_t pos = 0, size = -1;   // bytes read; the file's size (-1: unknown)
    template <class T>
    bool get(T* v, size_t n = 1) {
        if (ok && n && fread(v, sizeof(T), n, f) != n) ok = false;
        if (ok) pos += static_cast<int64_t>(n * sizeof(T));
        return ok;
    }
    // can `bytes` more be there?
    bool has(int64_t bytes) const { return size < 0 || bytes <= size - pos; }
};

struct Out {
    FILE* f = nullptr;
    bool ok = true;
    void put(const void* p, size_t n) {
        if (ok && n && fwrite(p, 1, n, f) != n) ok = false;
    }
    template <class T>
    void put(const T& v) { put(&v, sizeof(T)); }
};

int64_t file_size(const std::string& path) {
#ifdef _WIN32
    struct _stati64 st;   // (st_size of the plain stat is 32-bit there)
    return _stati64(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
#endif
}

// OUT is IN (also through a link): writing it would destroy the model
bool same_file(const std::string& a, const std::string& b) {
#ifdef _WIN32
    // volume serial + file index (paths can't tell: hard links, 8.3 names,
    // subst / mapped drives, paths over MAX_PATH). Narrow paths are UTF-8
    // under the manifest's active code page, as fopen's
    auto id = [](const std::string& p, BY_HANDLE_FILE_INFORMATION* fi) {
        HANDLE h = CreateFileA(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const bool ok = GetFileInformationByHandle(h, fi) != 0;
        CloseHandle(h);
        return ok;
    };
    BY_HANDLE_FILE_INFORMATION fa, fb;
    if (!id(a, &fa) || !id(b, &fb)) return false;   // OUT not there yet
    return fa.dwVolumeSerialNumber == fb.dwVolumeSerialNumber && fa.nFileIndexHigh == fb.nFileIndexHigh &&
           fa.nFileIndexLow == fb.nFileIndexLow;
#else
    struct stat sa, sb;
    if (stat(a.c_str(), &sa) != 0 || stat(b.c_str(), &sb) != 0) return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

// the example's tensor rule: every 2D weight, except these
bool keep_as_is(const std::string& name) {
    return name == "encoder.conv1.bias" || name == "encoder.conv2.bias" || name == "encoder.positional_embedding" ||
           name == "decoder.positional_embedding";
}

// The tensors of a whisper model (whisper.cpp 1.9.4's src/whisper-arch.h), each
// once: a file cut at a tensor boundary reads as complete, whisper.cpp would
// refuse it later. (Loading the result in whisper to check it needs the whole
// model's memory - and aborts, leaving OUT, when there isn't enough)
std::set<std::string> whisper_tensors(int n_audio_layer, int n_text_layer) {
    std::set<std::string> t = {"encoder.positional_embedding", "encoder.conv1.weight", "encoder.conv1.bias",
                               "encoder.conv2.weight", "encoder.conv2.bias", "encoder.ln_post.weight",
                               "encoder.ln_post.bias", "decoder.positional_embedding",
                               "decoder.token_embedding.weight", "decoder.ln.weight", "decoder.ln.bias"};
    static const char* const ATTN[] = {"attn_ln.weight", "attn_ln.bias", "attn.query.weight", "attn.query.bias",
                                       "attn.key.weight", "attn.value.weight", "attn.value.bias", "attn.out.weight",
                                       "attn.out.bias"};
    static const char* const MLP[] = {"mlp_ln.weight", "mlp_ln.bias", "mlp.0.weight", "mlp.0.bias", "mlp.2.weight",
                                      "mlp.2.bias"};
    for (int l = 0; l < n_audio_layer; l++) {
        const std::string b = "encoder.blocks." + std::to_string(l) + ".";
        for (const char* x : ATTN) t.insert(b + x);
        for (const char* x : MLP) t.insert(b + x);
    }
    for (int l = 0; l < n_text_layer; l++) {
        const std::string b = "decoder.blocks." + std::to_string(l) + ".";
        for (const char* x : ATTN) t.insert(b + x);
        for (const char* x : ATTN) t.insert(b + "cross_" + x);
        for (const char* x : MLP) t.insert(b + x);
    }
    return t;
}

bool quantize(const std::string& in_path, const std::string& out_path, ggml_ftype ftype, ggml_type qtype, std::string* err) {
    In in;
    in.f = fopen(in_path.c_str(), "rb");
    if (!in.f) { *err = "cannot open " + in_path + ": " + strerror(errno); return false; }
    struct Closer { FILE*& f; ~Closer() { if (f) fclose(f); } } close_in{in.f};
    in.size = file_size(in_path);

    // magic + hparams (n_vocab, n_audio_ctx, n_audio_state, n_audio_head, n_audio_layer,
    // n_text_ctx, n_text_state, n_text_head, n_text_layer, n_mels, ftype)
    uint32_t magic = 0;
    int32_t hp[11];
    if (!in.get(&magic) || magic != GGML_FILE_MAGIC) { *err = in_path + " is not a whisper.cpp model (ggml-*.bin)"; return false; }
    if (!in.get(hp, 11)) { *err = in_path + " is truncated"; return false; }
    if (hp[0] <= 0 || (hp[9] != 80 && hp[9] != 128)) {   // n_vocab, n_mels (the VAD model has neither)
        *err = in_path + " is not a whisper.cpp speech model";
        return false;
    }
    const int32_t ftype_src = hp[10] % GGML_QNT_VERSION_FACTOR;
    if (ftype_src != GGML_FTYPE_ALL_F32 && ftype_src != GGML_FTYPE_MOSTLY_F16) {
        *err = in_path + " is already quantized (type " + std::to_string(ftype_src) + ") - quantize the f16 model";
        return false;
    }
    const int32_t ftype_dst = GGML_QNT_VERSION * GGML_QNT_VERSION_FACTOR + ftype;
    if (hp[4] < 1 || hp[4] > 64 || hp[8] < 1 || hp[8] > 64) {   // n_audio_layer, n_text_layer
        *err = in_path + " is not a whisper.cpp speech model";
        return false;
    }
    std::set<std::string> want = whisper_tensors(hp[4], hp[8]);
    const size_t n_want = want.size();

    // mel filters
    int32_t n_mel = 0, n_fft = 0;
    if (!in.get(&n_mel) || !in.get(&n_fft) || n_mel <= 0 || n_fft <= 0 || n_mel > 1024 || n_fft > 4096) {
        *err = in_path + ": bad mel filter header";
        return false;
    }
    std::vector<float> filters(static_cast<size_t>(n_mel) * n_fft);
    if (!in.get(filters.data(), filters.size())) { *err = in_path + " is truncated"; return false; }

    // vocabulary
    int32_t n_vocab = 0;
    if (!in.get(&n_vocab) || n_vocab < 0 || n_vocab > 1000000) { *err = in_path + ": bad vocabulary"; return false; }
    std::vector<char> vocab;   // copied as it is
    for (int32_t i = 0; i < n_vocab; i++) {
        uint32_t len = 0;
        if (!in.get(&len) || len > 4096 || !in.has(len)) { *err = in_path + ": bad vocabulary"; return false; }
        const size_t at = vocab.size();
        vocab.resize(at + 4 + len);
        memcpy(&vocab[at], &len, 4);
        if (!in.get(vocab.data() + at + 4, len)) { *err = in_path + " is truncated"; return false; }
    }

    if (same_file(in_path, out_path)) { *err = "OUT is the input file"; return false; }
    Out out;
    out.f = fopen(out_path.c_str(), "wb");
    if (!out.f) { *err = "cannot write " + out_path + ": " + strerror(errno); return false; }
    bool done = false;
    // (also when an exception unwinds through here: quantize_model catches it)
    struct Cleanup {
        Out& o;
        const std::string& path;
        bool& done;
        ~Cleanup() {
            if (o.f) fclose(o.f);
            if (!done) remove(path.c_str());
        }
    } cleanup{out, out_path, done};

    out.put(magic);
    out.put(hp, sizeof(int32_t) * 10);
    out.put(ftype_dst);
    out.put(n_mel);
    out.put(n_fft);
    out.put(filters.data(), filters.size() * sizeof(float));
    out.put(n_vocab);
    out.put(vocab.data(), vocab.size());

    // tensors to the end of the file: n_dims, name length, type, dims, name, data
    std::vector<uint8_t> raw;
    std::vector<ggml_fp16_t> f16;
    std::vector<float> f32, work;
    int n_tensors = 0, n_quantized = 0;
    for (;;) {
        int32_t n_dims = 0, length = 0, ttype = 0;
        const size_t got = fread(&n_dims, 1, 4, in.f);
        if (got == 0 && feof(in.f) && !ferror(in.f)) break;   // the end, at a tensor boundary
        if (got != 4) { *err = in_path + " is truncated (or has stray bytes at its end)"; return false; }
        in.pos += 4;
        if (!in.get(&length) || !in.get(&ttype)) { *err = in_path + " is truncated"; return false; }
        if (n_dims < 1 || n_dims > 4 || length <= 0 || length > 256 ||
            (ttype != GGML_TYPE_F32 && ttype != GGML_TYPE_F16)) {
            *err = in_path + ": unexpected tensor (dims " + std::to_string(n_dims) + ", type " + std::to_string(ttype) + ")";
            return false;
        }
        // whisper's largest: the token embedding, n_text_state (<= 1280) x n_vocab
        // (~52k); a row (ne[0]) <= 65536 keeps every buffer below ~4 MB, and the
        // whole tensor must fit in what is left of the file
        int32_t ne[4] = {1, 1, 1, 1};
        int64_t nelements = 1;
        for (int i = 0; i < n_dims; i++) {
            if (!in.get(&ne[i])) { *err = in_path + " is truncated"; return false; }
            if (ne[i] <= 0 || ne[i] > (i == 0 ? 65536 : (1 << 24)) || nelements > (int64_t(1) << 31) / ne[i]) {
                *err = in_path + ": bad tensor shape";
                return false;
            }
            nelements *= ne[i];
        }
        const size_t esize = ttype == GGML_TYPE_F32 ? 4 : 2;
        if (!in.has(length + nelements * static_cast<int64_t>(esize))) { *err = in_path + " is truncated"; return false; }
        std::string name(static_cast<size_t>(length), '\0');
        if (!in.get(&name[0], name.size())) { *err = in_path + " is truncated"; return false; }
        if (!want.erase(name)) {
            *err = in_path + ": tensor '" + name.substr(0, 64) + "' is not a whisper one, or there twice";
            return false;
        }

        const bool q = n_dims == 2 && !keep_as_is(name) && ne[0] % ggml_blck_size(qtype) == 0;
        const int32_t otype = q ? static_cast<int32_t>(qtype) : ttype;
        out.put(n_dims);
        out.put(length);
        out.put(otype);
        out.put(ne, sizeof(int32_t) * static_cast<size_t>(n_dims));
        out.put(name.data(), name.size());
        // the data, ~1M weights at a time (small.en's token embedding alone is
        // 40M: ~0.4 GB at once - too much beside a running receiver on a Pi).
        // Blocks never cross a row, so this is what one call over all rows writes
        const size_t per_row = static_cast<size_t>(ne[0]);
        const size_t rows = static_cast<size_t>(nelements) / per_row;
        const size_t chunk_rows = std::max<size_t>(1, (size_t(1) << 20) / per_row);
        for (size_t r0 = 0; r0 < rows && out.ok; r0 += chunk_rows) {
            const size_t nr = std::min(chunk_rows, rows - r0), n = nr * per_row;
            if (q) {
                f32.resize(n);
                if (ttype == GGML_TYPE_F16) {
                    f16.resize(n);
                    if (!in.get(f16.data(), n)) { *err = in_path + " is truncated"; return false; }
                    for (size_t i = 0; i < n; i++) f32[i] = ggml_fp16_to_fp32(f16[i]);
                } else if (!in.get(f32.data(), n)) {
                    *err = in_path + " is truncated";
                    return false;
                }
                work.resize(n);   // q8_0 / q5_x take less than 4 bytes per weight
                const size_t sz = ggml_quantize_chunk(qtype, f32.data(), work.data(), 0, static_cast<int64_t>(nr),
                                                      static_cast<int64_t>(per_row), nullptr);
                out.put(work.data(), sz);
            } else {
                raw.resize(n * esize);
                if (!in.get(raw.data(), raw.size())) { *err = in_path + " is truncated"; return false; }
                out.put(raw.data(), raw.size());
            }
        }
        if (q) n_quantized++;
        n_tensors++;
        if (!out.ok) break;
    }
    if (!want.empty()) {
        *err = in_path + " is incomplete: " + std::to_string(n_tensors) + " of " + std::to_string(n_want) + " tensors";
        return false;
    }
    if (out.ok && fflush(out.f) != 0) out.ok = false;
    if (out.ok && fsync(fileno(out.f)) != 0) out.ok = false;
    if (out.ok && fclose(out.f) != 0) out.ok = false;
    out.f = nullptr;
    if (!out.ok) { *err = "cannot write " + out_path + " (disk full?)"; return false; }
    done = true;
    printf("quantized %d of %d tensors -> %s\n", n_quantized, n_tensors, out_path.c_str());
    return true;
}

}  // namespace

// 0 = written; else the message is on stderr and OUT is not there
int quantize_model(const std::string& in_path, const std::string& out_path, const std::string& type) {
    ggml_ftype ftype;
    ggml_type qtype;
    if (type == "q8_0") { ftype = GGML_FTYPE_MOSTLY_Q8_0; qtype = GGML_TYPE_Q8_0; }
    else if (type == "q5_0") { ftype = GGML_FTYPE_MOSTLY_Q5_0; qtype = GGML_TYPE_Q5_0; }
    else if (type == "q5_1") { ftype = GGML_FTYPE_MOSTLY_Q5_1; qtype = GGML_TYPE_Q5_1; }
    else {
        fprintf(stderr, "kraken_whisper --quantize: unknown type '%s' (q8_0, q5_0 or q5_1)\n", type.c_str());
        return 1;
    }
    // ggml's fp16 tables
    {
        ggml_init_params gp = {0, nullptr, false};
        ggml_free(ggml_init(gp));
    }
    std::string err;
    bool ok = false;
    try {
        ok = quantize(in_path, out_path, ftype, qtype, &err);
    } catch (const std::exception& e) {   // bad_alloc: OUT is removed while unwinding
        err = std::string("failed: ") + e.what();
    } catch (...) {
        err = "failed";
    }
    if (!ok) {
        fprintf(stderr, "kraken_whisper --quantize: %s\n", err.c_str());
        return 2;
    }
    return 0;
}
