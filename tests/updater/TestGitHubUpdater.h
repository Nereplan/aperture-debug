#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Updater.h"
#include "../../src/Json.h"

namespace Aperture::Test {

static std::string MakeZipPayload(size_t totalBytes) {
    std::string data(totalBytes, '\x5A');
    if (totalBytes >= 4) {
        data[0] = 'P';
        data[1] = 'K';
        data[2] = '\x03';
        data[3] = '\x04';
    }
    return data;
}

static std::string BuildGitHubReleaseJson(
    const std::string& tagName,
    bool draft,
    bool prerelease,
    const std::string& assetName,
    uint64_t assetSize,
    const std::string& digestField,
    const std::string& downloadUrl,
    const std::string& sidecarUrl = "",
    const std::string& notes = "Fixes and improvements.")
{
    JsonValue asset = JsonValue::Object();
    asset["name"] = assetName;
    asset["size"] = static_cast<double>(assetSize);
    asset["browser_download_url"] = downloadUrl;
    if (!digestField.empty()) asset["digest"] = digestField;

    JsonValue assets = JsonValue::Array();
    assets.as_array().push_back(std::move(asset));

    if (!sidecarUrl.empty()) {
        JsonValue sidecar = JsonValue::Object();
        sidecar["name"] = assetName + ".sha256";
        sidecar["size"] = 128.0;
        sidecar["browser_download_url"] = sidecarUrl;
        assets.as_array().push_back(std::move(sidecar));
    }

    JsonValue rel = JsonValue::Object();
    rel["tag_name"] = tagName;
    rel["draft"] = draft;
    rel["prerelease"] = prerelease;
    rel["published_at"] = "2026-10-01T12:00:00Z";
    rel["body"] = notes;
    rel["assets"] = std::move(assets);
    return rel.stringify(false);
}

static void step_GitHubUpdater_RepoValidationAndReleaseSourceConfig() {
    TEST_ASSERT(IsValidGitHubRepository("Nereplan/aperture-debug"));
    TEST_ASSERT(IsValidGitHubRepository("org_1/repo.name-2"));
    TEST_ASSERT(!IsValidGitHubRepository(""));
    TEST_ASSERT(!IsValidGitHubRepository("noslash"));
    TEST_ASSERT(!IsValidGitHubRepository("a/b/c"));
    TEST_ASSERT(!IsValidGitHubRepository("../evil"));
    TEST_ASSERT(!IsValidGitHubRepository("owner/.."));
    TEST_ASSERT(!IsValidGitHubRepository("owner/repo with space"));

    ScopedTempDir root;
    ReleaseSourceConfig cfg;
    std::string err;
    TEST_ASSERT(!LoadReleaseSourceConfig(root.GetPath(), cfg, err));
    TEST_ASSERT(!err.empty());

    TEST_ASSERT(AtomicWriteTextFile(root.SubPath(L"release-source.json"), "{ invalid json").is_ok());
    TEST_ASSERT(!LoadReleaseSourceConfig(root.GetPath(), cfg, err));

    TEST_ASSERT(AtomicWriteTextFile(root.SubPath(L"release-source.json"), R"({"version":"2.0.0.0","repository":"bad repo"})").is_ok());
    TEST_ASSERT(!LoadReleaseSourceConfig(root.GetPath(), cfg, err));

    TEST_ASSERT(AtomicWriteTextFile(root.SubPath(L"release-source.json"), R"({"version":"2.0.0.0","repository":"example/aperture"})").is_ok());
    TEST_ASSERT(LoadReleaseSourceConfig(root.GetPath(), cfg, err));
    TEST_ASSERT_EQ(cfg.version, "2.0.0.0");
    TEST_ASSERT_EQ(cfg.repository, "example/aperture");
}

static void step_GitHubUpdater_CheckSuccessSidecarFallbackAndUpToDate() {
    ScopedTempDir root;
    const std::string payload = MakeZipPayload(160 * 1024);
    const std::string sha = ComputeSha256Hex(payload.data(), payload.size());
    const std::string zipUrl = "https://github.com/example/aperture/releases/download/v9.9.9.9/Aperture-9.9.9.9-windows-x64.zip";
    const std::string sidecarUrl = zipUrl + ".sha256";

    MockHttpTransport mock;
    HttpResponse latestOk;
    latestOk.success = true;
    latestOk.statusCode = 200;
    latestOk.finalUrl = L"https://api.github.com/repos/example/aperture/releases/latest";
    latestOk.body = BuildGitHubReleaseJson("v9.9.9.9", false, false, "Aperture-9.9.9.9-windows-x64.zip", payload.size(), "sha256:" + sha, zipUrl);
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases/latest", latestOk);

    GitHubUpdateConfig ghCfg;
    ghCfg.repository = "example/aperture";
    ghCfg.channel = "stable";

    UpdateCheckResult res = CheckForGitHubUpdates(ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::UpdateAvailable);
    TEST_ASSERT(res.updateAvailable);
    TEST_ASSERT_EQ(res.manifest.version, "9.9.9.9");
    TEST_ASSERT_EQ(res.manifest.sha256, sha);
    TEST_ASSERT_EQ(res.manifest.downloadUrl, zipUrl);
    TEST_ASSERT_EQ(res.manifest.fileSizeBytes, payload.size());

    // Sidecar fallback when asset.digest is omitted
    latestOk.body = BuildGitHubReleaseJson("v9.9.9.9", false, false, "Aperture-9.9.9.9-windows-x64.zip", payload.size(), "", zipUrl, sidecarUrl);
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases/latest", latestOk);
    HttpResponse sidecarResp;
    sidecarResp.success = true;
    sidecarResp.statusCode = 200;
    sidecarResp.finalUrl = Utf8ToUtf16(sidecarUrl);
    sidecarResp.body = sha + " *Aperture-9.9.9.9-windows-x64.zip\n";
    mock.SetResponse(Utf8ToUtf16(sidecarUrl), sidecarResp);

    res = CheckForGitHubUpdates(ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::UpdateAvailable);
    TEST_ASSERT_EQ(res.manifest.sha256, sha);

    // Reject missing checksum when both asset.digest and sidecar are absent
    latestOk.body = BuildGitHubReleaseJson("v9.9.9.9", false, false, "Aperture-9.9.9.9-windows-x64.zip", payload.size(), "", zipUrl);
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases/latest", latestOk);
    res = CheckForGitHubUpdates(ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::InvalidManifestJson);
    TEST_ASSERT(!res.updateAvailable);

    // Up-to-date and downgrade checks
    const std::string curVer = SemanticVersion::Current().ToString();
    latestOk.body = BuildGitHubReleaseJson("v" + curVer, false, false, "Aperture-" + curVer + "-windows-x64.zip", payload.size(), "sha256:" + sha, zipUrl);
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases/latest", latestOk);
    res = CheckForGitHubUpdates(ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::UpToDate);
    TEST_ASSERT(!res.updateAvailable);

    latestOk.body = BuildGitHubReleaseJson("v1.0.0.0", false, false, "Aperture-1.0.0.0-windows-x64.zip", payload.size(), "sha256:" + sha, zipUrl);
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases/latest", latestOk);
    res = CheckForGitHubUpdates(ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::ManifestDowngrade);
    TEST_ASSERT(!res.updateAvailable);
}

static void step_GitHubUpdater_404DisambiguationRateLimitAndChannels() {
    ScopedTempDir root;
    MockHttpTransport mock;
    GitHubUpdateConfig ghCfg;
    ghCfg.repository = "example/aperture";
    ghCfg.channel = "stable";

    HttpResponse notFound;
    notFound.success = false;
    notFound.status = HttpTransportStatus::HttpError;
    notFound.statusCode = 404;
    notFound.errorMessage = L"HTTP 404 Not Found";
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases/latest", notFound);

    // 1. Both /latest and /releases return 404 -> missing or private repository
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases?per_page=10", notFound);
    UpdateCheckResult res = CheckForGitHubUpdates(ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::HttpError);
    TEST_ASSERT_EQ(res.httpStatusCode, 404);
    TEST_ASSERT(res.message.find("not found") != std::string::npos);

    // 2. Public repo exists with 0 releases -> UpToDate with clear message
    HttpResponse listEmpty;
    listEmpty.success = true;
    listEmpty.statusCode = 200;
    listEmpty.finalUrl = L"https://api.github.com/repos/example/aperture/releases?per_page=10";
    listEmpty.body = "[]";
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases?per_page=10", listEmpty);
    res = CheckForGitHubUpdates(ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::UpToDate);
    TEST_ASSERT(!res.updateAvailable);

    // 3. Rate limit (403)
    HttpResponse rateLimited;
    rateLimited.success = false;
    rateLimited.status = HttpTransportStatus::HttpError;
    rateLimited.statusCode = 403;
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases/latest", rateLimited);
    res = CheckForGitHubUpdates(ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::HttpError);
    TEST_ASSERT_EQ(res.httpStatusCode, 403);
    TEST_ASSERT(res.message.find("rate limit") != std::string::npos);

    // 4. Beta channel skips draft and selects beta prerelease cleanly
    const std::string payload = MakeZipPayload(4096);
    const std::string sha = ComputeSha256Hex(payload.data(), payload.size());
    const std::string urlBeta = "https://github.com/example/aperture/releases/download/v9.1.0.0-beta.2/Aperture-9.1.0.0-windows-x64.zip";
    const std::string urlNightly = "https://github.com/example/aperture/releases/download/v9.2.0.0-nightly.1/Aperture-9.2.0.0-windows-x64.zip";

    std::string listJson = "[" +
        BuildGitHubReleaseJson("v9.9.0.0", true, false, "Aperture-9.9.0.0-windows-x64.zip", payload.size(), "sha256:" + sha, urlBeta) + "," +
        BuildGitHubReleaseJson("v9.2.0.0-nightly.1", false, true, "Aperture-9.2.0.0-windows-x64.zip", payload.size(), "sha256:" + sha, urlNightly) + "," +
        BuildGitHubReleaseJson("v9.1.0.0-beta.2", false, true, "Aperture-9.1.0.0-windows-x64.zip", payload.size(), "sha256:" + sha, urlBeta) +
        "]";
    HttpResponse listBeta;
    listBeta.success = true;
    listBeta.statusCode = 200;
    listBeta.finalUrl = L"https://api.github.com/repos/example/aperture/releases?per_page=15";
    listBeta.body = listJson;
    mock.SetResponse(L"https://api.github.com/repos/example/aperture/releases?per_page=15", listBeta);

    ghCfg.channel = "beta";
    res = CheckForGitHubUpdates(ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::UpdateAvailable);
    TEST_ASSERT_EQ(res.manifest.version, "9.1.0.0-beta.2");

    // 5. Disallow URL from unexpected repository or HTTP scheme
    const std::string evilUrl = "https://github.com/attacker/fork/releases/download/v9.9.9.9/Aperture-9.9.9.9-windows-x64.zip";
    const std::string badRepoJson = BuildGitHubReleaseJson("v9.9.9.9", false, false, "Aperture-9.9.9.9-windows-x64.zip", payload.size(), "sha256:" + sha, evilUrl);
    ghCfg.channel = "stable";
    res = ValidateGitHubReleaseJson(badRepoJson, ghCfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::DisallowedRedirect);
    TEST_ASSERT(!res.updateAvailable);
}

static void step_GitHubUpdater_DownloadVerificationProgressAndCancellation() {
    ScopedTempDir root;
    const std::string payload = MakeZipPayload(150 * 1024);
    const std::string sha = ComputeSha256Hex(payload.data(), payload.size());
    const std::string zipUrl = "https://github.com/example/aperture/releases/download/v9.9.9.9/Aperture-9.9.9.9-windows-x64.zip";

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.version = "9.9.9.9";
    manifest.downloadUrl = zipUrl;
    manifest.fileSizeBytes = payload.size();
    manifest.sha256 = sha;

    GitHubUpdateConfig ghCfg;
    ghCfg.repository = "example/aperture";

    MockHttpTransport mock;
    HttpResponse pkgResp;
    pkgResp.success = true;
    pkgResp.statusCode = 200;
    pkgResp.finalUrl = L"https://objects.githubusercontent.com/github-production-release-asset/12345";
    pkgResp.body = payload;
    mock.SetResponse(Utf8ToUtf16(zipUrl), pkgResp);

    int progressEvents = 0;
    uint64_t lastDownloaded = 0;
    uint64_t lastTotal = 0;
    UpdateDownloadResult dlRes = DownloadGitHubReleasePackage(
        manifest,
        ghCfg,
        &mock,
        [&](uint64_t dl, uint64_t total) {
            ++progressEvents;
            lastDownloaded = dl;
            lastTotal = total;
        },
        root.GetPath());

    TEST_ASSERT_EQ(dlRes.status, UpdaterStatusCode::Success);
    TEST_ASSERT(progressEvents >= 3); // Initial + chunks + completion
    TEST_ASSERT_EQ(lastDownloaded, payload.size());
    TEST_ASSERT_EQ(lastTotal, payload.size());
    TEST_ASSERT(!dlRes.packagePath.empty());
    TEST_ASSERT(FileExists(dlRes.packagePath));
    TEST_ASSERT_EQ(ComputeFileSha256Hex(dlRes.packagePath), sha);

    const std::wstring stagingPath = dlRes.stagingDirectory;
    dlRes.stagingLease.reset();
    TEST_ASSERT(!DirectoryExists(stagingPath));

    // Reject HTML/corrupt file claiming to be ZIP even if size & sha match wrong magic check
    std::string htmlFake(4096, ' ');
    htmlFake.replace(0, 6, "<html>");
    manifest.fileSizeBytes = htmlFake.size();
    manifest.sha256 = ComputeSha256Hex(htmlFake.data(), htmlFake.size());
    pkgResp.body = htmlFake;
    mock.SetResponse(Utf8ToUtf16(zipUrl), pkgResp);
    UpdateDownloadResult badMagic = DownloadGitHubReleasePackage(manifest, ghCfg, &mock, nullptr, root.GetPath());
    TEST_ASSERT_EQ(badMagic.status, UpdaterStatusCode::InvalidManifestJson);

    // Reject SHA-256 mismatch
    manifest.fileSizeBytes = payload.size();
    manifest.sha256 = std::string(64, 'a');
    pkgResp.body = payload;
    mock.SetResponse(Utf8ToUtf16(zipUrl), pkgResp);
    UpdateDownloadResult badHash = DownloadGitHubReleasePackage(manifest, ghCfg, &mock, nullptr, root.GetPath());
    TEST_ASSERT_EQ(badHash.status, UpdaterStatusCode::HashMismatch);

    // Pre-cancelled download stops immediately without network call
    ghCfg.cancellation = std::make_shared<OperationCancellation>();
    ghCfg.cancellation->Cancel();
    manifest.sha256 = sha;
    UpdateDownloadResult cancelledRes = DownloadGitHubReleasePackage(manifest, ghCfg, &mock, nullptr, root.GetPath());
    TEST_ASSERT_EQ(cancelledRes.status, UpdaterStatusCode::Cancelled);
}

static void step_GitHubUpdater_SignedCheckAndAuthenticodeFailClosed() {
    // Verify Authenticode fails closed on unsigned binaries instead of continuing after WinVerifyTrust error
    ScopedTempDir root;
    const std::wstring copiedExe = root.SubPath(L"ApertureUnsignedCopy.exe");
    TEST_ASSERT(CopyFileW(GetExePath().c_str(), copiedExe.c_str(), TRUE) != FALSE);
    std::string thumb, authErr;
    // Test harness binary is unsigned, so VerifyAuthenticodeAndPin MUST return false cleanly
    TEST_ASSERT(!VerifyAuthenticodeAndPin(copiedExe, "", thumb, authErr));
    TEST_ASSERT(!authErr.empty());

    // FormatBytesHuman formatting checks
    TEST_ASSERT_EQ(FormatBytesHuman(512), std::wstring(L"512 B"));
    TEST_ASSERT_EQ(FormatBytesHuman(2048), std::wstring(L"2 KB"));
    TEST_ASSERT_EQ(FormatBytesHuman(5ULL * 1024 * 1024), std::wstring(L"5.0 MB"));

    // Verify CheckForUpdates maps HTTP 404 to HttpError instead of collapsing to generic NetworkError
    MockHttpTransport mock;
    HttpResponse notFound;
    notFound.success = false;
    notFound.status = HttpTransportStatus::HttpError;
    notFound.statusCode = 404;
    notFound.errorMessage = L"Not Found";
    mock.SetResponse(L"https://updates.example.com/release.json", notFound);

    UpdaterConfig cfg;
    cfg.manifestUrl = "https://updates.example.com/release.json";
    cfg.allowUnsignedForTesting = true;
    UpdateCheckResult res = CheckForUpdates(cfg, &mock, root.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::HttpError);
    TEST_ASSERT_EQ(res.httpStatusCode, 404);
}

TEST_CASE(Updater, GitHubReleaseAndSecurityHardening) {
    step_GitHubUpdater_RepoValidationAndReleaseSourceConfig();
    step_GitHubUpdater_CheckSuccessSidecarFallbackAndUpToDate();
    step_GitHubUpdater_404DisambiguationRateLimitAndChannels();
    step_GitHubUpdater_DownloadVerificationProgressAndCancellation();
    step_GitHubUpdater_SignedCheckAndAuthenticodeFailClosed();
}

} // namespace Aperture::Test
