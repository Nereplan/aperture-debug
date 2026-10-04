#include "ThumbnailMigration.h"
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <numeric>

using namespace Aperture;
namespace fs = std::filesystem;
static size_t checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output.write(bytes.data(), bytes.size());
    Check(output.good(), "Fixture write failed");
}
std::string Key(unsigned n, bool seek = false) {
    std::ostringstream key; key << (seek ? "thumb_" : "hub_") << std::hex << std::setfill('0') << std::setw(16) << n << ".bin";
    return key.str();
}
std::wstring Wide(const std::string& value) { return {value.begin(), value.end()}; }
std::vector<uint8_t> Pixels(unsigned seed, bool alpha = false, int w = 320, int h = 180) {
    std::mt19937 random(seed); std::vector<uint8_t> pixels(static_cast<size_t>(w)*h*4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const size_t i = (static_cast<size_t>(y)*w+x)*4;
        const int grain = static_cast<int>(random()%9)-4;
        pixels[i] = static_cast<uint8_t>(std::clamp(x*210/w + grain, 0, 255));
        pixels[i+1] = static_cast<uint8_t>(std::clamp(y*190/h + grain, 0, 255));
        pixels[i+2] = static_cast<uint8_t>(std::clamp((x+y)*180/(w+h) + grain, 0, 255));
        pixels[i+3] = alpha ? static_cast<uint8_t>((x+y)%256) : 255;
    }
    return pixels;
}
std::string Legacy(const std::vector<uint8_t>& pixels, bool raw = false, int w = 320, int h = 180) {
    ThumbnailDiskHeader header; header.version = raw ? 1 : 2;
    header.width = w; header.height = h; header.stride = w*4; header.timestamp = 12.5;
    std::vector<uint8_t> encoded;
    if (raw) encoded = pixels;
    else Check(ThumbnailCodec::EncodeImage(pixels.data(), w, h, false, encoded), "Legacy PNG fixture");
    header.pixelBytes = static_cast<uint32_t>(encoded.size());
    std::string result(sizeof(header)+encoded.size(), '\0');
    std::memcpy(result.data(), &header, sizeof(header));
    std::memcpy(result.data()+sizeof(header), encoded.data(), encoded.size());
    return result;
}
double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    Check(a.size() == b.size(), "Decoded pixel count");
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) if (i%4 != 3) { double d = double(a[i])-b[i]; sum += d*d; }
    return sum == 0 ? 99 : 10*std::log10(255.0*255/(sum/(a.size()/4*3)));
}

void SelfTest(const fs::path& root) {
    fs::create_directories(root);
    bool staCodecPassed = false;
    std::thread sta([&]() {
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return;
        auto pixels = Pixels(19);
        std::string bytes;
        staCodecPassed = ThumbnailCodec::Encode(pixels,320,180,0,bytes);
        CoUninitialize(); // Thread-local codec still owns COM until thread exit.
        ThumbnailDiskHeader header; std::vector<uint8_t> decoded;
        staCodecPassed = staCodecPassed && ThumbnailCodec::Decode(bytes,header,decoded);
    });
    sta.join(); Check(staCodecPassed,"Codec lifetime on an STA UI thread");
    const auto original = Pixels(731);
    std::string encoded; ThumbnailDiskHeader header; std::vector<uint8_t> decoded;
    Check(ThumbnailCodec::Encode(original, 320, 180, 12.5, encoded), "Opaque encode");
    Check(ThumbnailCodec::Decode(encoded, header, decoded) && header.version == 3 && header.timestamp == 12.5, "JPEG roundtrip metadata");
    Check(Psnr(original, decoded) > 32, "JPEG quality on seeded gradients and grain");
    Check(encoded.size() < original.size()/4, "Opaque compression regression");
    for (bool checkerboard : {false, true}) {
        std::vector<uint8_t> graphic(320 * 180 * 4, 255);
        for (int y = 0; y < 180; ++y) for (int x = 0; x < 320; ++x) {
            const size_t offset = (static_cast<size_t>(y) * 320 + x) * 4;
            const uint8_t value = checkerboard ? ((x + y) % 2 ? 0 : 255) : 32;
            graphic[offset] = graphic[offset + 1] = graphic[offset + 2] = value;
        }
        std::vector<uint8_t> jpeg;
        Check(ThumbnailCodec::EncodeImage(graphic.data(), 320, 180, true, jpeg), "Graphic JPEG baseline");
        std::string adaptive;
        Check(ThumbnailCodec::Encode(graphic, 320, 180, 0, adaptive), "Adaptive graphic encode");
        Check(adaptive.size() < jpeg.size() + sizeof(ThumbnailDiskHeader), "Graphics use fewer bytes than fixed JPEG");
        Check(ThumbnailCodec::Decode(adaptive, header, decoded) && header.version == 2 && decoded == graphic,
              "Smaller opaque graphics remain lossless");
        std::string upgraded;
        Check(!ThumbnailCodec::Upgrade(adaptive, upgraded), "Optimal PNG is not rewritten");
    }
    const auto transparent = Pixels(42, true);
    std::string alpha;
    Check(ThumbnailCodec::Encode(transparent, 320, 180, 0, alpha), "Transparent encode");
    Check(ThumbnailCodec::Decode(alpha, header, decoded) && header.version == 2 && decoded == transparent, "Alpha must be lossless");
    for (bool raw : {false, true}) {
        auto legacy = Legacy(original, raw); std::string smaller;
        Check(ThumbnailCodec::Upgrade(legacy, smaller) && smaller.size() < legacy.size(), "Legacy migration reduces storage");
        Check(ThumbnailCodec::Decode(smaller, header, decoded) && header.width == 320 && header.height == 180, "Migrated frame dimensions");
    }
    std::string unused;
    Check(!ThumbnailCodec::Upgrade(encoded, unused), "JPEG must never be recompressed");
    Check(!ThumbnailCodec::Encode(std::vector<uint8_t>(5), 320, 180, 0, unused), "Short pixel buffer rejected");
    Check(!ThumbnailCodec::Encode(original, 320, 180, NAN, unused), "Invalid timestamp rejected");
    auto legacy = Legacy(original);
    for (size_t length : {size_t(0), size_t(31), size_t(32), legacy.size()-1})
        Check(!ThumbnailCodec::Decode(legacy.substr(0,length), header, decoded), "Truncated cache rejected");
    for (int field : {0,1,2,3,4,7}) {
        auto corrupt = legacy; uint32_t bad = 0xffffffff;
        std::memcpy(corrupt.data()+field*4, &bad, 4);
        Check(!ThumbnailCodec::Decode(corrupt, header, decoded), "Malformed cache header rejected");
    }
    auto mismatch = legacy; uint32_t narrow = 319; std::memcpy(mismatch.data()+8,&narrow,4);
    narrow *= 4; std::memcpy(mismatch.data()+16,&narrow,4);
    Check(!ThumbnailCodec::Decode(mismatch,header,decoded), "PNG/header dimensions must match");
    auto damaged = legacy; damaged[40] ^= 0x7f;
    Check(!ThumbnailCodec::Decode(damaged,header,decoded), "Corrupt payload rejected");

    CacheBudgetConfig config; config.totalCeilingBytes = 16000; config.thumbnailBudgetBytes = 12000;
    const auto cacheRoot = (root/L"budget").wstring();
    const auto hub = Wide(Key(1));
    Check(AtomicWriteCacheFile(CacheClass::Thumbnails,hub,std::string(8000,'h'),config,cacheRoot).is_ok(), "Hub fits its allowance");
    for (unsigned i=0; i<100; ++i)
        Check(AtomicWriteCacheFile(CacheClass::Thumbnails,Wide(Key(i,true)),std::string(900,'s'),config,cacheRoot).is_ok(), "Scrubbing writes within preview allowance");
    std::string read;
    Check(ReadCacheFile(CacheClass::Thumbnails,hub,read,cacheRoot).is_ok() && read == std::string(8000,'h'), "Scrubbing must preserve library thumbnail");
    Check(QueryCacheStats(cacheRoot).classStats[CacheClass::Thumbnails].totalBytes <= 12000, "Combined budget respected");
    Check(AtomicWriteCacheFile(CacheClass::Thumbnails,hub,std::string(8000,'n'),config,cacheRoot).is_ok(), "Same-size replacement charges no extra space");
    Check(ReadCacheFile(CacheClass::Thumbnails,hub,read,cacheRoot).is_ok() && read[0]=='n', "Replacement is readable");
    auto aliasRoot = cacheRoot;
    std::transform(aliasRoot.begin(),aliasRoot.end(),aliasRoot.begin(),[](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    Check(AtomicWriteCacheFile(CacheClass::Thumbnails,hub,std::string(8000,'a'),config,aliasRoot).is_ok(), "Windows path casing does not double-charge replacement");
    Check(QueryCacheStats(cacheRoot).totalBytes<=config.thumbnailBudgetBytes,"Case-alias write stays within allowance");
    auto reduced = config; reduced.totalCeilingBytes = 8500;
    Check(!AtomicWriteCacheFile(CacheClass::Thumbnails,Wide(Key(999,true)),std::string(900,'s'),reduced,cacheRoot).is_ok(), "Preview write rejected when it would evict protected hub");
    Check(ReadCacheFile(CacheClass::Thumbnails,hub,read,cacheRoot).is_ok(), "Hub survives total-budget pressure");
    auto disabled = config; disabled.thumbnailBudgetBytes=0; SetGlobalCacheBudgets(disabled);
    Check(!AtomicWriteCacheFile(CacheClass::Thumbnails,hub,"x",GetGlobalCacheBudgets(),cacheRoot).is_ok(), "Active disabled setting honored");
    SetGlobalCacheBudgets(CacheBudgetConfig{});

    const auto migrationRoot = (root/L"migration").wstring();
    const auto cache = fs::path(migrationRoot)/L"cache"/L"thumbnails";
    Write(cache/hub,legacy);
    ThumbnailMigrationQueue migrations;
    Check(migrations.Queue(migrationRoot,hub,legacy), "Migration queued"); migrations.WaitIdle();
    auto migrated=Read(cache/hub);
    Check(migrated.size()<legacy.size() && ThumbnailCodec::Decode(migrated,header,decoded) && header.version==3, "Background migration committed");
    Check(!AtomicWriteCacheFile(CacheClass::Thumbnails,hub,encoded,CacheBudgetConfig{},migrationRoot,&legacy).is_ok(), "Stale migration cannot overwrite newer content");
    Check(ClearDisposableCache(migrationRoot).is_ok(), "Clear cache");
    Check(!AtomicWriteCacheFile(CacheClass::Thumbnails,hub,encoded,CacheBudgetConfig{},migrationRoot,&legacy).is_ok(), "Migration cannot resurrect cleared cache");
    Write(cache/hub,legacy);
    HANDLE locked=CreateFileW((cache/hub).c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(locked!=INVALID_HANDLE_VALUE,"Lock migration fixture");
    Check(migrations.Queue(migrationRoot,hub,legacy),"Locked migration queued"); migrations.WaitIdle(); CloseHandle(locked);
    Check(Read(cache/hub)==legacy,"Failed migration preserves original bytes");
    Check(migrations.Queue(migrationRoot,hub,legacy),"Migration retry queued"); migrations.WaitIdle();
    Check(Read(cache/hub).size()<legacy.size(),"Migration retries after lock released");
    Write(cache/Wide(Key(2)),alpha);
    migrations.Queue(migrationRoot,Wide(Key(2)),alpha); migrations.WaitIdle();
    Check(Read(cache/Wide(Key(2)))==alpha,"Unhelpful conversion preserves transparent cache");
    Check(!migrations.Queue(migrationRoot,Wide(Key(2)),alpha),"Optimal PNG does not re-encode on repeated reads");
    auto changedAlpha = alpha;
    std::memcpy(&header,changedAlpha.data(),sizeof(header)); header.timestamp = 1;
    std::memcpy(changedAlpha.data(),&header,sizeof(header));
    Write(cache/Wide(Key(2)),changedAlpha);
    Check(migrations.Queue(migrationRoot,Wide(Key(2)),changedAlpha),"Changed content at the same key and size is rechecked");
    migrations.WaitIdle();
    Check(!migrations.Queue(migrationRoot,Wide(Key(2)),changedAlpha),"Changed optimal content is remembered");
    for (unsigned i = 1000; i < 1129; ++i) {
        Check(migrations.Queue(migrationRoot,Wide(Key(i)),alpha),"Bounded migration history accepts new keys");
        migrations.WaitIdle();
    }
    Check(migrations.Queue(migrationRoot,Wide(Key(2)),changedAlpha),"Old migration fingerprints expire from bounded history");
    migrations.WaitIdle();

    const auto concurrentRoot=(root/L"concurrent").wstring();
    std::atomic<unsigned> failures{0}; std::vector<std::thread> writers;
    for (unsigned t=0;t<6;++t) writers.emplace_back([&,t]() {
        for (unsigned i=0;i<70;++i) if (!AtomicWriteCacheFile(CacheClass::Thumbnails,Wide(Key(t*100+i,true)),std::string(500,'x'),config,concurrentRoot).is_ok()) ++failures;
    });
    for (auto& writer:writers) writer.join();
    Check(failures==0 && QueryCacheStats(concurrentRoot).totalBytes<=3000,"Concurrent writers obey shared preview allowance");
    Check(!AtomicWriteCacheFile(CacheClass::Thumbnails,L"..\\..\\data\\settings.json","bad",config,concurrentRoot).is_ok(),"Unsafe path rejected");
    Check(!AtomicWriteCacheFile(CacheClass::Thumbnails,L"..\\shaders\\wrong-class.bin","bad",config,concurrentRoot).is_ok(),"Cross-class traversal cannot bypass accounting");
    // A cache bigger than the former 8192-file scan cap must still be counted.
    const auto many=root/L"many"/L"cache"/L"generic";
    for (unsigned i=0;i<8200;++i) Write(many/(std::to_wstring(i)+L".bin"),"x");
    Check(QueryCacheStats((root/L"many").wstring()).totalFiles==8200,"Accounting includes files beyond the old scan cap");
    CacheBudgetConfig manyConfig; manyConfig.genericBudgetBytes=8199;
    Check(AtomicWriteCacheFile(CacheClass::Generic,L"incoming.bin","new",manyConfig,(root/L"many").wstring()).is_ok(),"Large-cache eviction succeeds");
    Check(QueryCacheStats((root/L"many").wstring()).totalBytes<=8199,"Large-cache budget counts every file");
    std::cout<<"PASS thumbnail codec, migration, budgets, concurrent writers, corruption and containment: "<<checks<<" assertions\n";
}

double Percentile(std::vector<double> values, double p) {
    std::sort(values.begin(),values.end()); return values[static_cast<size_t>((values.size()-1)*p)];
}
void Benchmark(const fs::path& source, const fs::path& output) {
    fs::create_directories(output/L"cache"/L"thumbnails");
    std::ofstream csv(output/L"codec.csv");
    csv<<"file,old_bytes,new_bytes,width,height,psnr_rgb,encode_ms,old_decode_ms,new_decode_ms\n";
    uint64_t before=0,after=0; size_t count=0; std::vector<double> encTimes,oldTimes,newTimes,qualities;
    ThumbnailMigrationQueue migrations;
    auto elapsed=[](auto start) { return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); };
    for(const auto& file:fs::directory_iterator(source)) {
        if(file.path().extension()!=L".bin") continue;
        const auto bytes=Read(file.path()); ThumbnailDiskHeader header; std::vector<uint8_t> pixels,decoded;
        auto start=std::chrono::steady_clock::now();
        Check(ThumbnailCodec::Decode(bytes,header,pixels),"Corpus original decodes"); const auto oldMs=elapsed(start);
        start=std::chrono::steady_clock::now(); std::string encoded;
        Check(ThumbnailCodec::Encode(pixels,header.width,header.height,header.timestamp,encoded),"Corpus encodes"); const auto encodeMs=elapsed(start);
        if(header.version==3 || encoded.size()>=bytes.size()) encoded=bytes;
        start=std::chrono::steady_clock::now(); ThumbnailDiskHeader restored;
        Check(ThumbnailCodec::Decode(encoded,restored,decoded),"Corpus candidate decodes"); const auto newMs=elapsed(start);
        Check(restored.width==header.width && restored.height==header.height && restored.timestamp==header.timestamp,"Corpus metadata survives");
        const double quality=Psnr(pixels,decoded);
        csv<<file.path().filename().string()<<','<<bytes.size()<<','<<encoded.size()<<','<<header.width<<','<<header.height<<','<<quality<<','<<encodeMs<<','<<oldMs<<','<<newMs<<'\n';
        before+=bytes.size(); after+=encoded.size(); ++count;
        encTimes.push_back(encodeMs); oldTimes.push_back(oldMs); newTimes.push_back(newMs); qualities.push_back(quality);
        // Exercise the production migration worker with the real copied cache.
        Write(output/L"cache"/L"thumbnails"/file.path().filename(),bytes);
        if (header.version != 3)
            while(!migrations.Queue(output.wstring(),file.path().filename().wstring(),bytes)) migrations.WaitIdle();
    }
    migrations.WaitIdle();
    Check(count>0,"Corpus is nonempty");
    const auto actual=QueryCacheStats(output.wstring());
    Check(actual.totalFiles==count && actual.totalBytes==after,"Background conversion matches codec benchmark; no duplicate entries");
    // Independent second pass models restart: load every persisted frame again.
    for(const auto& file:fs::directory_iterator(output/L"cache"/L"thumbnails")) {
        ThumbnailDiskHeader header; std::vector<uint8_t> pixels;
        Check(ThumbnailCodec::Decode(Read(file.path()),header,pixels),"Persisted corpus reload");
    }
    std::ofstream summary(output/L"summary.json");
    summary<<"{\n\"count\":"<<count<<",\n\"beforeBytes\":"<<before<<",\n\"afterBytes\":"<<after
        <<",\n\"encodeP95Ms\":"<<Percentile(encTimes,.95)<<",\n\"oldDecodeP95Ms\":"<<Percentile(oldTimes,.95)
        <<",\n\"newDecodeP95Ms\":"<<Percentile(newTimes,.95)<<",\n\"psnrMedian\":"<<Percentile(qualities,.5)
        <<",\n\"psnrMin\":"<<Percentile(qualities,0)<<"\n}\n";
    std::cout<<"PASS real cache: "<<count<<" images, "<<before<<" -> "<<after<<" bytes, decode p95 "
        <<Percentile(oldTimes,.95)<<" -> "<<Percentile(newTimes,.95)<<" ms\n";
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc==3 && std::wstring(argv[1])==L"--test") SelfTest(argv[2]);
        else if(argc==4 && std::wstring(argv[1])==L"--corpus") Benchmark(argv[2],argv[3]);
        else throw std::runtime_error("Usage: --test TEMP_ROOT | --corpus COPIED_CACHE OUTPUT_ROOT");
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
