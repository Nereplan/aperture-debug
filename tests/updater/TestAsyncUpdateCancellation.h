#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/AsyncOwnedJob.h"
#include "../../src/Updater.h"
#include <chrono>

namespace Aperture::Test {

TEST_CASE(UpdaterNetworkBounds, PreCancelledCheckAndDownloadSkipTransport) {
    ScopedTempDir root;
    struct CountingTransport : IHttpTransport {
        int calls = 0;
        HttpResponse Get(const std::wstring&, const HttpRequestOptions&) override { ++calls; return {}; }
    } transport;
    UpdaterConfig config; config.cancellation = std::make_shared<OperationCancellation>(); config.cancellation->Cancel();
    TEST_ASSERT_EQ(CheckForUpdates(config, &transport, root.GetPath()).status, UpdaterStatusCode::Cancelled);
    TEST_ASSERT_EQ(DownloadAndStageUpdate({}, config, &transport, nullptr, root.GetPath()).status, UpdaterStatusCode::Cancelled);
    TEST_ASSERT_EQ(transport.calls, 0);
}

TEST_CASE(UpdaterTempCleanup, DiscardedAsyncCompletionOwnsItsStagingDirectory) {
    AsyncOwnedJob<UpdateDownloadResult> job;
    std::wstring directory;
    job.Start([](const auto&) {
        UpdateDownloadResult result;
        result.stagingLease = std::make_shared<ScopedTempDirectory>(L"Aperture_TestAsyncUpdate_");
        result.stagingDirectory = result.stagingLease->GetPath();
        return result;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::optional<AsyncOwnedJob<UpdateDownloadResult>::Completion> result;
    while (!result && std::chrono::steady_clock::now() < deadline) { result = job.TakeResult(); Sleep(1); }
    TEST_ASSERT(result && result->result);
    if (!result || !result->result) return;
    directory = result->result->stagingDirectory;
    TEST_ASSERT(DirectoryExists(directory));
    result.reset();
    TEST_ASSERT(!DirectoryExists(directory));
}

} // namespace Aperture::Test
