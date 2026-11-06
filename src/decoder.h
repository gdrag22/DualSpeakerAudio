#pragma once
#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include "audio_buffer.h"

class Decoder {
    unsigned channels_, bytes_;
    bool floating_;
    std::vector<audio::Stereo> weights_;
public:
    explicit Decoder(const WAVEFORMATEX* format) : channels_(format->nChannels), bytes_(format->wBitsPerSample / 8), floating_(false) {
        WORD tag = format->wFormatTag;
        DWORD mask = 0;
        if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
            if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
            mask = extended->dwChannelMask;
        }
        floating_ = tag == WAVE_FORMAT_IEEE_FLOAT;
        if (!channels_ || channels_ > 32 || format->nSamplesPerSec < 8000 || format->nSamplesPerSec > 384000 ||
            format->nBlockAlign != channels_ * bytes_ ||
            !((floating_ && (bytes_ == 4 || bytes_ == 8)) ||
              (tag == WAVE_FORMAT_PCM && bytes_ >= 1 && bytes_ <= 4)))
            throw std::runtime_error("Unsupported primary mix format (expected PCM or IEEE float).");
        weights_.resize(channels_);
        if (channels_ == 1) { weights_[0] = {1, 1}; return; }
        if (!mask && channels_ <= 8) {
            // Standard WAVE channel orders for formats without an explicit mask.
            static constexpr DWORD defaults[] = {0, 0, 3, 7, 0x33, 0x37, 0x3f, 0x13f, 0x63f};
            mask = defaults[channels_];
        }
        if (!mask) throw std::runtime_error("Multichannel primary format has no usable speaker mask.");
        unsigned index = 0;
        for (unsigned bit = 0; bit < 32 && index < channels_; ++bit) {
            const DWORD speaker = DWORD{1} << bit;
            if (!(mask & speaker)) continue;
            audio::Stereo weight{};
            switch (speaker) {
            case SPEAKER_FRONT_LEFT: weight = {1, 0}; break;
            case SPEAKER_FRONT_RIGHT: weight = {0, 1}; break;
            case SPEAKER_LOW_FREQUENCY: break; // No unfiltered LFE in the stereo fold-down.
            case SPEAKER_BACK_LEFT: case SPEAKER_SIDE_LEFT: case SPEAKER_FRONT_LEFT_OF_CENTER:
            case SPEAKER_TOP_FRONT_LEFT: case SPEAKER_TOP_BACK_LEFT: weight = {0.7071f, 0}; break;
            case SPEAKER_BACK_RIGHT: case SPEAKER_SIDE_RIGHT: case SPEAKER_FRONT_RIGHT_OF_CENTER:
            case SPEAKER_TOP_FRONT_RIGHT: case SPEAKER_TOP_BACK_RIGHT: weight = {0, 0.7071f}; break;
            default: weight = {0.7071f, 0.7071f}; break;
            }
            weights_[index++] = weight;
        }
        if (index != channels_) throw std::runtime_error("Primary channel mask does not match its channel count.");
        float left = 0, right = 0;
        for (auto w : weights_) { left += w.left; right += w.right; }
        const float scale = 1 / std::max(1.0f, std::max(left, right));
        for (auto& w : weights_) { w.left *= scale; w.right *= scale; }
    }
    float sample(const BYTE* data) const {
        if (floating_) {
            double value;
            if (bytes_ == 4) { float f; std::memcpy(&f, data, 4); value = f; }
            else std::memcpy(&value, data, 8);
            return std::isfinite(value) ? static_cast<float>(std::clamp(value, -1.0, 1.0)) : 0;
        }
        if (bytes_ == 1) return (static_cast<int>(*data) - 128) / 128.0f;
        uint32_t bits = 0;
        for (unsigned i = 0; i < bytes_; ++i) bits |= static_cast<uint32_t>(data[i]) << (8 * i);
        // Integer samples with fewer valid bits are left-aligned in the container.
        const unsigned width = bytes_ * 8;
        const int64_t signedValue = (bits & (uint32_t{1} << (width - 1))) ?
            static_cast<int64_t>(bits) - (int64_t{1} << width) : bits;
        return static_cast<float>(signedValue / static_cast<double>(int64_t{1} << (width - 1)));
    }
    void append(audio::Buffer& buffer, const BYTE* data, UINT32 frames, bool silent) const {
        for (UINT32 i = 0; i < frames; ++i) {
            audio::Stereo frame{};
            if (!silent) {
                for (unsigned ch = 0; ch < channels_; ++ch) {
                    const float value = sample(data + (static_cast<size_t>(i) * channels_ + ch) * bytes_);
                    frame.left += value * weights_[ch].left;
                    frame.right += value * weights_[ch].right;
                }
            }
            buffer.push(frame);
        }
    }
};

