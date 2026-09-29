#pragma once

#include "../support/TestFramework.h"
#include "NetworkStream.h"
#include <string>
#include <vector>
#include <chrono>
#include <thread>

namespace Aperture::Test {

TEST_CASE(NetworkStream, SchemeValidation) {
    // Allowed schemes
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://example.com/live.m3u8"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"https://example.com/video.mp4"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"rtsp://192.168.1.100:554/stream1"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"rtmp://live.twitch.tv/app/streamkey"));

    // Case-insensitive scheme
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"HTTP://example.com/test"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"Https://example.com/test"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"RTSP://127.0.0.1/live"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"RTMP://localhost/live"));

    // Disallowed schemes
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"file:///C:/videos/test.mp4"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"javascript:alert(1)"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"data:text/html,<html></html>"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"blob:http://example.com/uuid"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"ftp://ftp.example.com/media.mkv"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"custom://myprotocol/stream"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"ws://echo.websocket.org"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"chrome://settings"));

    // Malformed delimiter
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http:/example.com/test"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"https:example.com/test"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"://example.com/test"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"example.com/test"));
}

TEST_CASE(NetworkStream, HostPortAndIPv6) {
    // Valid IPv4
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://127.0.0.1/live"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://192.168.1.1:8080/stream"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"rtsp://10.0.0.1:554/live"));

    // Invalid IPv4
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://999.999.999.999/live"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://192.168.1.256/live"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://192.168.1/live"));

    // Valid IPv6
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://[::1]/live"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"https://[2001:db8::1]/stream.m3u8"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://[::1]:8080/live"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"rtsp://[fe80::1%eth0]:554/cam"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://[::ffff:192.168.1.1]:8000/stream"));

    // Invalid IPv6
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://[:::1]/live"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://[::1/live")); // missing closing bracket
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://[2001:xyz::1]/live")); // invalid hex

    // Valid domains
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://example.com/stream"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"https://stream.live-video.net/channel1"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://localhost:3000/media"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://sub-domain.example.co.uk:9000/stream"));

    // Invalid domains / empty host
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http:///path"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://:8080/path"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://-example.com/stream")); // leading hyphen in label
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://example..com/stream")); // consecutive dots

    // Port validation
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://example.com:1/stream"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://example.com:80/stream"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://example.com:65535/stream"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://example.com:0/stream"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://example.com:65536/stream"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://example.com:999999/stream"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://example.com:abc/stream"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://example.com:-80/stream"));
}

TEST_CASE(NetworkStream, CredentialsAndPolicy) {
    // Default: no embedded credentials
    NetworkStreamValidationOptions defaultOpts;
    defaultOpts.allowCredentials = false;

    NetworkStreamUrl res1 = NetworkStreamValidator::ValidateAndParse(L"http://user:password@example.com/live.m3u8", defaultOpts);
    TEST_ASSERT(!res1.isValid);
    TEST_ASSERT_EQ(static_cast<int>(res1.error), static_cast<int>(NetworkStreamValidationError::EmbeddedCredentialsNotAllowed));
    TEST_ASSERT(res1.hasCredentials);

    NetworkStreamUrl res2 = NetworkStreamValidator::ValidateAndParse(L"http://admin@example.com/live", defaultOpts);
    TEST_ASSERT(!res2.isValid);
    TEST_ASSERT_EQ(static_cast<int>(res2.error), static_cast<int>(NetworkStreamValidationError::EmbeddedCredentialsNotAllowed));

    // When explicitly permitted:
    NetworkStreamValidationOptions allowCreds;
    allowCreds.allowCredentials = true;

    NetworkStreamUrl res3 = NetworkStreamValidator::ValidateAndParse(L"http://admin:secret123@example.com:8080/live.m3u8", allowCreds);
    TEST_ASSERT(res3.isValid);
    TEST_ASSERT(res3.hasCredentials);
    TEST_ASSERT_EQ(res3.username, std::wstring(L"admin"));
    TEST_ASSERT_EQ(res3.password, std::wstring(L"secret123"));
}

TEST_CASE(NetworkStream, RedactionAndSafeErrors) {
    // Redacting user credentials
    std::wstring raw1 = L"http://admin:supersecret@192.168.1.50:554/h264";
    std::wstring redacted1 = NetworkStreamValidator::RedactUrl(raw1);
    TEST_ASSERT(redacted1.find(L"supersecret") == std::wstring::npos);
    TEST_ASSERT(redacted1.find(L"admin:***@") != std::wstring::npos);

    // Redacting query tokens
    std::wstring raw2 = L"https://cdn.example.com/stream.m3u8?token=xyz123abc&quality=hd&key=secretkey";
    std::wstring redacted2 = NetworkStreamValidator::RedactUrl(raw2);
    TEST_ASSERT(redacted2.find(L"xyz123abc") == std::wstring::npos);
    TEST_ASSERT(redacted2.find(L"secretkey") == std::wstring::npos);
    TEST_ASSERT(redacted2.find(L"quality=hd") != std::wstring::npos);

    // Safe error message does not leak raw credentials
    std::wstring safeErr = NetworkStreamValidator::GetSafeErrorMessage(
        NetworkStreamValidationError::EmbeddedCredentialsNotAllowed, raw1);
    TEST_ASSERT(!safeErr.empty());
    TEST_ASSERT(safeErr.find(L"supersecret") == std::wstring::npos);

    std::string safeErrUtf8 = NetworkStreamValidator::GetSafeErrorMessageUtf8(
        NetworkStreamValidationError::EmbeddedCredentialsNotAllowed, Utf16ToUtf8(raw1));
    TEST_ASSERT(!safeErrUtf8.empty());
    TEST_ASSERT(safeErrUtf8.find("supersecret") == std::string::npos);
}

TEST_CASE(NetworkStream, BoundsAndControlCharacters) {
    // Empty URL
    NetworkStreamUrl emptyRes = NetworkStreamValidator::ValidateAndParse(L"");
    TEST_ASSERT(!emptyRes.isValid);
    TEST_ASSERT_EQ(static_cast<int>(emptyRes.error), static_cast<int>(NetworkStreamValidationError::EmptyUrl));

    // Bounded length
    NetworkStreamValidationOptions lengthOpts;
    lengthOpts.maxLength = 50;
    std::wstring longUrl = L"http://example.com/a/very/long/path/that/exceeds/the/fifty/character/limit/test.mp4";
    NetworkStreamUrl lenRes = NetworkStreamValidator::ValidateAndParse(longUrl, lengthOpts);
    TEST_ASSERT(!lenRes.isValid);
    TEST_ASSERT_EQ(static_cast<int>(lenRes.error), static_cast<int>(NetworkStreamValidationError::UrlTooLong));

    // Control characters rejection
    std::wstring withNewline = L"http://example.com/live\n.m3u8";
    TEST_ASSERT(!NetworkStreamValidator::IsValid(withNewline));
    TEST_ASSERT_EQ(static_cast<int>(NetworkStreamValidator::ValidateAndParse(withNewline).error),
        static_cast<int>(NetworkStreamValidationError::ContainsControlCharacters));

    std::wstring withCarriageReturn = L"http://example.com/live\r.m3u8";
    TEST_ASSERT(!NetworkStreamValidator::IsValid(withCarriageReturn));

    std::wstring withNull = std::wstring(L"http://example.com/live") + wchar_t(0) + L".m3u8";
    TEST_ASSERT(!NetworkStreamValidator::IsValid(withNull));

    std::wstring withTab = L"http://example.com/live\t.m3u8";
    TEST_ASSERT(!NetworkStreamValidator::IsValid(withTab));

    std::wstring withEscape = L"http://example.com/live\x1b[31m.m3u8";
    TEST_ASSERT(!NetworkStreamValidator::IsValid(withEscape));
}

TEST_CASE(NetworkStream, CommandAndModelProjection) {
    NetworkStreamModel model;
    TEST_ASSERT(!model.IsOpen());

    model.Open();
    TEST_ASSERT(model.IsOpen());

    model.SetInput(L"https://example.com/live.m3u8");
    TEST_ASSERT(model.IsValid());
    TEST_ASSERT_EQ(model.GetInput(), std::wstring(L"https://example.com/live.m3u8"));

    NetworkStreamProjection proj = model.GetProjection();
    TEST_ASSERT(proj.isOpen);
    TEST_ASSERT(proj.isValidInput);
    TEST_ASSERT_EQ(proj.currentInput, std::wstring(L"https://example.com/live.m3u8"));
    TEST_ASSERT_EQ(proj.redactedDisplayUrl, std::wstring(L"https://example.com/live.m3u8"));
    TEST_ASSERT(proj.lastError.empty());

    // Invalid input reflects in projection
    model.SetInput(L"ftp://invalid.com");
    TEST_ASSERT(!model.IsValid());
    NetworkStreamProjection projInvalid = model.GetProjection();
    TEST_ASSERT(!projInvalid.isValidInput);
    TEST_ASSERT(!projInvalid.lastError.empty());

    model.Close();
    TEST_ASSERT(!model.IsOpen());
    TEST_ASSERT(!model.GetProjection().isOpen);
}

TEST_CASE(NetworkStream, AsyncCancellationAndStaleCallbacks) {
    NetworkStreamLoader loader;
    TEST_ASSERT(!loader.IsOpening());

    std::atomic<bool> callbackFired{ false };
    std::atomic<uint64_t> receivedGen{ 0 };

    // Begin open with null player (standalone test)
    uint64_t gen1 = loader.BeginOpen(L"https://example.com/live.m3u8", nullptr,
        [&callbackFired, &receivedGen](bool success, const std::wstring& displayUrl, const std::wstring& errorMsg, uint64_t gen) {
            callbackFired.store(true);
            receivedGen.store(gen);
        });

    TEST_ASSERT(gen1 > 0);

    // Cancel immediately before callback can complete
    loader.Cancel();
    TEST_ASSERT(!loader.IsOpening());
    uint64_t activeGen = loader.GetActiveGeneration();
    TEST_ASSERT(activeGen > gen1);

    // Wait slightly to ensure background thread finishes safely without executing stale callback
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TEST_ASSERT(!callbackFired.load()); // callback must have been discarded due to generation mismatch
}

TEST_CASE(NetworkStream, RecentStreamsDisabledByDefault) {
    NetworkStreamModel model;
    TEST_ASSERT(!model.IsRecentStreamsEnabled());

    // Adding stream when disabled does nothing
    model.AddRecentStream(L"https://example.com/live.m3u8");
    TEST_ASSERT(model.GetRecentStreams().empty());

    // Save and load return OK without writing to disk
    StorageStatus saveStatus = model.SaveRecentStreams();
    TEST_ASSERT(saveStatus.is_ok());

    StorageStatus loadStatus = model.LoadRecentStreams();
    TEST_ASSERT(loadStatus.is_ok());
}

TEST_CASE(NetworkStream, RootLocalRecentStreamsRoundTrip) {
    NetworkStreamConfig config;
    config.enableRecentStreams = true;
    config.maxRecentStreams = 5;

    std::wstring testDir = GetExeDir();
    std::wstring testFile = L"test_recent_streams.json";

    NetworkStreamModel model(config);
    TEST_ASSERT(model.IsRecentStreamsEnabled());

    // Add valid streams
    model.AddRecentStream(L"https://example.com/stream1.m3u8", L"Stream 1");
    model.AddRecentStream(L"http://192.168.1.100:8080/live", L"Local Cam");
    model.AddRecentStream(L"rtsp://10.0.0.1:554/feed", L"RTSP Feed");

    TEST_ASSERT_EQ(model.GetRecentStreams().size(), static_cast<size_t>(3));

    // Save to test file
    StorageStatus saveRes = model.SaveRecentStreams(testDir, testFile);
    TEST_ASSERT(saveRes.is_ok());

    // Load into a new model
    NetworkStreamModel loadedModel(config);
    StorageStatus loadRes = loadedModel.LoadRecentStreams(testDir, testFile);
    TEST_ASSERT(loadRes.is_ok());

    const auto& loadedStreams = loadedModel.GetRecentStreams();
    TEST_ASSERT_EQ(loadedStreams.size(), static_cast<size_t>(3));
    TEST_ASSERT_EQ(loadedStreams[0].title, std::wstring(L"RTSP Feed"));
    TEST_ASSERT_EQ(loadedStreams[1].title, std::wstring(L"Local Cam"));
    TEST_ASSERT_EQ(loadedStreams[2].title, std::wstring(L"Stream 1"));

    // Cleanup test file
    std::wstring fullPath = testDir + L"\\" + testFile;
    DeleteFileW(fullPath.c_str());
}


TEST_CASE(NetworkStream, DroppedUrlTextExtraction) {
    // 1. Single-line URL
    std::wstring url1 = L"https://example.com/video.mp4";
    TEST_ASSERT_EQ(NetworkStreamValidator::ExtractCandidateUrl(url1), url1);
    DroppedUrlResult res1 = NetworkStreamValidator::ExtractAndValidateDroppedText(url1);
    TEST_ASSERT(res1.hasUrl);
    TEST_ASSERT(res1.parsedUrl.isValid);
    TEST_ASSERT_EQ(res1.extractedUrl, url1);
    TEST_ASSERT(res1.safeErrorMessage.empty());

    // 2. Whitespace and newlines padding
    std::wstring url2 = L"   \r\n\t  https://example.com/stream.m3u8   \r\n  ";
    TEST_ASSERT_EQ(NetworkStreamValidator::ExtractCandidateUrl(url2), std::wstring(L"https://example.com/stream.m3u8"));
    DroppedUrlResult res2 = NetworkStreamValidator::ExtractAndValidateDroppedText(url2);
    TEST_ASSERT(res2.hasUrl);
    TEST_ASSERT(res2.parsedUrl.isValid);
    TEST_ASSERT_EQ(res2.extractedUrl, std::wstring(L"https://example.com/stream.m3u8"));

    // 3. Multi-line uri-list with comment headers
    std::wstring uriList = L"# EXTINF:0,Sample Stream\r\n# https://ignored.comment\r\nhttps://example.com/main.m3u8\r\nhttps://example.com/fallback.m3u8";
    std::wstring extracted = NetworkStreamValidator::ExtractCandidateUrl(uriList);
    TEST_ASSERT_EQ(extracted, std::wstring(L"https://example.com/main.m3u8"));
    DroppedUrlResult res3 = NetworkStreamValidator::ExtractAndValidateDroppedText(uriList);
    TEST_ASSERT(res3.hasUrl);
    TEST_ASSERT(res3.parsedUrl.isValid);
    TEST_ASSERT_EQ(res3.extractedUrl, std::wstring(L"https://example.com/main.m3u8"));

    // 4. Empty, whitespace-only, comments-only
    TEST_ASSERT_EQ(NetworkStreamValidator::ExtractCandidateUrl(L""), std::wstring(L""));
    TEST_ASSERT(!NetworkStreamValidator::ExtractAndValidateDroppedText(L"").hasUrl);

    TEST_ASSERT_EQ(NetworkStreamValidator::ExtractCandidateUrl(L"   \t\r\n   "), std::wstring(L""));
    TEST_ASSERT(!NetworkStreamValidator::ExtractAndValidateDroppedText(L"   \t\r\n   ").hasUrl);

    TEST_ASSERT_EQ(NetworkStreamValidator::ExtractCandidateUrl(L"# Comment 1\r\n# Comment 2\r\n"), std::wstring(L""));
    TEST_ASSERT(!NetworkStreamValidator::ExtractAndValidateDroppedText(L"# Comment 1\r\n# Comment 2\r\n").hasUrl);

    // 5. UTF-8 candidate extraction
    std::string utf8Input = "  \n  rtsp://192.168.1.50:554/cam1 \r\n ";
    TEST_ASSERT_EQ(NetworkStreamValidator::ExtractCandidateUrlUtf8(utf8Input), std::string("rtsp://192.168.1.50:554/cam1"));
    DroppedUrlResult resUtf8 = NetworkStreamValidator::ExtractAndValidateDroppedTextUtf8(utf8Input);
    TEST_ASSERT(resUtf8.hasUrl);
    TEST_ASSERT(resUtf8.parsedUrl.isValid);
    TEST_ASSERT_EQ(resUtf8.extractedUrl, std::wstring(L"rtsp://192.168.1.50:554/cam1"));
}

TEST_CASE(NetworkStream, DroppedUrlSchemeAndSecurityValidation) {
    // Valid schemes
    TEST_ASSERT(NetworkStreamValidator::ExtractAndValidateDroppedText(L"http://example.com/stream").parsedUrl.isValid);
    TEST_ASSERT(NetworkStreamValidator::ExtractAndValidateDroppedText(L"https://example.com/stream").parsedUrl.isValid);
    TEST_ASSERT(NetworkStreamValidator::ExtractAndValidateDroppedText(L"rtsp://10.0.0.1:554/live").parsedUrl.isValid);
    TEST_ASSERT(NetworkStreamValidator::ExtractAndValidateDroppedText(L"rtmp://live.twitch.tv/app/feed").parsedUrl.isValid);
    TEST_ASSERT(NetworkStreamValidator::ExtractAndValidateDroppedText(L"RtSp://127.0.0.1:554/cam").parsedUrl.isValid);

    // Disallowed schemes produce safe errors
    DroppedUrlResult fileRes = NetworkStreamValidator::ExtractAndValidateDroppedText(L"file:///C:/videos/test.mp4");
    TEST_ASSERT(fileRes.hasUrl);
    TEST_ASSERT(!fileRes.parsedUrl.isValid);
    TEST_ASSERT_EQ(static_cast<int>(fileRes.parsedUrl.error), static_cast<int>(NetworkStreamValidationError::UnsupportedScheme));
    TEST_ASSERT(!fileRes.safeErrorMessage.empty());

    DroppedUrlResult jsRes = NetworkStreamValidator::ExtractAndValidateDroppedText(L"javascript:alert(1)");
    TEST_ASSERT(jsRes.hasUrl);
    TEST_ASSERT(!jsRes.parsedUrl.isValid);
    TEST_ASSERT_EQ(static_cast<int>(jsRes.parsedUrl.error), static_cast<int>(NetworkStreamValidationError::MissingSchemeDelimiter));

    DroppedUrlResult customRes = NetworkStreamValidator::ExtractAndValidateDroppedText(L"custom://myprotocol/stream");
    TEST_ASSERT(customRes.hasUrl);
    TEST_ASSERT(!customRes.parsedUrl.isValid);
    TEST_ASSERT_EQ(static_cast<int>(customRes.parsedUrl.error), static_cast<int>(NetworkStreamValidationError::UnsupportedScheme));
    DroppedUrlResult dataRes = NetworkStreamValidator::ExtractAndValidateDroppedText(L"data:text/html,payload");
    TEST_ASSERT(dataRes.hasUrl);
    TEST_ASSERT(!dataRes.parsedUrl.isValid);

    DroppedUrlResult ftpRes = NetworkStreamValidator::ExtractAndValidateDroppedText(L"ftp://ftp.example.com/media.mkv");
    TEST_ASSERT(ftpRes.hasUrl);
    TEST_ASSERT(!ftpRes.parsedUrl.isValid);

    // Control character injection
    DroppedUrlResult ctrlRes = NetworkStreamValidator::ExtractAndValidateDroppedText(L"https://example.com/stream\x1b[31m.m3u8");
    TEST_ASSERT(ctrlRes.hasUrl);
    TEST_ASSERT(!ctrlRes.parsedUrl.isValid);
    TEST_ASSERT_EQ(static_cast<int>(ctrlRes.parsedUrl.error), static_cast<int>(NetworkStreamValidationError::ContainsControlCharacters));
    TEST_ASSERT(!ctrlRes.safeErrorMessage.empty());
}

TEST_CASE(NetworkStream, DroppedUrlCredentialsAndRedaction) {
    // Dropped URL with embedded credentials must be rejected by default
    std::wstring urlWithCreds = L"http://admin:SecretPass123!@192.168.1.100:8080/live.m3u8";
    DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateDroppedText(urlWithCreds);
    TEST_ASSERT(res.hasUrl);
    TEST_ASSERT(!res.parsedUrl.isValid);
    TEST_ASSERT_EQ(static_cast<int>(res.parsedUrl.error), static_cast<int>(NetworkStreamValidationError::EmbeddedCredentialsNotAllowed));

    // Safe error message MUST NOT leak password
    TEST_ASSERT(!res.safeErrorMessage.empty());
    TEST_ASSERT(res.safeErrorMessage.find(L"SecretPass123!") == std::wstring::npos);

    // Dropped URL with token query parameter in safe error
    std::wstring urlWithToken = L"http://[::1/missing_bracket?token=super_secret_token_12345";
    DroppedUrlResult tokenRes = NetworkStreamValidator::ExtractAndValidateDroppedText(urlWithToken);
    TEST_ASSERT(tokenRes.hasUrl);
    TEST_ASSERT(!tokenRes.parsedUrl.isValid);
    TEST_ASSERT(tokenRes.safeErrorMessage.find(L"super_secret_token_12345") == std::wstring::npos);
}

namespace {

class MockDataObject : public IDataObject {
public:
    LONG refCount = 1;
    std::map<CLIPFORMAT, std::vector<uint8_t>> dataMap;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppvObject = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG count = InterlockedDecrement(&refCount);
        if (count == 0) {
            delete this;
            return 0;
        }
        return count;
    }

    void SetWideText(CLIPFORMAT cf, const std::wstring& text) {
        size_t byteCount = (text.size() + 1) * sizeof(wchar_t);
        std::vector<uint8_t> bytes(byteCount);
        memcpy(bytes.data(), text.c_str(), byteCount);
        dataMap[cf] = std::move(bytes);
    }

    void SetAnsiText(CLIPFORMAT cf, const std::string& text) {
        size_t byteCount = text.size() + 1;
        std::vector<uint8_t> bytes(byteCount);
        memcpy(bytes.data(), text.c_str(), byteCount);
        dataMap[cf] = std::move(bytes);
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override {
        if (!pformatetcIn || !pmedium) return E_POINTER;
        if (!(pformatetcIn->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;

        auto it = dataMap.find(pformatetcIn->cfFormat);
        if (it == dataMap.end()) return DV_E_FORMATETC;

        HGLOBAL hGlob = GlobalAlloc(GHND, it->second.size());
        if (!hGlob) return E_OUTOFMEMORY;

        void* ptr = GlobalLock(hGlob);
        if (ptr) {
            memcpy(ptr, it->second.data(), it->second.size());
            GlobalUnlock(hGlob);
        }

        pmedium->tymed = TYMED_HGLOBAL;
        pmedium->hGlobal = hGlob;
        pmedium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* pformatetc) override {
        if (!pformatetc) return E_POINTER;
        return dataMap.count(pformatetc->cfFormat) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return DATA_S_SAMEFORMATETC; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

} // namespace

TEST_CASE(NetworkStream, DroppedUrlDataObjectFormats) {
    const UINT cfInetUrlW = RegisterClipboardFormatW(L"UniformResourceLocatorW");
    const UINT cfInetUrlA = RegisterClipboardFormatW(L"UniformResourceLocator");
    const UINT cfUriList = RegisterClipboardFormatW(L"text/uri-list");

    // 1. UniformResourceLocatorW format (browser drag)
    {
        MockDataObject* mock = new MockDataObject();
        mock->SetWideText(static_cast<CLIPFORMAT>(cfInetUrlW), L"https://cdn.example.com/video.mp4");
        DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateFromDataObject(mock);
        TEST_ASSERT(res.hasUrl);
        TEST_ASSERT(res.parsedUrl.isValid);
        TEST_ASSERT_EQ(res.extractedUrl, std::wstring(L"https://cdn.example.com/video.mp4"));
        mock->Release();
    }

    // 2. CF_UNICODETEXT format (selected text drag)
    {
        MockDataObject* mock = new MockDataObject();
        mock->SetWideText(CF_UNICODETEXT, L"   rtsp://192.168.1.200:554/cam1 \r\n");
        DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateFromDataObject(mock);
        TEST_ASSERT(res.hasUrl);
        TEST_ASSERT(res.parsedUrl.isValid);
        TEST_ASSERT_EQ(res.extractedUrl, std::wstring(L"rtsp://192.168.1.200:554/cam1"));
        mock->Release();
    }

    // 3. text/uri-list with comments
    {
        MockDataObject* mock = new MockDataObject();
        mock->SetAnsiText(static_cast<CLIPFORMAT>(cfUriList), "# Comments\r\nrtmp://live.twitch.tv/stream\r\n");
        DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateFromDataObject(mock);
        TEST_ASSERT(res.hasUrl);
        TEST_ASSERT(res.parsedUrl.isValid);
        TEST_ASSERT_EQ(res.extractedUrl, std::wstring(L"rtmp://live.twitch.tv/stream"));
        mock->Release();
    }

    // 4. UniformResourceLocator (ANSI)
    {
        MockDataObject* mock = new MockDataObject();
        mock->SetAnsiText(static_cast<CLIPFORMAT>(cfInetUrlA), "http://127.0.0.1:8080/live");
        DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateFromDataObject(mock);
        TEST_ASSERT(res.hasUrl);
        TEST_ASSERT(res.parsedUrl.isValid);
        TEST_ASSERT_EQ(res.extractedUrl, std::wstring(L"http://127.0.0.1:8080/live"));
        mock->Release();
    }

    // 5. CF_TEXT format
    {
        MockDataObject* mock = new MockDataObject();
        mock->SetAnsiText(CF_TEXT, "https://example.com/feed.m3u8");
        DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateFromDataObject(mock);
        TEST_ASSERT(res.hasUrl);
        TEST_ASSERT(res.parsedUrl.isValid);
        TEST_ASSERT_EQ(res.extractedUrl, std::wstring(L"https://example.com/feed.m3u8"));
        mock->Release();
    }

    // 6. Empty / unsupported format data object
    {
        MockDataObject* mock = new MockDataObject();
        DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateFromDataObject(mock);
        TEST_ASSERT(!res.hasUrl);
        mock->Release();
    }

    // 7. Null data object
    {
        DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateFromDataObject(nullptr);
        TEST_ASSERT(!res.hasUrl);
    }
}

TEST_CASE(NetworkStream, DroppedUrlPersistencePolicy) {
    // Default model: persistence disabled
    NetworkStreamModel defaultModel;
    TEST_ASSERT(!defaultModel.IsRecentStreamsEnabled());

    DroppedUrlResult dropRes = NetworkStreamValidator::ExtractAndValidateDroppedText(L"https://example.com/stream.m3u8");
    TEST_ASSERT(dropRes.hasUrl && dropRes.parsedUrl.isValid);

    defaultModel.AddRecentStream(dropRes.parsedUrl.rawUrl);
    TEST_ASSERT(defaultModel.GetRecentStreams().empty());

    // Explicitly enabled root-local model
    NetworkStreamConfig optInConfig;
    optInConfig.enableRecentStreams = true;
    NetworkStreamModel optInModel(optInConfig);
    TEST_ASSERT(optInModel.IsRecentStreamsEnabled());

    optInModel.AddRecentStream(dropRes.parsedUrl.rawUrl);
    TEST_ASSERT_EQ(optInModel.GetRecentStreams().size(), static_cast<size_t>(1));
    TEST_ASSERT_EQ(optInModel.GetRecentStreams()[0].url, std::wstring(L"https://example.com/stream.m3u8"));
}
} // namespace Aperture::Test
