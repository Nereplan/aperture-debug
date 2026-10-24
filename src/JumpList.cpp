#include "JumpList.h"

#include <objbase.h>
#include <shobjidl.h>
#include <shlobj.h>

namespace Aperture {

namespace {
// CLSID_DestinationList (ShObjIdl_core.idl). The SDK headers emit the
// ICustomDestinationList interface but not this coclass's CLSID constant, so
// it is defined locally.
constexpr CLSID kDestinationListClsid = { 0x77f10cf0, 0x3db5, 0x4966, { 0xb5, 0x20, 0xb7, 0xc5, 0x4f, 0xd3, 0x5e, 0xd6 } };
}

void UpdateJumpList(const std::vector<std::wstring>& recentPaths) {
    ICustomDestinationList* destinationList = nullptr;
    if (FAILED(CoCreateInstance(kDestinationListClsid, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&destinationList))) || !destinationList) {
        return;
    }
    // BeginList/CommitList replaces the whole list; an empty path list simply
    // skips AppendCategory so the published list drops the "Recent" category.
    UINT maxSlots = 0;
    IObjectArray* removed = nullptr;
    if (SUCCEEDED(destinationList->BeginList(&maxSlots, IID_PPV_ARGS(&removed)))) {
        if (!recentPaths.empty()) {
            IObjectCollection* collection = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&collection))) && collection) {
                for (const std::wstring& path : recentPaths) {
                    if (path.empty()) continue;
                    IShellItem* item = nullptr;
                    // Skip missing/unresolvable paths: a jump list entry that
                    // cannot be resolved to a shell item is worthless.
                    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
                        collection->AddObject(item);
                        item->Release();
                    }
                }
                IObjectArray* categoryItems = nullptr;
                if (SUCCEEDED(collection->QueryInterface(IID_PPV_ARGS(&categoryItems))) && categoryItems) {
                    destinationList->AppendCategory(L"Recent", categoryItems);
                    categoryItems->Release();
                }
                collection->Release();
            }
        }
        destinationList->CommitList();
        if (removed) removed->Release();
    }
    destinationList->Release();
}

} // namespace Aperture
