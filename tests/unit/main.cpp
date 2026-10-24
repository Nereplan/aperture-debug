#include "../support/TestRunner.h"
#include "TestAccessibility.h"
#include "TestAnimationPerformance.h"
#include "TestAudioManagement.h"
#include "TestBackgroundAvailability.h"
#include "TestBackendBalance.h"
#include "TestBulkLibraryOperations.h"
#include "TestColorManagement.h"
#include "TestCommandDispatcher.h"
#include "TestCustomMpvConfig.h"
#include "TestDeferredServices.h"
#include "TestPlaybackRenderer.h"
#include "TestDuplicateDetection.h"
#include "TestFeatureGate.h"
#include "TestFolderRediscovery.h"
#include "TestFolderWatchConfiguration.h"
#include "TestGlobalSearch.h"
#include "TestLibraryQuery.h"
#include "TestLibrarySaving.h"
#include "TestLibrarySnapshot.h"
#include "TestLocalization.h"
#include "TestMediaTransport.h"
#include "TestMetadataOverrides.h"
#include "TestMpvInitialization.h"
#include "TestNetworkStream.h"
#include "TestOnlineSubtitles.h"
#include "TestOverlayPacing.h"
#include "TestOverlaySurface.h"
#include "TestPlaybackSession.h"
#include "TestPlaybackFlyout.h"
#include "TestRendererCache.h"
#include "TestRtlComplexScripts.h"
#include "TestSubtitleDiscovery.h"
#include "TestSubtitlesDual.h"
#include "TestTaskbarController.h"
#include "TestWatchReconcile.h"

#include "TestAsyncNetworkJobs.h"

#include "TestHistoryGrouping.h"

#include "TestHttpTransportFixes.h"

#include "TestImportFingerprints.h"

#include "TestIncrementalSaving.h"

#include "TestLongPathSupport.h"

#include "TestMetadataBackfill.h"

#include "TestPlaylistIO.h"

#include "TestPresentation.h"

#include "TestRenderViewModel.h"

#include "TestReorder.h"

#include "TestScreenshotCapture.h"

#include "TestShuffle.h"

#include "TestHubThumbnailCache.h"

#include "TestSmartPlaylists.h"

#include "TestTouchInput.h"

int main(int argc, char* argv[]) {
    return Aperture::Test::RunRegisteredTests(argc, argv, "Aperture unit tests");
}
