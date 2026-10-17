#include "LibrarySnapshot.h"

namespace Aperture {
namespace {

bool IsCanceled(const LibrarySnapshotBuildOptions& options) {
    return options.cancelRequested && options.cancelRequested->load(std::memory_order_acquire);
}

void Mark(const LibrarySnapshotBuildOptions& options, const wchar_t* phase) { if (options.milestone) options.milestone(phase); }

LibraryQueryOptions InitialQueryOptions(const LibrarySnapshotBuildOptions& input) {
    LibraryQueryOptions options;
    options.groupMode = input.groupMode;
    options.sortField = input.sortField == LibrarySortField::Manual && input.groupMode != LibraryGroupMode::Flat
        ? LibrarySortField::Title : input.sortField;
    options.sortDirection = input.sortDirection;
    if (input.groupMode == LibraryGroupMode::Folder) options.customFolderOrder = input.customFolderOrder;
    return options;
}

} // namespace

std::unique_ptr<LibraryStartupSnapshot> BuildLibraryStartupSnapshot( const LibrarySnapshotBuildOptions& options) {
    auto snapshot = std::make_unique<LibraryStartupSnapshot>();
    snapshot->generation = options.generation;
    snapshot->groupMode = options.groupMode;
    snapshot->preservesExisting = options.preservesExisting;
    snapshot->baseLibraryVersion = options.baseLibraryVersion;
    Mark(options, L"library-worker-start");
    if (IsCanceled(options)) { snapshot->canceled = true; return snapshot; }
    snapshot->loadStatus = snapshot->library.LoadFromRoot( options.rootDir, [&](const wchar_t* phase) { Mark(options, phase); });
    if (IsCanceled(options)) { snapshot->canceled = true; return snapshot; }
    if (!snapshot->loadStatus.is_ok()) { return snapshot; }
    // Promotion repairs only the two swapped lookup entries; the startup path
    // no longer performs a second O(n) RebuildPathIndex pass.
    snapshot->library.PromoteMostRecentlyWatched();
    Mark(options, L"library-indexes-ready");
    if (IsCanceled(options)) { snapshot->canceled = true; return snapshot; }
    LibraryQueryEngine queryEngine;
    snapshot->initialQuery = InitialQueryOptions(options);
    queryEngine.QueryView(snapshot->library, snapshot->initialQuery);
    snapshot->initialProjection = queryEngine.ReleaseCachedView();
    Mark(options, L"library-projection-ready");
    if (IsCanceled(options)) {
        snapshot->canceled = true;
        snapshot->initialProjection = LibraryViewProjection();
        return snapshot;
    }
    snapshot->publishable = true;
    return snapshot;
}

uint64_t LibrarySnapshotMailbox::BeginGeneration() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
    accepting_ = true;
    activeGeneration_ = ++nextGeneration_;
    return activeGeneration_;
}

bool LibrarySnapshotMailbox::Publish(std::unique_ptr<LibraryStartupSnapshot> snapshot) {
    if (!snapshot) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_ || snapshot->generation != activeGeneration_) return false;
    pending_ = std::move(snapshot);
    return true;
}

std::unique_ptr<LibraryStartupSnapshot> LibrarySnapshotMailbox::Take(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_ || generation != activeGeneration_ || !pending_ || pending_->generation != generation) { return nullptr; }
    accepting_ = false;
    return std::move(pending_);
}

void LibrarySnapshotMailbox::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
    pending_.reset();
    activeGeneration_ = ++nextGeneration_;
}

bool LibrarySnapshotMailbox::HasPending() const { std::lock_guard<std::mutex> lock(mutex_); return pending_ != nullptr; }

uint64_t LibrarySnapshotMailbox::ActiveGeneration() const { std::lock_guard<std::mutex> lock(mutex_); return activeGeneration_; }
} // namespace Aperture
