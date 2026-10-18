#pragma once

namespace Aperture::PlaybackScaling {
struct Option {
    const char* name;
    const char* value;
};

// Explicit comparison profile, not a claim of equivalent picture quality.
inline constexpr Option kLinear[] = {
    {"scale", "bilinear"},
    {"dscale", "bilinear"},
    {"cscale", "bilinear"},
    {"linear-downscaling", "no"},
    {"correct-downscaling", "no"},
    {"sigmoid-upscaling", "no"},
};

// Keep the native luma scalers and downsampling antialiasing. Avoid the
// additional linear-light/sigmoid passes; chroma uses hardware interpolation.
// Custom MPV settings are applied later and can restore the full native profile.
inline constexpr Option kBalanced[] = {
    {"scale", "lanczos"},
    {"dscale", "hermite"},
    {"cscale", "bilinear"},
    {"linear-downscaling", "no"},
    {"correct-downscaling", "yes"},
    {"sigmoid-upscaling", "no"},
};
}
