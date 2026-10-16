// Behavioral backend gate: actual stores, filesystem operations and allocations.
// The runner places this executable in a disposable directory, beside every write.
#include "../support/TestRunner.h"
#include "MediaLibrary.h"
#include "../benchmark/AllocationProbe.h"
#include <filesystem>

using namespace Aperture;
namespace fs = std::filesystem;

static std::wstring Root(const wchar_t* name) {
    const auto path = GetExeDir() + L"\\" + name;
    fs::create_directories(path + L"\\data");
    return path;
}
static void Write(const std::wstring& path, const std::string& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    TEST_ASSERT(AtomicWriteTextFile(path, bytes).is_ok());
}
#include "TestLibraryLoading.h"

int main(int argc, char** argv) {
    if (!FileExists(GetExeDir() + L"\\.loading-test-root")) {
        std::cerr << "Refusing to write outside an isolated loading test installation.\n";
        return 2;
    }
    return Aperture::Test::RunRegisteredTests(argc, argv, "Aperture portable loading pipeline");
}
