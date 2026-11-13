#pragma once

#include "Common.h"
#include "Renderer.h"
#include "OverlayRenderer.h"
#include "Settings.h"
#include "MediaLibrary.h"
#include "Subtitles.h"
#include "MpvPlayer.h"
#include "RenderViewModel.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>
#include <cstddef>
#include <limits>

namespace Aperture {
class UIComponents {
public:
    static void DrawMediaItemCard( Renderer& r, const Settings& s, const D2D1_RECT_F& bounds, const MediaItem& item, bool isHovered,
        bool isSelected, bool isPinnedTop = false, float previewOpacity = 0.0f, ID2D1Bitmap* pLivePreviewBitmap = nullptr,
        bool isCloseHovered = false, const std::string* statusOverride = nullptr );
    // Draws one clipped, single-line editor surface. The caller owns any
    // outer border/background; paddingX is applied inside bounds. Text is
    // horizontally scrolled to keep the caret visible while focused, and unfocused long values receive Unicode-safe ellipsis treatment.
    static void DrawTextInput( Renderer& r, const Settings& s, const D2D1_RECT_F& bounds, const TextInputVisualState& state,
        const std::wstring& placeholder = L"", FontType font = FontType::Regular, float paddingX = 0.0f );
    static void DrawTextInput(OverlayRenderer& r, const Settings& s, const D2D1_RECT_F& bounds, const TextInputVisualState& state,
        const std::wstring& placeholder = L"", FontType font = FontType::Regular, float paddingX = 0.0f);
    static void DrawOSDBadge( Renderer& r, const Settings& s, const D2D1_RECT_F& bounds, IconType icon, const std::wstring& text,
        float opacity, bool compact = false );
    static void DrawOSDBadge( OverlayRenderer& r, const Settings& s, const D2D1_RECT_F& bounds, IconType icon, const std::wstring& text,
        float opacity, bool compact = false );
    static void DrawContextMenu( Renderer& r, const Settings& s, const D2D1_POINT_2F& origin, const std::vector<ContextMenuItem>& items,
        int hoveredIndex, float animProgress = 1.0f );
    static void DrawOverlayContextMenu( OverlayRenderer& r, const Settings& s, const D2D1_POINT_2F& origin,
        const std::vector<ContextMenuItem>& items, int hoveredIndex, float animProgress = 1.0f );
    // --- Immutable View-Model Consuming Rendering APIs ---
    static void DrawNavBar(Renderer& r, const Settings& s, const NavBarViewModel& vm);
    static void DrawEmptyState(Renderer& r, const Settings& s, const EmptyStateViewModel& vm);
    static void DrawMediaToolbar(Renderer& r, const Settings& s, const MediaTabViewModel& vm);
    static void DrawListenPanel(Renderer& r, const Settings& s, const ListenPanelViewModel& vm);
    static void DrawListenIsland(Renderer& r, const Settings& s, const ListenIslandViewModel& vm);
    static void DrawMediaGroupHeader(Renderer& r, const Settings& s, const VisibleMediaGroupHeaderViewModel& vm);
    static void DrawMediaItemCard(Renderer& r, const Settings& s, const VisibleMediaCardViewModel& vm);
    static void DrawPlaylistsRail(Renderer& r, const Settings& s, const PlaylistsRailViewModel& vm);
    static void DrawPlaylistDetailPane(Renderer& r, const Settings& s, const PlaylistDetailViewModel& vm);
    static void DrawHistoryGroupHeader(Renderer& r, const Settings& s, const HistoryGroupSummary& vm);
    static void DrawHistoryList(Renderer& r, const Settings& s, const HistoryTabViewModel& vm);
    static void DrawSettingsView(Renderer& r, const Settings& s, SettingsTabViewModel& vm);
    static void DrawPlayerTopBar(OverlayRenderer& r, const Settings& s, const PlayerTopBarViewModel& vm);
    static void DrawPlayerBottomBar(OverlayRenderer& r, const Settings& s, const PlayerBottomBarViewModel& vm);
    static void DrawPiPOverlay(OverlayRenderer& r, const Settings& s, const PiPOverlayViewModel& vm);
    static void DrawPlaybackDiagnostics(OverlayRenderer& r, const Settings& s, const DiagnosticsOverlayViewModel& vm);
    static void DrawLogOverlay(OverlayRenderer& r, const Settings& s, const LogOverlayViewModel& vm);
    static void DrawSettingsFlyout(OverlayRenderer& r, const Settings& s, const SettingsFlyoutViewModel& vm);
    static void DrawOSDBadge(Renderer& r, const Settings& s, const OsdBadgeViewModel& vm);
    static void DrawOSDBadge(OverlayRenderer& r, const Settings& s, const OsdBadgeViewModel& vm);
    static void DrawDoubleTapFeedback(OverlayRenderer& r, const Settings& s, const DoubleTapFeedbackViewModel& vm);
    static void DrawCenterPlayPauseBadge(OverlayRenderer& r, const Settings& s, const PlayPauseBadgeViewModel& vm);
    static void DrawJumpAheadButton(OverlayRenderer& r, const Settings& s, const JumpAheadViewModel& vm);
    static void DrawContextMenu(Renderer& r, const Settings& s, const ContextMenuViewModel& vm);
    static void DrawOverlayContextMenu(OverlayRenderer& r, const Settings& s, const ContextMenuViewModel& vm);
    static void DrawInputModal(Renderer& r, const Settings& s, const D2D1_RECT_F& bounds, const ModalViewModel& vm);
    static void DrawInputModal(OverlayRenderer& r, const Settings& s, const D2D1_RECT_F& bounds, const ModalViewModel& vm);
    static void DrawConfirmModal(Renderer& r, const Settings& s, const D2D1_RECT_F& bounds, const ModalViewModel& vm);
    static void DrawConfirmModal(OverlayRenderer& r, const Settings& s, const D2D1_RECT_F& bounds, const ModalViewModel& vm);
    static void DrawAutoscrollAnchor(Renderer& r, const Settings& s, const AutoscrollViewModel& vm);
    static void DrawImportProgress(Renderer& r, const Settings& s, const ImportProgressViewModel& vm);
    static void DrawIslandShell(Renderer& r, const Settings& s, const D2D1_RECT_F& islandRect, float radiusPx, float expand, bool hovered, float alpha);
    static void DrawGlobalSearchSurface(Renderer& r, const Settings& s, const GlobalSearchSurfaceViewModel& vm);
    static bool HasVisibleOverlayContent(const PlayerRenderViewModel& vm);
    static void DrawPlayerOverlay(OverlayRenderer& r, const Settings& s, const PlayerRenderViewModel& vm);
    static void DrawHub(Renderer& r, const Settings& s, HubRenderViewModel& vm);
};

} // namespace Aperture
