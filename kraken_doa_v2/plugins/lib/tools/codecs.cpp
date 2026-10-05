// Which digital voice codecs the decoder plugins can use, as the plugins
// themselves find them (same MbeLib / TetraCodec code, same environment:
// KRAKEN_MBELIB, KRAKEN_TETRA_CODEC_DIR, PATH). kraken_doa runs it for the
// sidebar's "Digital Decoders" box. Prints one flat JSON object.

#include <cstdio>
#include <string>

#include "dig_vocoder.hpp"

static std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) o += ' ';
        else o += static_cast<char>(c);
    }
    return o;
}

int main() {
    const auto& mbe = dig::MbeLib::instance();
    printf("{\"imbe_ok\":true,\"imbe\":\"built in\",\"ambe_ok\":%s,\"ambe\":\"%s\",\"acelp_ok\":%s,\"acelp\":\"%s\"}\n",
           mbe.available() ? "true" : "false", jesc(mbe.status()).c_str(),
           dig::TetraCodec::available() ? "true" : "false", jesc(dig::TetraCodec::status()).c_str());
    return 0;
}
