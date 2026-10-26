#pragma once

#include <cstddef>

namespace Aperture {

enum class IconType {
    Play, Pause, Next, Prev, VolumeHigh, VolumeLow, VolumeMuted, Subtitles, SettingsGear, PictureInPicture, Pin, Fullscreen, ExitFullscreen,
    FolderPlaylist, HistoryClock, MediaFilm, Plus, Trash, CloseX, Checkmark, Refresh, ChevronRight, ChevronLeft,
    ChevronDown, SpeedGauge, AspectFrame, LoopRepeat, AutoplayNext, DragHandle, Search, ApertureLogo,
    MicPodcast, RadioWaves, Link, Star, ChevronUp
};

struct IconPath { IconType icon; const char* path; float viewBox; bool fill; bool overlaySupported; };

inline constexpr IconPath kIconPaths[] = {
    {IconType::Play, "M8 5v14l11-7z", 24.0f, true, true},
    {IconType::Pause, "M6 19h4V5H6v14zm8-14v14h4V5h-4z", 24.0f, true, true},
    {IconType::Next, "M6 18l8.5-6L6 6v12zM16 6v12h2V6h-2z", 24.0f, true, true},
    {IconType::Prev, "M6 6h2v12H6zm3.5 6l8.5 6V6z", 24.0f, true, true},
    {IconType::VolumeHigh, "M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02zM14 3.23v2.06c2.89.86 5 3.54 5 6.71s-2.11 5.85-5 6.71v2.06c4.01-.91 7-4.49 7-8.77s-2.99-7.86-7-8.77z", 24.0f, true, true},
    {IconType::VolumeLow, "M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02z", 24.0f, true, true},
    {IconType::VolumeMuted, "M16.5 12c0-1.77-1.02-3.29-2.5-4.03v2.21l2.45 2.45c.03-.2.05-.41.05-.63zm2.5 0c0 .94-.2 1.82-.54 2.64l1.51 1.51C20.63 14.91 21 13.5 21 12c0-4.28-2.99-7.86-7-8.77v2.06c2.89.86 5 3.54 5 6.71zM4.27 3L3 4.27 7.73 9H3v6h4l5 5v-6.73l4.25 4.25c-.67.52-1.42.93-2.25 1.18v2.06c1.38-.31 2.63-.95 3.69-1.81L19.73 21 21 19.73l-9-9L4.27 3zM12 4L9.91 6.09 12 8.18V4z", 24.0f, true, true},
    {IconType::Subtitles, "M19 4H5c-1.11 0-2 .9-2 2v12c0 1.1.89 2 2 2h14c1.1 0 2-.9 2-2V6c0-1.1-.9-2-2-2zm-8 7H9.5v-.5h-2v3h2V13H11v1c0 .55-.45 1-1 1H7c-.55 0-1-.45-1-1v-4c0-.55.45-1 1-1h3c.55 0 1 .45 1 1v1zm7 0h-1.5v-.5h-2v3h2V13H18v1c0 .55-.45 1-1 1h-3c-.55 0-1-.45-1-1v-4c0-.55.45-1 1-1h3c.55 0 1 .45 1 1v1z", 24.0f, true, true},
    {IconType::SettingsGear, "M19.43 12.98c.04-.32.07-.64.07-.98s-.03-.66-.07-.98l2.11-1.65c.19-.15.24-.42.12-.64l-2-3.46c-.12-.22-.39-.3-.61-.22l-2.49 1c-.52-.4-1.08-.73-1.69-.98l-.38-2.65C14.46 2.18 14.25 2 14 2h-4c-.25 0-.46.18-.49.42l-.38 2.65c-.61.25-1.17.59-1.69.98l-2.49-1c-.23-.09-.49 0-.61.22l-2 3.46c-.13.22-.07.49.12.64l2.11 1.65c-.04.32-.07.65-.07.98s.03.66.07.98l-2.11 1.65c-.19.15-.24.42-.12.64l2 3.46c.12.22.39.3.61.22l2.49-1c.03.24.42.49.42h4c.25 0 .46-.18.49-.42l.38-2.65c.61-.25 1.17-.59 1.69-.98l2.49 1c.23.09.49 0 .61-.22l2-3.46c.12-.22.07-.49-.12-.64l-2.11-1.65zM12 15.5c-1.93 0-3.5-1.57-3.5-3.5s1.57-3.5 3.5-3.5 3.5 1.57 3.5 3.5-1.57 3.5-3.5 3.5z", 24.0f, true, true},
    {IconType::PictureInPicture, "M19 11h-8v6h8v-6zm4 8V4.98C23 3.88 22.1 3 21 3H3c-1.1 0-2 .88-2 1.98V19c0 1.1.9 2 2 2h18c1.1 0 2-.9 2-2zm-2 .02H3V4.97h18v14.05z", 24.0f, true, true},
    {IconType::Pin, "M16 9V4l1 0c.55 0 1-.45 1-1s-.45-1-1-1H7c-.55 0-1 .45-1 1s.45 1 1 1l1 0v5c0 1.66-1.34 3-3 3v2h5.97v7l1 1 1-1v-7H19v-2c-1.66 0-3-1.34-3-3z", 24.0f, true, true},
    {IconType::Fullscreen, "M7 14H5v5h5v-2H7v-3zm-2-4h2V7h3V5H5v5zm12 7h-3v2h5v-5h-2v3zM14 5v2h3v3h2V5h-5z", 24.0f, true, true},
    {IconType::ExitFullscreen, "M5 16h3v3h2v-5H5v2zm3-8H5v2h5V5H8v3zm6 11h2v-3h3v-2h-5v5zm2-11V5h-2v5h5V8h-3z", 24.0f, true, true},
    {IconType::FolderPlaylist, "M2 3.5h12M2 7h12M2 10.5h8M12 9.5l3 2-3 2v-4z", 16.0f, false, false},
    {IconType::HistoryClock, "M8 3 a5.5 5.5 0 1 0 0.001 0 M8 5.5V8.5L10 10", 16.0f, false, false},
    {IconType::MediaFilm, "M1.5 2.5 h13 v11 h-13 z", 16.0f, false, false},
    {IconType::Plus, "M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z", 24.0f, true, true},
    {IconType::Trash, "M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z", 24.0f, true, true},
    {IconType::CloseX, "M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z", 24.0f, true, true},
    {IconType::Checkmark, "M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z", 24.0f, true, true},
    {IconType::Refresh, "M12 4V1L8 5l4 4V6c3.31 0 6 2.69 6 6 0 1.01-.25 1.97-.7 2.8l1.46 1.46C19.54 15.03 20 13.57 20 12c0-4.42-3.58-8-8-8zm0 14c-3.31 0-6-2.69-6-6 0-1.01.25-1.97.7-2.8L5.24 7.74C4.46 8.97 4 10.43 4 12c0 4.42 3.58 8 8 8v3l4-4-4-4v3z", 24.0f, true, false},
    {IconType::ChevronRight, "M10 6L8.59 7.41 13.17 12l-4.58 4.59L10 18l6-6z", 24.0f, true, true},
    {IconType::ChevronLeft, "M15.41 7.41L14 6l-6 6 6 6 1.41-1.41L10.83 12z", 24.0f, true, true},
    {IconType::ChevronDown, "M7.41 8.59L12 13.17l4.59-4.58L18 10l-6 6-6-6 1.41-1.41z", 24.0f, true, true},
    {IconType::SpeedGauge, "M13 2.05v3.03c3.39.49 6 3.39 6 6.92 0 .9-.18 1.75-.48 2.54l2.6 1.53c.56-1.24.88-2.62.88-4.07 0-5.18-3.95-9.45-9-9.95zM12 19c-3.87 0-7-3.13-7-7 0-3.53 2.61-6.43 6-6.92V2.05c-5.05.5-9 4.76-9 9.95 0 5.52 4.47 10 9.99 10 3.31 0 6.24-1.61 8.01-4.09l-2.45-1.45C16.14 17.76 14.23 19 12 19z", 24.0f, true, true},
    {IconType::AspectFrame, "M21 3H3c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h18c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm0 16H3V5h18v14zM5 15h2v2H5v-2zm0-4h2v2H5v-2zm0-4h2v2H5V7zm14 8h-2v2h2v-2zm0-4h-2v2h2v-2zm0-4h-2v2h2V7z", 24.0f, true, true},
    {IconType::LoopRepeat, "M7 7h10v3l4-4-4-4v3H5v6h2V7zm10 10H7v-3l-4 4 4 4v-3h12v-6h-2v4z", 24.0f, true, true},
    {IconType::AutoplayNext, "M6 18l8.5-6L6 6v12zM16 6v12h2V6h-2z", 24.0f, true, true},
    {IconType::DragHandle, nullptr, 24.0f, true, false},
    {IconType::Search, "M15.5 14h-.79l-.28-.27C15.41 12.59 16 11.11 16 9.5 16 5.91 13.09 3 9.5 3S3 5.91 3 9.5 5.91 16 9.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14z", 24.0f, true, false},
    {IconType::ApertureLogo, "M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm0 18c-4.41 0-8-3.59-8-8s3.59-8 8-8 8 3.59 8 8-3.59 8-8 8z", 24.0f, true, false},
    {IconType::MicPodcast, "M12 14c1.66 0 3-1.34 3-3V5c0-1.66-1.34-3-3-3S9 3.34 9 5v6c0 1.66 1.34 3 3 3zm5.3-3c0 3-2.54 5.1-5.3 5.1S6.7 14 6.7 11H5c0 3.41 2.72 6.23 6 6.72V21h2v-3.28c3.28-.49 6-3.31 6-6.72h-1.7z", 24.0f, true, true},
    {IconType::RadioWaves, "M8 10.3 m-1.3 0 a1.3 1.3 0 1 0 2.6 0 a1.3 1.3 0 1 0 -2.6 0 M5.4 7.9 a4.4 4.4 0 0 1 5.2 0 M3.4 5.4 a7.4 7.4 0 0 1 9.2 0", 16.0f, false, false},
    {IconType::Link, "M3.9 12c0-1.71 1.39-3.1 3.1-3.1h4V7H7c-2.76 0-5 2.24-5 5s2.24 5 5 5h4v-1.9H7c-1.71 0-3.1-1.39-3.1-3.1zM8 13h8v-2H8v2zm9-6h-4v1.9h4c1.71 0 3.1 1.39 3.1 3.1s-1.39 3.1-3.1 3.1h-4V17h4c2.76 0 5-2.24 5-5s-2.24-5-5-5z", 24.0f, true, true},
    {IconType::Star, "M12 17.27L18.18 21l-1.64-7.03L22 9.24l-7.19-.61L12 2 9.19 8.63 2 9.24l5.46 4.73L5.82 21z", 24.0f, true, true},
    {IconType::ChevronUp, "M7.41 15.41L12 9.83l4.59 5.58L18 14l-6-6-6 6z", 24.0f, true, true},
};
static_assert([] {
    for (std::size_t i = 0; i < sizeof(kIconPaths) / sizeof(*kIconPaths); ++i) {
        if (static_cast<std::size_t>(kIconPaths[i].icon) != i) return false;
    }
    return true;
}(), "Icon paths must follow IconType order");

inline const IconPath* FindIconPath(IconType icon) {
    const auto index = static_cast<std::size_t>(icon);
    return index < sizeof(kIconPaths) / sizeof(*kIconPaths) ? &kIconPaths[index] : nullptr;
}

} // namespace Aperture
