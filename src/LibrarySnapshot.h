#pragma once

#include "LibraryQuery.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Aperture {

// A worker-owned startup package. It is constructed privately, published once,
// and moved into App on the UI thread; no renderer ever observes partial state.
struct LibraryStartupSnapshot {
    uint64_t generation = 0;
    LibraryGroupMode groupMode = LibraryGroupMode::Flat;
    MediaLibrary library;
    LibraryQueryProjection initialProjection;
    std::vector<size_t> visibleIndices;
    StorageStatus loadStatus = StorageStatus::Ok();
    bool publishable = false, canceled = false, preservesExisting = false;
    uint64_t baseLibraryVersion = 0;
};

struct LibrarySnapshotBuildOptions {
    uint64_t generation = 0;
    LibraryGroupMode groupMode = LibraryGroupMode::Flat;
    std::wstring rootDir;
    bool preservesExisting = false;
    uint64_t baseLibraryVersion = 0;
    const std::atomic<bool>* cancelRequested = nullptr;
    std::function<void(const wchar_t* phase)> milestone;
};

std::unique_ptr<LibraryStartupSnapshot> BuildLibraryStartupSnapshot( const LibrarySnapshotBuildOptions& options);

// Small generation-aware mailbox shared by the worker and UI thread. A stale producer cannot replace a newer request, and Cancel
// invalidates both queued and in-flight publications.
class LibrarySnapshotMailbox {
public:
    uint64_t BeginGeneration();
    bool Publish(std::unique_ptr<LibraryStartupSnapshot> snapshot);
    std::unique_ptr<LibraryStartupSnapshot> Take(uint64_t generation);
    void Cancel();
    bool HasPending() const;
    uint64_t ActiveGeneration() const;

private:
    mutable std::mutex mutex_;
    uint64_t nextGeneration_ = 0, activeGeneration_ = 0;
    bool accepting_ = false;
    std::unique_ptr<LibraryStartupSnapshot> pending_;
};

} // namespace Aperture
