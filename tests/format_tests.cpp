#include "../src/decoder.h"
#include <array>
#include <iostream>
#include <limits>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static WAVEFORMATEX format(WORD tag, WORD bits, WORD channels = 2) {
    WAVEFORMATEX f{};
    f.wFormatTag = tag;
    f.wBitsPerSample = bits;
    f.nChannels = channels;
    f.nSamplesPerSec = 48000;
    f.nBlockAlign = channels * bits / 8;
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    return f;
}
int main() {
    try {
        for (WORD bits : {8, 16, 24, 32}) {
            const auto f = format(WAVE_FORMAT_PCM, bits);
            Decoder decoder(&f);
            BYTE zero[4]{};
            BYTE negative[4]{};
            BYTE positive[4] = {255, 255, 255, 255};
            negative[bits / 8 - 1] = 128;
            positive[bits / 8 - 1] = 127;
            if (bits == 8) { zero[0] = 128; negative[0] = 0; positive[0] = 255; }
            require(decoder.sample(zero) == 0, "PCM silence conversion");
            require(decoder.sample(negative) == -1, "PCM negative full scale conversion");
            require(decoder.sample(positive) > 0.99f, "PCM positive full scale conversion");
        }
        for (WORD bits : {32, 64}) {
            const auto f = format(WAVE_FORMAT_IEEE_FLOAT, bits);
            Decoder decoder(&f);
            const float f32 = 0.25f, nan32 = std::numeric_limits<float>::quiet_NaN();
            const double f64 = 0.25, nan64 = std::numeric_limits<double>::quiet_NaN();
            require(decoder.sample(reinterpret_cast<const BYTE*>(bits == 32 ? static_cast<const void*>(&f32) : &f64)) == 0.25f,
                    "float sample conversion");
            require(decoder.sample(reinterpret_cast<const BYTE*>(bits == 32 ? static_cast<const void*>(&nan32) : &nan64)) == 0,
                    "NaN must not reach output");
        }
        const auto mono = format(WAVE_FORMAT_IEEE_FLOAT, 32, 1);
        Decoder monoDecoder(&mono);
        std::array<float, 16> monoData;
        monoData.fill(0.25f);
        audio::Buffer fifo(64, 8);
        float out[8]{};
        monoDecoder.append(fifo, reinterpret_cast<const BYTE*>(monoData.data()), 16, false);
        fifo.render(out, 4, 48000);
        require(out[0] == 0.25f && out[1] == 0.25f, "mono was not duplicated");
        fifo.clear();
        monoDecoder.append(fifo, nullptr, 16, true);
        fifo.render(out, 4, 48000);
        for (float sample : out) require(sample == 0, "silent packet accessed null buffer or produced sound");

        WAVEFORMATEXTENSIBLE surround{};
        surround.Format = format(WAVE_FORMAT_EXTENSIBLE, 32, 6);
        surround.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        surround.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        surround.Samples.wValidBitsPerSample = 32;
        surround.dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
        Decoder surroundDecoder(&surround.Format);
        std::array<float, 96> frames{};
        for (size_t i = 0; i < 16; ++i) frames[i * 6 + 2] = 1; // Center only.
        fifo.clear();
        surroundDecoder.append(fifo, reinterpret_cast<const BYTE*>(frames.data()), 16, false);
        fifo.render(out, 4, 48000);
        require(out[0] > 0 && out[0] == out[1], "5.1 center fold-down failed");
        frames.fill(0);
        for (size_t i = 0; i < 16; ++i) frames[i * 6 + 3] = 1; // LFE only.
        fifo.clear();
        surroundDecoder.append(fifo, reinterpret_cast<const BYTE*>(frames.data()), 16, false);
        fifo.render(out, 4, 48000);
        for (float sample : out) require(sample == 0, "LFE should be omitted");
        surround.dwChannelMask = SPEAKER_FRONT_LEFT;
        bool rejected = false;
        try { Decoder invalid(&surround.Format); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "invalid channel mask accepted");
        std::cout << "PCM, float, NaN, silence, mono, 5.1 fold-down, and invalid format tests passed.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
