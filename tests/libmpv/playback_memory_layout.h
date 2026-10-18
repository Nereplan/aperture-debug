// Optional post-measurement diagnostics. VA/heap categories overlap process
// counters and must not be added to RAM or GPU figures.
static void ReportPlaybackMemoryLayout() {
    struct Summary { DWORD cb; SIZE_T allocated, committed, reserved, maxReserve; };
    using SummaryFn = BOOL (WINAPI*)(HANDLE, DWORD, Summary*);
    auto summarize = reinterpret_cast<SummaryFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary"));
    const DWORD count = GetProcessHeaps(0, nullptr);
    std::vector<HANDLE> heaps(count);
    const DWORD actual = count ? GetProcessHeaps(count, heaps.data()) : 0;
    uint64_t allocated = 0, committed = 0, reserved = 0;
    unsigned measured = 0;
    if (summarize && actual <= count) for (DWORD i = 0; i < actual; ++i) {
        Summary s{}; s.cb = sizeof(s);
        if (!summarize(heaps[i], 0, &s)) continue;
        ++measured; allocated += s.allocated; committed += s.committed; reserved += s.reserved;
    }
    uint64_t privateVa = 0, imageVa = 0, mappedVa = 0;
    MEMORY_BASIC_INFORMATION region{};
    uintptr_t address = 0;
    while (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) {
        if (region.State == MEM_COMMIT) {
            if (region.Type == MEM_PRIVATE) privateVa += region.RegionSize;
            else if (region.Type == MEM_IMAGE) imageVa += region.RegionSize;
            else if (region.Type == MEM_MAPPED) mappedVa += region.RegionSize;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= address) break;
        address = next;
    }
    const auto* base = reinterpret_cast<const unsigned char*>(g_hMod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    uint64_t writableImage = 0, bss = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sections[i].Characteristics & IMAGE_SCN_MEM_WRITE) writableImage += sections[i].Misc.VirtualSize;
        if (sections[i].Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) bss += sections[i].Misc.VirtualSize;
    }
    printf("HEAP_LAYOUT {\"post_measurement\":true,\"heaps\":%lu,\"heaps_measured\":%u,"
        "\"heap_summary_available\":%s,\"observed_heap_allocated\":%llu,\"observed_heap_committed\":%llu,"
        "\"observed_heap_reserved\":%llu,\"private_va_committed\":%llu,\"image_va_committed\":%llu,"
        "\"mapped_va_committed\":%llu,\"mpv_writable_image_virtual\":%llu,\"mpv_bss_virtual\":%llu}\n",
        actual, measured, summarize ? "true" : "false", static_cast<unsigned long long>(allocated),
        static_cast<unsigned long long>(committed), static_cast<unsigned long long>(reserved),
        static_cast<unsigned long long>(privateVa), static_cast<unsigned long long>(imageVa),
        static_cast<unsigned long long>(mappedVa), static_cast<unsigned long long>(writableImage), static_cast<unsigned long long>(bss));
}
