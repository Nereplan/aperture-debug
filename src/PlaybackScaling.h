#pragma once

namespace Aperture::PlaybackScaling {
struct Option {
    const char* name;
    const char* value;
};

// Match the linear sampler used by VLC's default D3D11 output. mpv's native
// defaults add higher-order filters and linear-light intermediate surfaces.
// Custom MPV settings are applied later and can select those filters explicitly.
inline constexpr Option kLinear[] = {
    {"scale", "bilinear"},
    {"dscale", "bilinear"},
    {"cscale", "bilinear"},
    {"linear-downscaling", "no"},
    {"correct-downscaling", "no"},
    {"sigmoid-upscaling", "no"},
};
}
