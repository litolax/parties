#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace parties::client {

class SoundPlayer {
public:
    enum class Effect {
        Mute,
        Unmute,
        Deafen,
        Undeafen,
        JoinChannel,
        LeaveChannel,
        UserJoined,
        UserLeft,
        Count_
    };

    SoundPlayer();

    // Queue a sound to play (call from any thread)
    void play(Effect effect);

    // Mix active sounds into output buffer (call from audio thread)
    void mix_output(float* output, int frame_count);

private:
    static constexpr int kSampleRate = 48000;
    static constexpr int kMaxPlaying = 8;

    struct Sound {
        std::vector<float> samples;
    };

    std::array<Sound, static_cast<size_t>(Effect::Count_)> sounds_;

    struct PlayingSound {
        std::atomic<int> effect{-1};     // -1 = inactive
        std::atomic<size_t> position{0};
    };

    std::array<PlayingSound, kMaxPlaying> playing_;
};

} // namespace parties::client
