#include "../../src/Diagnostics.cpp"
#include "../../src/Common.h"
#include "../../src/Json.h"

#include <filesystem>
#include <iostream>
#include <fstream>

using namespace Aperture;

int wmain(int argc, wchar_t** argv) {
    const bool crashMode = argc > 1 && wcscmp(argv[1], L"--crash") == 0;
    if (!Diagnostics::Initialize(true)) return 10;

    if (crashMode) {
        Diagnostics::RecordHeartbeat(L"deliberate-crash-test");
        volatile int* invalid = nullptr;
        *invalid = 1;
        return 11;
    }

    Diagnostics::RecordHeartbeat(L"diagnostics-test");
    Diagnostics::Log(AppError::Storage(7, ERROR_ACCESS_DENIED,
        L"Test storage failure", L"diagnostics verification", GetDataDir()));

    const std::wstring directory = GetExeDir() + L"\\diagnostics";
    Diagnostics::RecordStartupMilestone(L"timestamp-only");
    Diagnostics::RecordStartupMilestone(L"explicit-resources", true);
    Diagnostics::WriteStartupReport();
    bool startupReportValid = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory + L"\\perf")) {
        if (entry.path().extension() != L".json") continue;
        std::ifstream input(entry.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto report = JsonValue::parse(text);
        const auto& milestones = report.get("milestones").as_array();
        if (milestones.size() != 2) continue;
        const auto& lightweight = milestones[0];
        const auto& resources = milestones[1];
        startupReportValid = lightweight.get("name").as_string() == "timestamp-only" &&
            !lightweight.get("resourcesCaptured").as_bool(true) &&
            lightweight.get("threadCount").as_int(-1) == 0 &&
            lightweight.get("moduleCount").as_int(-1) == 0 &&
            lightweight.get("workingSetBytes").as_uint64() == 0 &&
            resources.get("name").as_string() == "explicit-resources" &&
            resources.get("resourcesCaptured").as_bool() &&
            resources.get("workingSetBytes").as_uint64() > 0 &&
            resources.get("msSinceStart").as_number(-1) >= lightweight.get("msSinceStart").as_number();
        if (startupReportValid) break;
    }
    const std::wstring logPath = directory + L"\\aperture.log";
    const std::wstring hangDumpPath = Diagnostics::WriteProcessDump(L"hang", nullptr);
    const bool valid = startupReportValid && IsPathUnderRoot(directory) && FileExists(logPath) &&
        !hangDumpPath.empty() && FileExists(hangDumpPath) && IsPathUnderRoot(hangDumpPath);
    Diagnostics::Shutdown();

    if (!valid) {
        std::wcerr << L"Portable diagnostics artifacts were not created under the executable root\n";
        return 12;
    }
    std::wcout << L"portable diagnostics log, startup report and live dump verified\n";
    return 0;
}
