#pragma once

// Behavioral load pipeline tests. Fixtures use actual portable stores and restart
// the production model; no source-text assertions or copied loading algorithms.
namespace LoadingFixture {
std::string Records(size_t count, size_t richEvery = 1) {
    std::string text = "[";
    for (size_t i = 0; i < count; ++i) {
        if (i) text += ',';
        text += "{\"id\":\"" + std::to_string(i) + "\",\"filePath\":\"media/episode" + std::to_string(i) +
            ".mkv\",\"title\":\"日本語 Café\",\"durationSec\":120.5,\"lastPosSec\":12.25,"
            "\"fileSizeBytes\":1234567,\"addedTimestamp\":1700000000,\"status\":\"in_progress\"";
        if (i % richEvery == 0) text += ","
            "\"preferences\":{\"playbackSpeed\":1.25,\"plugin\":true},"
            "\"bookmarks\":[{\"id\":\"scene\",\"timeSec\":42.5,\"label\":\"Scene\",\"color\":\"green\"}],"
            "\"extension\":{\"values\":[1,true,null,\"retained\"]}}";
        else text += '}';
    }
    return text + ']';
}
std::string Bytes(const std::wstring& path) {
    std::string text;
    TEST_ASSERT(ReadTextFile(path, text).is_ok());
    return text;
}
}

TEST_CASE(BackendLoading, StreamingJsonMatchesDomAndRejectsMalformedSuffixes) {
    const std::vector<std::string> valid = {
        "[]", " \r\n [ null,true,false,0,-12.5,1e3 ] \t",
        R"([{"k":1,"k":2},["a\nb","\uD83D\uDE00","a\\b\"c"],"日本語",{}])",
        LoadingFixture::Records(150)
    };
    for (const auto& text : valid) {
        JsonValue expected;
        TEST_ASSERT(JsonValue::parse(text, expected));
        size_t count = 0;
        TEST_ASSERT(JsonValue::consume_array(text, [&](JsonValue& record) {
            TEST_ASSERT(count < expected.as_array().size());
            TEST_ASSERT(record == expected[count]);
            record = JsonValue(); // Consumers may move from or clear the record.
            ++count;
        }));
        TEST_ASSERT_EQ(count, expected.as_array().size());
    }
    const std::string complete = R"([{"id":"a","v":[1,"\u0061"]},false])";
    for (size_t end = 0; end < complete.size(); ++end) {
        TEST_ASSERT(!JsonValue::consume_array(complete.substr(0, end), [](JsonValue&) {}));
    }
    for (const auto* invalid : {"[1,]", "[1] trailing", "[1][2]", "[01]", "[NaN]", "[1e999]", "{}", "null", R"(["\uD800"])"}) {
        TEST_ASSERT(!JsonValue::consume_array(invalid, [](JsonValue&) {}));
    }
    TEST_ASSERT(!JsonValue::consume_array("[]", {}));
}

TEST_CASE(BackendLoading, StreamingJsonRetainsDepthStringAndElementLimits) {
    for (size_t depth : {JsonValue::kMaxJsonDepth, JsonValue::kMaxJsonDepth + 1}) {
        const std::string text = std::string(depth, '[') + '0' + std::string(depth, ']');
        JsonValue dom;
        const bool expected = depth == JsonValue::kMaxJsonDepth;
        TEST_ASSERT_EQ(JsonValue::parse(text, dom), expected);
        TEST_ASSERT_EQ(JsonValue::consume_array(text, [](JsonValue&) {}), expected);
    }
    std::string many = "[0";
    for (size_t i = 1; i < JsonValue::kMaxJsonContainerElements; ++i) many += ",0";
    many += ']';
    size_t seen = 0;
    TEST_ASSERT(JsonValue::consume_array(many, [&](JsonValue&) { ++seen; }));
    TEST_ASSERT_EQ(seen, JsonValue::kMaxJsonContainerElements);
    many.insert(many.size() - 1, ",0");
    seen = 0;
    TEST_ASSERT(!JsonValue::consume_array(many, [&](JsonValue&) { ++seen; }));
    TEST_ASSERT_EQ(seen, JsonValue::kMaxJsonContainerElements);
    std::string large = "[\"" + std::string(JsonValue::kMaxJsonStringBytes, 'x') + "\"]";
    TEST_ASSERT(JsonValue::consume_array(large, [](JsonValue&) {}));
    large.insert(large.size() - 2, 1, 'x');
    TEST_ASSERT(!JsonValue::consume_array(large, [](JsonValue&) {}));
}

TEST_CASE(BackendLoading, ArrayAndWrappedStoresHaveIdenticalPortableRoundtrips) {
    const auto arrayRoot = Root(L"stream-array"), wrappedRoot = Root(L"stream-wrapper");
    auto records = LoadingFixture::Records(1200);
    records.insert(records.size() - 1, R"(,null,12,{"filePath":"https://example.invalid/x"},{"id":"duplicate","filePath":"media/episode0.mkv"},{"filePath":"media/generated.mkv","lastPosSec":999,"durationSec":10})");
    Write(arrayRoot + L"\\data\\library.json", records);
    Write(wrappedRoot + L"\\data\\library.json", "{\"schemaVersion\":1,\"items\":" + records + '}');
    MediaLibrary array, wrapped;
    TEST_ASSERT(array.LoadFromRoot(arrayRoot).is_ok());
    TEST_ASSERT(wrapped.LoadFromRoot(wrappedRoot).is_ok());
    TEST_ASSERT_EQ(array.mediaItems.size(), size_t(1201));
    TEST_ASSERT_EQ(wrapped.mediaItems.size(), array.mediaItems.size());
    for (size_t i = 0; i < 1200; ++i) {
        const auto& item = array.mediaItems[i];
        TEST_ASSERT_EQ(item.id, std::to_string(i));
        TEST_ASSERT_EQ(item.title, std::wstring(L"日本語 Café"));
        TEST_ASSERT_EQ(item.bookmarks.at(0).extraFields.get("color").as_string(), "green");
        TEST_ASSERT(item.preferences->extraFields.get("plugin").as_bool());
        TEST_ASSERT(array.FindById(item.id) == &item);
        TEST_ASSERT(array.FindByPath(item.filePath) == &item);
    }
    TEST_ASSERT_EQ(array.mediaItems.back().lastPosSec, 10.0);
    // Auto-generated IDs include the resolved path, so equalize this one field
    // before comparing independently relocated serializations.
    wrapped.mediaItems.back().id = array.mediaItems.back().id;
    TEST_ASSERT(array.Save().is_ok());
    TEST_ASSERT(wrapped.Save().is_ok());
    TEST_ASSERT_EQ(LoadingFixture::Bytes(arrayRoot + L"\\data\\library.json"), LoadingFixture::Bytes(wrappedRoot + L"\\data\\library.json"));
    const auto relocated = Root(L"stream-relocated");
    fs::copy(arrayRoot + L"\\data", relocated + L"\\data", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    MediaLibrary restarted;
    TEST_ASSERT(restarted.LoadFromRoot(relocated).is_ok());
    TEST_ASSERT_EQ(restarted.mediaItems.size(), size_t(1201));
    TEST_ASSERT(IsPathUnderRoot(restarted.mediaItems.front().filePath, relocated));
}

TEST_CASE(BackendLoading, SmallRecordsAndEscapedDelimitersSurviveCapacityGrowth) {
    const auto root = Root(L"stream-growth");
    std::string text = "[";
    for (int i = 0; i < 257; ++i) {
        if (i) text += ',';
        text += "{\"filePath\":\"x" + std::to_string(i) + "\"";
        if (i % 53 == 0) text += R"(,"title":"[{}] \"quote\" \\ end")";
        text += '}';
    }
    text += ']';
    Write(root + L"\\data\\library.json", text);
    MediaLibrary library;
    TEST_ASSERT(library.LoadFromRoot(root).is_ok());
    TEST_ASSERT_EQ(library.mediaItems.size(), size_t(257));
    TEST_ASSERT_EQ(library.mediaItems.capacity(), library.mediaItems.size());
    for (size_t i = 0; i < library.mediaItems.size(); ++i) {
        const auto& item = library.mediaItems[i];
        TEST_ASSERT_EQ(item.filePath, root + L"\\x" + std::to_wstring(i));
        TEST_ASSERT(library.FindByPath(item.filePath) == &item);
        TEST_ASSERT(library.FindById(item.id) == &item);
        if (i % 53 == 0) TEST_ASSERT_EQ(item.title, std::wstring(L"[{}] \"quote\" \\ end"));
    }
    TEST_ASSERT(library.Save().is_ok());
    const auto saved = LoadingFixture::Bytes(root + L"\\data\\library.json");
    MediaLibrary restarted;
    TEST_ASSERT(restarted.LoadFromRoot(root).is_ok());
    TEST_ASSERT_EQ(restarted.mediaItems.size(), size_t(257));
    TEST_ASSERT(restarted.Save().is_ok());
    TEST_ASSERT_EQ(LoadingFixture::Bytes(root + L"\\data\\library.json"), saved);
}

TEST_CASE(BackendLoading, TruncatedStreamDiscardsRecordsAndRecoversOnlyValidBackup) {
    const auto root = Root(L"stream-corrupt");
    const auto path = root + L"\\data\\library.json";
    const auto corrupt = LoadingFixture::Records(45) + "trailing garbage";
    Write(path, corrupt);
    MediaLibrary failed;
    TEST_ASSERT(!failed.LoadFromRoot(root).is_ok());
    TEST_ASSERT(failed.mediaItems.empty());
    TEST_ASSERT(failed.FindById("0") == nullptr);
    TEST_ASSERT(!failed.Save().libraryStatus.is_ok());
    TEST_ASSERT_EQ(LoadingFixture::Bytes(path), corrupt);
    Write(root + L"\\data\\backups\\library.json.bak.1", LoadingFixture::Records(3));
    MediaLibrary recovered;
    TEST_ASSERT(recovered.LoadFromRoot(root).is_ok());
    TEST_ASSERT(recovered.GetLastLoadResult().libraryRecoveredFromBackup);
    TEST_ASSERT_EQ(recovered.mediaItems.size(), size_t(3));
    TEST_ASSERT(recovered.FindById("44") == nullptr);
    TEST_ASSERT(recovered.Save().is_ok());
    MediaLibrary restart;
    TEST_ASSERT(restart.LoadFromRoot(root).is_ok());
    TEST_ASSERT_EQ(restart.mediaItems.size(), size_t(3));
}

TEST_CASE(BackendLoading, IncrementalEditsSurviveRestartAndCheckpoint) {
    const auto root = Root(L"stream-journal");
    Write(root + L"\\data\\library.json", LoadingFixture::Records(100));
    MediaLibrary library;
    TEST_ASSERT(library.LoadFromRoot(root).is_ok());
    MediaLibrarySaveState writer;
    writer.incrementalLibrary = true;
    auto snapshot = library.CaptureSaveSnapshot();
    TEST_ASSERT(WriteLibrarySaveSnapshot(*snapshot, writer).is_ok());
    const auto checkpoint = LoadingFixture::Bytes(root + L"\\data\\library.json");
    library.mediaItems[7].lastPosSec = 77.25;
    library.mediaItems[7].bookmarks.front().label = L"Changed";
    snapshot = library.CaptureSaveSnapshot(snapshot);
    TEST_ASSERT(WriteLibrarySaveSnapshot(*snapshot, writer).is_ok());
    TEST_ASSERT_EQ(writer.incrementalWriteCount, 1ull);
    TEST_ASSERT_EQ(LoadingFixture::Bytes(root + L"\\data\\library.json"), checkpoint);
    TEST_ASSERT(fs::file_size(root + L"\\data\\library.changes") < checkpoint.size() / 4);
    MediaLibrary restart;
    TEST_ASSERT(restart.LoadFromRoot(root).is_ok());
    TEST_ASSERT_EQ(restart.mediaItems.at(7).lastPosSec, 77.25);
    TEST_ASSERT_EQ(restart.mediaItems.at(7).bookmarks.front().label, std::wstring(L"Changed"));
    TEST_ASSERT_EQ(restart.mediaItems.at(7).extraFields.get("extension").get("values")[3].as_string(), "retained");
    const auto changes = LoadingFixture::Bytes(root + L"\\data\\library.changes");
    Write(root + L"\\data\\library.changes", "corrupt");
    MediaLibrary corruptJournal;
    TEST_ASSERT(!corruptJournal.LoadFromRoot(root).is_ok());
    TEST_ASSERT(corruptJournal.mediaItems.empty());
    TEST_ASSERT(!corruptJournal.Save().libraryStatus.is_ok());
    TEST_ASSERT_EQ(LoadingFixture::Bytes(root + L"\\data\\library.json"), checkpoint);
    Write(root + L"\\data\\library.changes", changes);
    writer.forceLibraryCheckpoint = true;
    TEST_ASSERT(WriteLibrarySaveSnapshot(*snapshot, writer).is_ok());
    TEST_ASSERT(!FileExists(root + L"\\data\\library.changes"));
    MediaLibrary compacted;
    TEST_ASSERT(compacted.LoadFromRoot(root).is_ok());
    TEST_ASSERT_EQ(compacted.mediaItems.at(7).lastPosSec, 77.25);
}

TEST_CASE(BackendLoading, FutureSchemaIsProtectedAndReloadDropsOldRecords) {
    const auto root = Root(L"stream-reload");
    const auto path = root + L"\\data\\library.json";
    Write(path, LoadingFixture::Records(40));
    MediaLibrary library;
    TEST_ASSERT(library.LoadFromRoot(root).is_ok());
    Write(path, "[]");
    TEST_ASSERT(library.LoadFromRoot(root).is_ok());
    TEST_ASSERT(library.mediaItems.empty());
    TEST_ASSERT(library.FindById("39") == nullptr);
    const auto future = "{\"schemaVersion\":999,\"items\":" + LoadingFixture::Records(2) + '}';
    Write(path, future);
    TEST_ASSERT(!library.LoadFromRoot(root).is_ok());
    TEST_ASSERT_EQ(library.GetLastLoadResult().libraryStatus.win32Error, DWORD(ERROR_REVISION_MISMATCH));
    TEST_ASSERT(!library.Save().libraryStatus.is_ok());
    TEST_ASSERT_EQ(LoadingFixture::Bytes(path), future);
}

TEST_CASE(BackendLoading, StreamingLoadBoundsPeakHeapWithoutRetainingGrowthSlack) {
    const auto arrayRoot = Root(L"stream-memory"), wrappedRoot = Root(L"dom-memory");
    const auto records = LoadingFixture::Records(6000, 10);
    Write(arrayRoot + L"\\data\\library.json", records);
    Write(wrappedRoot + L"\\data\\library.json", "{\"schemaVersion\":1,\"items\":" + records + '}');
    auto measure = [](const std::wstring& root) {
        TEST_ASSERT_EQ(stats.live, size_t(0));
        stats = {};
        AllocationStats result;
        {
            MediaLibrary library;
            struct Measure { Measure() { measuring = true; } ~Measure() { measuring = false; } } scope;
            const auto status = library.LoadFromRoot(root);
            measuring = false;
            result = stats;
            TEST_ASSERT(status.is_ok());
            TEST_ASSERT_EQ(library.mediaItems.size(), size_t(6000));
            TEST_ASSERT_EQ(library.mediaItems.capacity(), library.mediaItems.size());
        }
        return result;
    };
    const auto dom = measure(wrappedRoot);
    const auto streamed = measure(arrayRoot);
    TEST_ASSERT(streamed.peak < dom.peak * 95 / 100);
    TEST_ASSERT(streamed.live <= dom.live * 11 / 10);
    std::cout << "  Sparse load heap: streamed=" << streamed.peak << ", DOM=" << dom.peak << " bytes\n";
    // When every record has preferences/bookmarks/extensions the retained typed
    // model dominates memory. That workload must not regress either.
    const auto dense = LoadingFixture::Records(6000);
    Write(arrayRoot + L"\\data\\library.json", dense);
    Write(wrappedRoot + L"\\data\\library.json", "{\"schemaVersion\":1,\"items\":" + dense + '}');
    const auto denseDom = measure(wrappedRoot);
    const auto denseStream = measure(arrayRoot);
    TEST_ASSERT(denseStream.peak <= denseDom.peak * 105 / 100);
    TEST_ASSERT(denseStream.live <= denseDom.live * 11 / 10);
    std::cout << "  Dense load heap: streamed=" << denseStream.peak << ", DOM=" << denseDom.peak << " bytes\n";
}
