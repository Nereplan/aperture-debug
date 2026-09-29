#include "CacheStorage.h"
#include <filesystem>
#include <iostream>

using namespace Aperture;
namespace fs = std::filesystem;
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    std::vector<std::pair<std::wstring,std::string>> corpus;
    for (const auto& entry : fs::directory_iterator(argv[1])) {
        if (entry.path().extension() != L".bin") continue;
        std::ifstream file(entry.path(),std::ios::binary);
        corpus.emplace_back(entry.path().filename().wstring(),std::string(std::istreambuf_iterator<char>(file),{}));
    }
    if(corpus.empty()) return 3;
    const auto root=std::wstring(argv[2]);
    const auto start=std::chrono::steady_clock::now();
    for(const auto& file:corpus)
        if(!AtomicWriteCacheFile(CacheClass::Thumbnails,file.first,file.second,CacheBudgetConfig{},root).is_ok()) return 4;
    const auto written=std::chrono::steady_clock::now();
    for(const auto& file:corpus) {
        std::string data;
        if(!ReadCacheFile(CacheClass::Thumbnails,file.first,data,root).is_ok() || data!=file.second) return 5;
    }
    const auto read=std::chrono::steady_clock::now();
    auto stats=QueryCacheStats(root);
    if(stats.totalFiles!=corpus.size()) return 6;
    std::cout<<"{\"count\":"<<corpus.size()<<",\"bytes\":"<<stats.totalBytes
        <<",\"writeMs\":"<<std::chrono::duration<double,std::milli>(written-start).count()
        <<",\"readMs\":"<<std::chrono::duration<double,std::milli>(read-written).count()<<"}\n";
    return 0;
}
