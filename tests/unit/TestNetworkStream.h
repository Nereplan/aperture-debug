#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/NetworkStream.h"
#include "../../src/Json.h"
#include "../../src/TextInputModel.h"
#include <string>
#include <vector>
#include <map>

namespace Aperture::Test {
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

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount); }

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

TEST_CASE(NetworkStream, UrlValidationRedactionAndDragDrop) {
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"http://example.com/live.m3u8"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"https://[::1]:8080/video.mp4"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"rtsp://192.168.1.100:554/stream1"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"rtmp://live.twitch.tv/app/streamkey"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"udp://239.255.0.1:1234"));
    TEST_ASSERT(NetworkStreamValidator::IsValid(L"rtp://224.0.0.1:5004"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"file:///C:/videos/test.mp4"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"javascript:alert(1)"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://999.999.999.999/live"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://[::1/live"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://example.com:65536/stream"));
    TEST_ASSERT(!NetworkStreamValidator::IsValid(L"http://example.com/live\n.m3u8"));

    NetworkStreamValidationOptions defaultOpts;
    defaultOpts.allowCredentials = false;
    NetworkStreamUrl rejectedCreds = NetworkStreamValidator::ValidateAndParse(L"http://user:password@example.com/live.m3u8", defaultOpts);
    TEST_ASSERT(!rejectedCreds.isValid);
    TEST_ASSERT_EQ(static_cast<int>(rejectedCreds.error), static_cast<int>(NetworkStreamValidationError::EmbeddedCredentialsNotAllowed));

    NetworkStreamValidationOptions allowCreds;
    allowCreds.allowCredentials = true;
    NetworkStreamUrl parsedCreds = NetworkStreamValidator::ValidateAndParse(L"http://admin:secret123@example.com:8080/live.m3u8", allowCreds);
    TEST_ASSERT(parsedCreds.isValid);
    TEST_ASSERT(parsedCreds.GetDisplayUrl().find(L"secret123") == std::wstring::npos);

    std::wstring rawCredUrl = L"http://admin:supersecret@192.168.1.50:554/h264?token=xyz123abc&quality=hd";
    std::wstring redacted = NetworkStreamValidator::RedactUrl(rawCredUrl);
    TEST_ASSERT(redacted.find(L"supersecret") == std::wstring::npos);
    TEST_ASSERT(redacted.find(L"xyz123abc") == std::wstring::npos);
    TEST_ASSERT(redacted.find(L"quality=hd") != std::wstring::npos);
    const std::pair<std::wstring, std::wstring> queries[] = {
        {L"?ToKeN=one&key=two&quality=hd", L"?ToKeN=***&key=***&quality=hd"},
        {L"?password=&auth=a=b&sig=z", L"?password=***&auth=***&sig=***"},
        {L"?redirect=https://other.test/?token=private&x=1", L"?redirect=https://other.test/?token=***&x=1"},
        {L"?key=outer?token=inner&pass=tail", L"?key=***&pass=***"},
        {L"?monkey=value&tokenized=keep&x=token=keep", L"?monkey=value&tokenized=keep&x=token=keep"},
        {L"?SECRET=one&pwd=two&api_key=three&signature=four", L"?SECRET=***&pwd=***&api_key=***&signature=***"},
        {L"?&&token=one&&", L"?&&token=***&&"},
        {L"?", L"?"}
    };
    for (const auto& query : queries) {
        const auto url = NetworkStreamValidator::ValidateAndParse(L"https://example.com/live" + query.first + L"#chapter");
        TEST_ASSERT(url.isValid);
        TEST_ASSERT_EQ(url.GetDisplayUrl(), L"https://example.com/live" + query.second + L"#chapter");
        TEST_ASSERT_EQ(url.GetDisplayUrl(false), url.rawUrl);
    }
    TEST_ASSERT(NetworkStreamValidator::GetSafeErrorMessage(
        NetworkStreamValidationError::EmbeddedCredentialsNotAllowed, rawCredUrl).find(L"supersecret") == std::wstring::npos);

    std::wstring uriList = L"# EXTINF:0,Sample\r\nhttps://example.com/main.m3u8\r\nhttps://example.com/fallback.m3u8";
    DroppedUrlResult dropText = NetworkStreamValidator::ExtractAndValidateDroppedText(uriList);
    TEST_ASSERT(dropText.hasUrl && dropText.parsedUrl.isValid);
    TEST_ASSERT_EQ(dropText.extractedUrl, std::wstring(L"https://example.com/main.m3u8"));

    const UINT cfInetUrlW = RegisterClipboardFormatW(L"UniformResourceLocatorW");
    const UINT cfUriList = RegisterClipboardFormatW(L"text/uri-list");
    {
        MockDataObject* mock = new MockDataObject();
        mock->SetWideText(static_cast<CLIPFORMAT>(cfInetUrlW), L"https://cdn.example.com/video.mp4");
        DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateFromDataObject(mock);
        TEST_ASSERT(res.hasUrl && res.parsedUrl.isValid);
        TEST_ASSERT_EQ(res.extractedUrl, std::wstring(L"https://cdn.example.com/video.mp4"));
        mock->Release();
    }
    {
        MockDataObject* mock = new MockDataObject();
        mock->SetAnsiText(static_cast<CLIPFORMAT>(cfUriList), "# Comment\r\nrtmp://live.twitch.tv/stream\r\n");
        DroppedUrlResult res = NetworkStreamValidator::ExtractAndValidateFromDataObject(mock);
        TEST_ASSERT(res.hasUrl && res.parsedUrl.isValid);
        TEST_ASSERT_EQ(res.extractedUrl, std::wstring(L"rtmp://live.twitch.tv/stream"));
        mock->Release();
    }
    TEST_ASSERT(!NetworkStreamValidator::ExtractAndValidateFromDataObject(nullptr).hasUrl);

    TextInputModel editor;
    TEST_ASSERT(editor.InsertText(u"https://aperture.local/live\r\nextra"));
    TEST_ASSERT(editor.GetText() == u"https://aperture.local/live extra");
    TextInputModel::Text clipBuf;
    TextInputModel::ClipboardCallbacks clip{
        [&](const TextInputModel::Text& t) { clipBuf = t; },
        [&]() { return clipBuf; }
    };
    TEST_ASSERT(editor.HandleCtrlShortcut(u'a', clip));
    TEST_ASSERT(editor.HandleCtrlShortcut(u'c', clip));
    TEST_ASSERT(clipBuf == u"https://aperture.local/live extra");
}

TEST_CASE(NetworkStream, RecentStoreIsOptInBoundedRedactedAndSurvivesRestart) {
    ScopedTempDir root;
    NetworkStreamModel disabled;
    disabled.AddRecentStream(L"https://example.com/ignored");
    TEST_ASSERT(disabled.SaveRecentStreams(root.GetPath()).is_ok());
    const auto file = root.SubPath(NetworkStreamModel::kDefaultRecentStreamsFileName);
    TEST_ASSERT(!FileExists(file));

    NetworkStreamConfig config;
    config.enableRecentStreams = true;
    config.maxRecentStreams = 2;
    config.validationOptions.allowCredentials = true;
    NetworkStreamModel store(config);
    const auto credentialUrl = L"https://user:secret@example.com/one?token=alpha";
    store.AddRecentStream(credentialUrl);
    store.AddRecentStream(L"https://example.com/two?key=beta");
    store.AddRecentStream(credentialUrl, L"First");
    store.AddRecentStream(L"file:///rejected");
    TEST_ASSERT(store.SaveRecentStreams(root.GetPath()).is_ok());
    std::string saved;
    TEST_ASSERT(ReadTextFile(file, saved).is_ok());
    TEST_ASSERT(saved.find("secret") == std::string::npos);
    TEST_ASSERT(saved.find("alpha") == std::string::npos);
    TEST_ASSERT(saved.find("beta") == std::string::npos);
    JsonValue json;
    TEST_ASSERT(JsonValue::parse(saved, json));
    TEST_ASSERT_EQ(json.get("recentStreams").as_array().size(), size_t(2));
    TEST_ASSERT_EQ(json.get("recentStreams")[0].get("title").as_string(), "First");

    NetworkStreamModel restarted(config);
    TEST_ASSERT(restarted.LoadRecentStreams(root.GetPath()).is_ok());
    TEST_ASSERT(restarted.SaveRecentStreams(root.GetPath()).is_ok());
    std::string roundtrip;
    TEST_ASSERT(ReadTextFile(file, roundtrip).is_ok());
    TEST_ASSERT_EQ(saved, roundtrip);
    restarted.AddRecentStream(L"https://example.com/three");
    TEST_ASSERT(restarted.SaveRecentStreams(root.GetPath()).is_ok());
    TEST_ASSERT(ReadTextFile(file, roundtrip).is_ok());
    TEST_ASSERT(JsonValue::parse(roundtrip, json));
    TEST_ASSERT_EQ(json.get("recentStreams").as_array().size(), size_t(2));
    TEST_ASSERT_EQ(json.get("recentStreams")[0].get("url").as_string(), "https://example.com/three");
    TEST_ASSERT_EQ(json.get("recentStreams")[1].get("title").as_string(), "First");
}

} // namespace Aperture::Test
