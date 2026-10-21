#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Accessibility.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "uiautomationcore.lib")

namespace Aperture {

namespace {

bool ReadChildId(const VARIANT& value, LONG& childId) {
    if (value.vt == VT_I4) { childId = value.lVal; return true; }
    if (value.vt == VT_UI4 && value.ulVal <= static_cast<ULONG>(LONG_MAX)) { childId = static_cast<LONG>(value.ulVal); return true; }
    return false;
}

LONG RoleToControlTypeId(LONG role) {
    switch (role) {
    case ROLE_SYSTEM_PUSHBUTTON:    return UIA_ButtonControlTypeId;
    case ROLE_SYSTEM_CHECKBUTTON:   return UIA_CheckBoxControlTypeId;
    case ROLE_SYSTEM_RADIOBUTTON:   return UIA_RadioButtonControlTypeId;
    case ROLE_SYSTEM_SLIDER:        return UIA_SliderControlTypeId;
    case ROLE_SYSTEM_PROGRESSBAR:   return UIA_ProgressBarControlTypeId;
    case ROLE_SYSTEM_TEXT:          return UIA_EditControlTypeId;
    case ROLE_SYSTEM_STATICTEXT:    return UIA_TextControlTypeId;
    case ROLE_SYSTEM_PAGETAB:       return UIA_TabItemControlTypeId;
    case ROLE_SYSTEM_PAGETABLIST:   return UIA_TabControlTypeId;
    case ROLE_SYSTEM_LIST:          return UIA_ListControlTypeId;
    case ROLE_SYSTEM_LISTITEM:      return UIA_ListItemControlTypeId;
    case ROLE_SYSTEM_COMBOBOX:      return UIA_ComboBoxControlTypeId;
    case ROLE_SYSTEM_GROUPING:      return UIA_GroupControlTypeId;
    case ROLE_SYSTEM_PANE:          return UIA_PaneControlTypeId;
    case ROLE_SYSTEM_LINK:          return UIA_HyperlinkControlTypeId;
    case ROLE_SYSTEM_MENUITEM:      return UIA_MenuItemControlTypeId;
    case ROLE_SYSTEM_TOOLBAR:       return UIA_ToolBarControlTypeId;
    case ROLE_SYSTEM_CLIENT:        return UIA_PaneControlTypeId;
    case ROLE_SYSTEM_WINDOW:        return UIA_WindowControlTypeId;
    case ROLE_SYSTEM_STATUSBAR:    return UIA_StatusBarControlTypeId;
    case ROLE_SYSTEM_ALERT:        return UIA_PaneControlTypeId;
    default:                        return UIA_CustomControlTypeId;
    }
}

template<VARTYPE Type, typename T>
SAFEARRAY* CreateScalarArray(const T* values, size_t count) {
    if (count > LONG_MAX || (!values && count)) return nullptr;
    SAFEARRAYBOUND bound{static_cast<ULONG>(count), 0};
    SAFEARRAY* array = SafeArrayCreate(Type, 1, &bound);
    if (!array || !count) return array;
    void* data = nullptr;
    if (FAILED(SafeArrayAccessData(array, &data))) { SafeArrayDestroy(array); return nullptr; }
    std::copy_n(values, count, static_cast<T*>(data));
    if (FAILED(SafeArrayUnaccessData(array))) { SafeArrayDestroy(array); return nullptr; }
    return array;
}

SAFEARRAY* CreateIntArray(const int* values, size_t count) { return CreateScalarArray<VT_I4>(values, count); }
SAFEARRAY* CreateDoubleArray(const double* values, size_t count) { return CreateScalarArray<VT_R8>(values, count); }

class UiaValue {
public:
    explicit UiaValue(const std::wstring& text) { value.vt = VT_BSTR; value.bstrVal = SysAllocString(text.c_str()); }
    explicit UiaValue(bool flag) { value.vt = VT_BOOL; value.boolVal = flag ? VARIANT_TRUE : VARIANT_FALSE; }
    explicit UiaValue(double number) { value.vt = VT_R8; value.dblVal = number; }
    explicit UiaValue(ToggleState state) { value.vt = VT_I4; value.lVal = state; }
    explicit UiaValue(const RECT& bounds) {
        double rect[] = {static_cast<double>(bounds.left), static_cast<double>(bounds.top),
            static_cast<double>(std::max<LONG>(0, bounds.right - bounds.left)),
            static_cast<double>(std::max<LONG>(0, bounds.bottom - bounds.top))};
        value.vt = VT_ARRAY | VT_R8;
        value.parray = CreateDoubleArray(rect, 4);
    }
    ~UiaValue() { VariantClear(&value); }
    UiaValue(const UiaValue&) = delete;
    UiaValue& operator=(const UiaValue&) = delete;
    VARIANT value{};
};

template <class T> void RaiseValueChange(const AccessibilityTree &tree, LONG id, PROPERTYID property, const T &before, const T &after) {
    UiaValue oldValue(before), newValue(after);
    tree.RaiseUiaPropertyChanged(id, property, oldValue.value, newValue.value);
}

SAFEARRAY* CreateProviderArray(const std::vector<IRawElementProviderSimple*>& providers) {
    SAFEARRAYBOUND bound{};
    bound.cElements = static_cast<ULONG>(providers.size());
    bound.lLbound = 0;
    SAFEARRAY* sa = SafeArrayCreate(VT_UNKNOWN, 1, &bound);
    if (!sa) return nullptr;
    if (providers.empty()) return sa;
    IUnknown** data = nullptr;
    if (SUCCEEDED(SafeArrayAccessData(sa, reinterpret_cast<void**>(&data))) && data) {
        for (size_t i = 0; i < providers.size(); ++i) {
            data[i] = static_cast<IUnknown*>(static_cast<IRawElementProviderSimple*>(providers[i]));
            if (data[i]) { data[i]->AddRef(); }
        }
        SafeArrayUnaccessData(sa);
    } else { SafeArrayDestroy(sa); return nullptr; }
    return sa;
}

// UI Automation can synchronously call back into a provider while it is being disconnected. Never make that call while
// AccessibilityTree::mutex_ is held: the provider callback reads the tree and needs the same mutex.
void DisconnectChildProviders(HWND hwnd, std::vector<AccessibilityChildProvider*>& providers) {
    for (AccessibilityChildProvider* provider : providers) {
        if (!provider) continue;
        provider->DetachTree();
        if (hwnd) UiaDisconnectProvider(provider);
        provider->Release();
    }
    providers.clear();
}
} // namespace

// ============================================================================
// AccessibilityNode Capability Helpers
// ============================================================================

bool AccessibilityNode::IsInvokeSupported() const {
    if (hasInvoke || onInvoke) return true;
    if (hasToggle || role == ROLE_SYSTEM_CHECKBUTTON) return false;
    if (hasRangeValue || role == ROLE_SYSTEM_SLIDER || role == ROLE_SYSTEM_PROGRESSBAR) return false;
    if (role == ROLE_SYSTEM_PAGETAB || role == ROLE_SYSTEM_LISTITEM) return false;
    return onDefaultAction != nullptr;
}

bool AccessibilityNode::IsToggleSupported() const {
    if (hasToggle || onToggle) return true;
    if (role == ROLE_SYSTEM_CHECKBUTTON) return true;
    if (role == ROLE_SYSTEM_RADIOBUTTON && (state & STATE_SYSTEM_CHECKED)) return true;
    return false;
}

bool AccessibilityNode::IsValueSupported() const {
    if (hasValue || onSetValue) return true;
    return (role == ROLE_SYSTEM_TEXT || role == ROLE_SYSTEM_COMBOBOX) && !value.empty();
}

bool AccessibilityNode::IsRangeValueSupported() const { return hasRangeValue || (onSetRangeValue != nullptr); }

bool AccessibilityNode::IsSelectionSupported() const {
    if (hasSelection || onGetSelection) return true;
    return role == ROLE_SYSTEM_LIST || role == ROLE_SYSTEM_PAGETABLIST;
}

bool AccessibilityNode::IsSelectionItemSupported() const {
    if (hasSelectionItem || onSelect || onAddToSelection || onRemoveFromSelection) return true;
    return role == ROLE_SYSTEM_PAGETAB || role == ROLE_SYSTEM_LISTITEM || role == ROLE_SYSTEM_RADIOBUTTON;
}

bool AccessibilityNode::IsScrollItemSupported() const { return hasScrollItem || (onScrollIntoView != nullptr); }

bool AccessibilityNode::IsScrollSupported() const {
    if (!horizontallyScrollable && !verticallyScrollable) return false;
    return hasScroll || (onScroll != nullptr) || (onSetScrollPercent != nullptr);
}

LONG AccessibilityNode::GetEffectiveControlTypeId() const {
    if (controlTypeId != 0) return controlTypeId;
    return RoleToControlTypeId(role);
}

// ============================================================================
// AccessibilityTree Implementation
// ============================================================================

AccessibilityTree::AccessibilityTree(HWND hwnd) : hwnd_(hwnd) {}

AccessibilityTree::~AccessibilityTree() { InvalidateAndDisconnectChildProviders(); }

void AccessibilityTree::SetWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hwnd_ != hwnd) { hwnd_ = hwnd; }
}

HWND AccessibilityTree::GetWindow() const { std::lock_guard<std::mutex> lock(mutex_); return hwnd_; }

void AccessibilityTree::NotifySimple(DWORD eventId, LONG childId) const {
    HWND hwnd = nullptr;
    { std::lock_guard<std::mutex> lock(mutex_); hwnd = hwnd_; }
    if (hwnd && IsWindow(hwnd)) { NotifyWinEvent(eventId, hwnd, OBJID_CLIENT, childId); }
}

void AccessibilityTree::NotifyReorder() const { NotifySimple(EVENT_OBJECT_REORDER, CHILDID_SELF); }

bool AccessibilityTree::AreUiaClientsListening() { return UiaClientsAreListening() != FALSE; }

bool AccessibilityTree::RaiseUiaEvent(LONG childId, EVENTID eventId) const {
    HWND hwnd = nullptr;
    { std::lock_guard<std::mutex> lock(mutex_); hwnd = hwnd_; }
    if (!hwnd || !IsWindow(hwnd) || !AreUiaClientsListening()) return false;
    IRawElementProviderSimple* provider = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (childId <= 0) {
            provider = const_cast<AccessibilityTree*>(this);
        } else { provider = const_cast<AccessibilityTree*>(this)->GetOrCreateChildProviderLocked(childId); }
    }
    if (provider) { return SUCCEEDED(UiaRaiseAutomationEvent(provider, eventId)); }
    return false;
}

bool AccessibilityTree::RaiseUiaPropertyChanged(LONG childId, PROPERTYID propertyId, const VARIANT& oldValue, const VARIANT& newValue) const {
    HWND hwnd = nullptr;
    { std::lock_guard<std::mutex> lock(mutex_); hwnd = hwnd_; }
    if (!hwnd || !IsWindow(hwnd) || !AreUiaClientsListening()) return false;
    IRawElementProviderSimple* provider = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (childId <= 0) {
            provider = const_cast<AccessibilityTree*>(this);
        } else { provider = const_cast<AccessibilityTree*>(this)->GetOrCreateChildProviderLocked(childId); }
    }
    if (provider) { return SUCCEEDED(UiaRaiseAutomationPropertyChangedEvent(provider, propertyId, oldValue, newValue)); }
    return false;
}

bool AccessibilityTree::RaiseUiaStructureChanged(StructureChangeType changeType, LONG childId) const {
    HWND hwnd = nullptr;
    { std::lock_guard<std::mutex> lock(mutex_); hwnd = hwnd_; }
    if (!hwnd || !IsWindow(hwnd) || !AreUiaClientsListening()) return false;
    IRawElementProviderSimple* provider = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (childId <= 0) {
            provider = const_cast<AccessibilityTree*>(this);
        } else { provider = const_cast<AccessibilityTree*>(this)->GetOrCreateChildProviderLocked(childId); }
    }
    if (provider) {
        int runtimeId[2] = { UiaAppendRuntimeId, static_cast<int>(childId) };
        return SUCCEEDED(UiaRaiseStructureChangedEvent(provider, changeType, runtimeId, childId > 0 ? 2 : 0));
    }
    return false;
}

AccessibilityChildProvider* AccessibilityTree::GetOrCreateChildProviderLocked(LONG childId) {
    if (childId <= 0) return nullptr;
    auto it = childProviders_.find(childId);
    if (it != childProviders_.end()) { return it->second; }
    AccessibilityChildProvider* provider = new AccessibilityChildProvider(childId, this);
    childProviders_[childId] = provider;
    return provider;
}

void AccessibilityTree::InvalidateAndDisconnectChildProviders() {
    std::vector<AccessibilityChildProvider*> providers;
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hwnd = hwnd_;
        providers.reserve(childProviders_.size());
        for (const auto& pair : childProviders_) { if (pair.second) providers.push_back(pair.second); }
        childProviders_.clear();
    }
    DisconnectChildProviders(hwnd, providers);
}

IRawElementProviderSimple* AccessibilityTree::GetRootProvider() { AddRef(); return static_cast<IRawElementProviderSimple*>(this); }

IRawElementProviderSimple* AccessibilityTree::GetProviderForNode(LONG childId) {
    if (childId <= 0) return GetRootProvider();
    std::lock_guard<std::mutex> lock(mutex_);
    AccessibilityChildProvider* provider = GetOrCreateChildProviderLocked(childId);
    if (provider) { provider->AddRef(); return provider; }
    return nullptr;
}

bool AccessibilityTree::GetNodeCopy(LONG childId, AccessibilityNode& outNode) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const AccessibilityNode& node : nodes_) {
        if (node.id == childId) { outNode = node; return true; }
    }
    return false;
}

bool AccessibilityTree::ReplaceNodes(const std::vector<AccessibilityNode>& nodes) {
    std::vector<AccessibilityNode> oldNodes;
    std::vector<AccessibilityChildProvider*> providersToDisconnect;
    HWND hwnd = nullptr;
    std::unordered_set<LONG> newIds;
    newIds.reserve(nodes.size());
    for (const auto& n : nodes) { if (n.id <= 0 || !newIds.insert(n.id).second) return false; }
    // Copy the incoming snapshot before taking the lock, then transfer the
    // existing snapshot to the notification pass without copying its strings,
    // relationship vectors, and callbacks. Failed copies leave the tree intact.
    std::vector<AccessibilityNode> replacement = nodes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hwnd = hwnd_;
        oldNodes.swap(nodes_);
        nodes_.swap(replacement);
        // Disconnect providers that are no longer in the tree
        auto it = childProviders_.begin();
        while (it != childProviders_.end()) {
            if (newIds.find(it->first) == newIds.end()) {
                if (it->second) { providersToDisconnect.push_back(it->second); }
                it = childProviders_.erase(it);
            } else { ++it; }
        }
    }
    DisconnectChildProviders(hwnd, providersToDisconnect);
    const bool orderChanged = oldNodes.size() != nodes.size() || !std::equal(oldNodes.begin(), oldNodes.end(), nodes.begin(),
            [](const AccessibilityNode& old, const AccessibilityNode& current) { return old.id == current.id; });
    if (orderChanged) { NotifyReorder(); RaiseUiaStructureChanged(StructureChangeType_ChildrenInvalidated, 0); }
    std::unordered_map<LONG, size_t> oldIndices;
    if (orderChanged) {
        oldIndices.reserve(oldNodes.size());
        for (size_t i = 0; i < oldNodes.size(); ++i) oldIndices.emplace(oldNodes[i].id, i);
    }
    { std::lock_guard<std::mutex> lock(mutex_); hwnd = hwnd_; }
    const bool uiaListening = (hwnd && IsWindow(hwnd) && AreUiaClientsListening());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const AccessibilityNode& node = nodes[i];
        const AccessibilityNode* old = nullptr;
        if (!orderChanged && i < oldNodes.size()) {
            old = &oldNodes[i];
        } else { const auto oldIt = oldIndices.find(node.id); if (oldIt != oldIndices.end()) old = &oldNodes[oldIt->second]; }
        if (!old) continue;
        NotifyNodeChanges(*old, node, uiaListening);
    }
    return true;
}

void AccessibilityTree::NotifyNodeChanges(const AccessibilityNode& old, const AccessibilityNode& node, bool uiaListening) {
    if (old.name != node.name) {
        NotifySimple(EVENT_OBJECT_NAMECHANGE, node.id);
        if (uiaListening) { RaiseValueChange(*this, node.id, UIA_NamePropertyId, old.name, node.name); }
    }
    if (old.value != node.value) {
        NotifySimple(EVENT_OBJECT_VALUECHANGE, node.id);
        if (uiaListening) { RaiseValueChange(*this, node.id, UIA_ValueValuePropertyId, old.value, node.value); }
    }
    if (node.liveSetting != Off) {
        if (old.name != node.name || old.value != node.value) {
            NotifySimple(EVENT_SYSTEM_ALERT, node.id);
            if (uiaListening) { RaiseUiaEvent(node.id, UIA_LiveRegionChangedEventId); }
        }
    }
    if (node.IsRangeValueSupported() && old.rangeValue != node.rangeValue) {
        if (uiaListening) { RaiseValueChange(*this, node.id, UIA_RangeValueValuePropertyId, old.rangeValue, node.rangeValue); }
    }
    if (old.helpText != node.helpText) {
        if (uiaListening) { RaiseValueChange(*this, node.id, UIA_HelpTextPropertyId, old.helpText, node.helpText); }
    }
    if (old.itemStatus != node.itemStatus) {
        if (uiaListening) { RaiseValueChange(*this, node.id, UIA_ItemStatusPropertyId, old.itemStatus, node.itemStatus); }
    }
    if (old.role != node.role || old.state != node.state || old.selected != node.selected || old.focused != node.focused) {
        NotifySimple(EVENT_OBJECT_STATECHANGE, node.id);
        if (uiaListening) {
            if (old.selected != node.selected)
                RaiseValueChange(*this, node.id, UIA_SelectionItemIsSelectedPropertyId, old.selected, node.selected);
            if (old.focused != node.focused) {
                RaiseValueChange(*this, node.id, UIA_HasKeyboardFocusPropertyId, old.focused, node.focused);
                if (node.focused) { RaiseUiaEvent(node.id, UIA_AutomationFocusChangedEventId); }
            }
        }
    }
    if (old.bounds.left != node.bounds.left || old.bounds.top != node.bounds.top ||
        old.bounds.right != node.bounds.right || old.bounds.bottom != node.bounds.bottom) {
        NotifySimple(EVENT_OBJECT_LOCATIONCHANGE, node.id);
        if (uiaListening) { RaiseValueChange(*this, node.id, UIA_BoundingRectanglePropertyId, old.bounds, node.bounds); }
    }
}

void AccessibilityTree::ClearNodes() {
    bool hadNodes = false;
    { std::lock_guard<std::mutex> lock(mutex_); hadNodes = !nodes_.empty(); nodes_.clear(); }
    InvalidateAndDisconnectChildProviders();
    if (hadNodes) { NotifyReorder(); RaiseUiaStructureChanged(StructureChangeType_ChildrenInvalidated, 0); }
}

std::vector<AccessibilityNode> AccessibilityTree::GetNodes() const { std::lock_guard<std::mutex> lock(mutex_); return nodes_; }

bool AccessibilityTree::SetFocusedNode(LONG childId) {
    if (childId <= 0) return false;
    std::vector<LONG> changed;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (AccessibilityNode& node : nodes_) {
            const bool next = node.id == childId;
            if (next) found = true;
            if (node.focused != next) { node.focused = next; changed.push_back(node.id); }
        }
    }
    if (!found) return false;
    for (LONG id : changed) NotifySimple(EVENT_OBJECT_STATECHANGE, id);
    NotifySimple(EVENT_OBJECT_FOCUS, childId);
    RaiseUiaEvent(childId, UIA_AutomationFocusChangedEventId);
    return true;
}

bool AccessibilityTree::SetSelectedNode(LONG childId, bool selected) {
    if (childId <= 0) return false;
    bool found = false, changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (AccessibilityNode& node : nodes_) {
            if (node.id == childId) {
                found = true;
                if (node.selected != selected) { node.selected = selected; changed = true; }
            }
        }
    }
    if (changed) {
        NotifySimple(EVENT_OBJECT_STATECHANGE, childId);
        RaiseValueChange(*this, childId, UIA_SelectionItemIsSelectedPropertyId, (!selected), selected);
    }
    return found;
}

bool AccessibilityTree::AnnounceLiveRegion(LONG childId, const std::wstring& text) {
    if (childId <= 0) return false;
    std::wstring oldName;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (AccessibilityNode& node : nodes_) {
            if (node.id == childId) {
                found = true;
                if (node.name == text) return true;
                oldName = node.name;
                node.name = text;
                break;
            }
        }
        if (!found) {
            AccessibilityNode node;
            node.id = childId;
            node.name = text;
            node.role = ROLE_SYSTEM_STATUSBAR;
            node.state = STATE_SYSTEM_READONLY;
            node.controlTypeId = UIA_StatusBarControlTypeId;
            node.automationId = L"AppStatusLiveRegion";
            node.className = L"ApertureStatusLiveRegion";
            node.ariaRole = L"status";
            node.liveSetting = Polite;
            nodes_.push_back(std::move(node));
            found = true;
        }
    }
    HWND hwnd = nullptr;
    { std::lock_guard<std::mutex> lock(mutex_); hwnd = hwnd_; }
    if (hwnd && IsWindow(hwnd)) {
        NotifySimple(EVENT_OBJECT_NAMECHANGE, childId);
        NotifySimple(EVENT_SYSTEM_ALERT, childId);
        if (AreUiaClientsListening()) {
            RaiseValueChange(*this, childId, UIA_NamePropertyId, oldName, text);
            RaiseUiaEvent(childId, UIA_LiveRegionChangedEventId);
        }
    }
    return true;
}

LRESULT AccessibilityTree::HandleGetObject(WPARAM wParam, LPARAM lParam) {
    const LONG objectId = static_cast<LONG>(lParam);
    if (objectId == OBJID_CLIENT)
        return LresultFromObject(IID_IAccessible, wParam, static_cast<LPUNKNOWN>(static_cast<IAccessible*>(this)));
    if (objectId == static_cast<LONG>(UiaRootObjectId)) {
        HWND hwnd = GetWindow();
        if (hwnd) { return UiaReturnRawElementProvider(hwnd, wParam, lParam, static_cast<IRawElementProviderSimple*>(this)); }
    }
    return 0;
}

// ============================================================================
// IUnknown / IDispatch / IAccessible implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE AccessibilityTree::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IAccessible) {
        *object = static_cast<IAccessible*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IRawElementProviderSimple)) { *object = static_cast<IRawElementProviderSimple*>(this); AddRef(); return S_OK; }
    if (riid == __uuidof(IRawElementProviderFragment)) { *object = static_cast<IRawElementProviderFragment*>(this); AddRef(); return S_OK; }
    if (riid == __uuidof(IRawElementProviderFragmentRoot)) {
        *object = static_cast<IRawElementProviderFragmentRoot*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IRawElementProviderAdviseEvents)) {
        *object = static_cast<IRawElementProviderAdviseEvents*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE AccessibilityTree::AddRef() { return ++refCount_; }

ULONG STDMETHODCALLTYPE AccessibilityTree::Release() { const ULONG remaining = --refCount_; return remaining; }

HRESULT STDMETHODCALLTYPE AccessibilityTree::GetTypeInfoCount(UINT* count) { if (!count) return E_POINTER; *count = 0; return S_OK; }

HRESULT STDMETHODCALLTYPE AccessibilityTree::GetTypeInfo(UINT, LCID, ITypeInfo** typeInfo) {
    if (typeInfo) *typeInfo = nullptr;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE AccessibilityTree::Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) {
    return E_NOTIMPL;
}

bool AccessibilityTree::IsSelf(const VARIANT& childId) const { LONG id = 0; return ReadChildId(childId, id) && id == CHILDID_SELF; }

bool AccessibilityTree::LookupChild(const VARIANT& childId, NodeLookup& lookup) const {
    LONG id = 0;
    if (!ReadChildId(childId, id) || id <= 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(nodes_.begin(), nodes_.end(), [id](const AccessibilityNode& node) { return node.id == id; });
    if (it == nodes_.end()) return false;
    lookup.node = *it;
    lookup.index = static_cast<size_t>(std::distance(nodes_.begin(), it));
    return true;
}

void AccessibilityTree::SetEmpty(VARIANT* value) { if (!value) return; VariantInit(value); value->vt = VT_EMPTY; }

HRESULT AccessibilityTree::SetBstr(const std::wstring& text, BSTR* output) {
    if (!output) return E_POINTER;
    *output = SysAllocStringLen(text.data(), static_cast<UINT>(std::min<size_t>(text.size(), static_cast<size_t>(UINT_MAX))));
    return *output || text.empty() ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accParent(IDispatch** parent) {
    if (!parent) return E_POINTER;
    *parent = nullptr;
    return S_FALSE;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accChildCount(long* childCount) {
    if (!childCount) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    *childCount = static_cast<long>(std::min<size_t>(nodes_.size(), static_cast<size_t>(LONG_MAX)));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accChild(VARIANT childId, IDispatch** child) {
    if (!child) return E_POINTER;
    *child = nullptr;
    if (IsSelf(childId)) { *child = static_cast<IDispatch*>(this); AddRef(); return S_OK; }
    NodeLookup lookup;
    return LookupChild(childId, lookup) ? S_FALSE : E_INVALIDARG;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accName(VARIANT childId, BSTR* name) {
    if (IsSelf(childId)) return SetBstr(L"Aperture", name);
    NodeLookup lookup;
    return LookupChild(childId, lookup) ? SetBstr(lookup.node.name, name) : E_INVALIDARG;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accValue(VARIANT childId, BSTR* value) {
    if (IsSelf(childId)) return SetBstr(L"", value);
    NodeLookup lookup;
    return LookupChild(childId, lookup) ? SetBstr(lookup.node.value, value) : E_INVALIDARG;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accDescription(VARIANT childId, BSTR* description) {
    if (IsSelf(childId)) return SetBstr(L"Aperture media player", description);
    NodeLookup lookup;
    return LookupChild(childId, lookup) ? SetBstr(lookup.node.description, description) : E_INVALIDARG;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accRole(VARIANT childId, VARIANT* role) {
    if (!role) return E_POINTER;
    VariantInit(role);
    role->vt = VT_I4;
    if (IsSelf(childId)) { role->lVal = ROLE_SYSTEM_CLIENT; return S_OK; }
    NodeLookup lookup;
    if (!LookupChild(childId, lookup)) return E_INVALIDARG;
    role->lVal = lookup.node.role;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accState(VARIANT childId, VARIANT* state) {
    if (!state) return E_POINTER;
    VariantInit(state);
    state->vt = VT_I4;
    if (IsSelf(childId)) { state->lVal = STATE_SYSTEM_FOCUSABLE; return S_OK; }
    NodeLookup lookup;
    if (!LookupChild(childId, lookup)) return E_INVALIDARG;
    state->lVal = lookup.node.state;
    if (lookup.node.focused) state->lVal |= STATE_SYSTEM_FOCUSED;
    if (lookup.node.selected) state->lVal |= STATE_SYSTEM_SELECTED;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accHelp(VARIANT childId, BSTR* help) {
    if (IsSelf(childId)) return SetBstr(L"Aperture media player", help);
    NodeLookup lookup;
    if (!LookupChild(childId, lookup)) return E_INVALIDARG;
    if (lookup.node.description.empty()) { if (help) *help = nullptr; return S_FALSE; }
    return SetBstr(lookup.node.description, help);
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accHelpTopic(BSTR* helpFile, VARIANT, long* topicId) {
    if (!helpFile || !topicId) return E_POINTER;
    *helpFile = nullptr;
    *topicId = 0;
    return S_FALSE;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accKeyboardShortcut(VARIANT childId, BSTR* shortcut) {
    if (IsSelf(childId)) return SetBstr(L"", shortcut);
    NodeLookup lookup;
    return LookupChild(childId, lookup) ? SetBstr(lookup.node.keyboardShortcut, shortcut) : E_INVALIDARG;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accFocus(VARIANT* focusChild) {
    if (!focusChild) return E_POINTER;
    VariantInit(focusChild);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const AccessibilityNode& node : nodes_) {
        if (node.focused) { focusChild->vt = VT_I4; focusChild->lVal = node.id; return S_OK; }
    }
    SetEmpty(focusChild);
    return S_FALSE;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accSelection(VARIANT* selection) {
    if (!selection) return E_POINTER;
    VariantInit(selection);
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LONG> selectedIds;
    for (const AccessibilityNode& node : nodes_) {
        if (node.selected) { selectedIds.push_back(node.id); }
    }
    if (selectedIds.empty()) { SetEmpty(selection); return S_FALSE; }
    if (selectedIds.size() == 1) { selection->vt = VT_I4; selection->lVal = selectedIds.front(); return S_OK; }
    SAFEARRAY* sa = CreateIntArray(reinterpret_cast<const int*>(selectedIds.data()), selectedIds.size());
    if (!sa) return E_OUTOFMEMORY;
    selection->vt = VT_ARRAY | VT_I4;
    selection->parray = sa;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_accDefaultAction(VARIANT childId, BSTR* action) {
    if (IsSelf(childId)) return SetBstr(L"", action);
    NodeLookup lookup;
    return LookupChild(childId, lookup) ? SetBstr(lookup.node.defaultAction, action) : E_INVALIDARG;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::accSelect(long flags, VARIANT childId) {
    NodeLookup lookup;
    if (!LookupChild(childId, lookup)) return E_INVALIDARG;
    if ((flags & (SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION | SELFLAG_REMOVESELECTION |
        SELFLAG_ADDSELECTION | SELFLAG_EXTENDSELECTION)) == 0) return E_INVALIDARG;
    if (flags & SELFLAG_TAKEFOCUS) {
        SetFocusedNode(lookup.node.id);
        HWND hwnd = GetWindow();
        if (hwnd && GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()) { ::SetFocus(hwnd); }
    }
    if (flags & SELFLAG_TAKESELECTION) {
        if (lookup.node.onSelect) { lookup.node.onSelect(lookup.node.id); }
        std::vector<LONG> changed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (AccessibilityNode& node : nodes_) {
                const bool next = node.id == lookup.node.id;
                if (node.selected != next) { node.selected = next; changed.push_back(node.id); }
            }
        }
        for (LONG id : changed) NotifySimple(EVENT_OBJECT_STATECHANGE, id);
    } else if (flags & SELFLAG_ADDSELECTION) {
        if (lookup.node.onAddToSelection) {
            lookup.node.onAddToSelection(lookup.node.id);
        } else if (lookup.node.onSelect) { lookup.node.onSelect(lookup.node.id); }
        SetSelectedNode(lookup.node.id, true);
    } else if (flags & SELFLAG_REMOVESELECTION) {
        if (lookup.node.onRemoveFromSelection) { lookup.node.onRemoveFromSelection(lookup.node.id); }
        SetSelectedNode(lookup.node.id, false);
    } else if (flags & (SELFLAG_EXTENDSELECTION)) {
        if (lookup.node.onAddToSelection) {
            lookup.node.onAddToSelection(lookup.node.id);
        } else if (lookup.node.onSelect) { lookup.node.onSelect(lookup.node.id); }
        SetSelectedNode(lookup.node.id, true);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::accLocation(long* left, long* top, long* width, long* height, VARIANT childId) {
    if (!left || !top || !width || !height) return E_POINTER;
    RECT bounds{};
    if (IsSelf(childId)) {
        HWND hwnd = GetWindow();
        if (!hwnd || !GetWindowRect(hwnd, &bounds)) return E_FAIL;
    } else { NodeLookup lookup; if (!LookupChild(childId, lookup)) return E_INVALIDARG; bounds = lookup.node.bounds; }
    *left = bounds.left;
    *top = bounds.top;
    *width = std::max<LONG>(0, bounds.right - bounds.left);
    *height = std::max<LONG>(0, bounds.bottom - bounds.top);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::accNavigate(long direction, VARIANT startChild, VARIANT* endChild) {
    if (!endChild) return E_POINTER;
    VariantInit(endChild);
    LONG startId = CHILDID_SELF;
    if (!IsSelf(startChild) && !ReadChildId(startChild, startId)) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.empty()) return S_FALSE;
    LONG result = 0;
    if (direction == NAVDIR_FIRSTCHILD && startId == CHILDID_SELF) result = nodes_.front().id;
    else if (direction == NAVDIR_LASTCHILD && startId == CHILDID_SELF) result = nodes_.back().id;
    else if (direction == NAVDIR_NEXT || direction == NAVDIR_PREVIOUS) {
        const auto it = std::find_if(nodes_.begin(), nodes_.end(), [startId](const AccessibilityNode& node) { return node.id == startId; });
        if (it != nodes_.end()) {
            const ptrdiff_t index = std::distance(nodes_.begin(), it) + (direction == NAVDIR_NEXT ? 1 : -1);
            if (index >= 0 && static_cast<size_t>(index) < nodes_.size()) result = nodes_[static_cast<size_t>(index)].id;
        }
    } else if (direction == NAVDIR_UP && startId != CHILDID_SELF) { result = CHILDID_SELF; }
    if (!result) return S_FALSE;
    endChild->vt = VT_I4;
    endChild->lVal = result;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::accHitTest(long screenX, long screenY, VARIANT* childAtPoint) {
    if (!childAtPoint) return E_POINTER;
    VariantInit(childAtPoint);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        if (screenX >= it->bounds.left && screenX < it->bounds.right && screenY >= it->bounds.top && screenY < it->bounds.bottom) {
            childAtPoint->vt = VT_I4;
            childAtPoint->lVal = it->id;
            return S_OK;
        }
    }
    RECT windowBounds{};
    if (hwnd_ && GetWindowRect(hwnd_, &windowBounds) && PtInRect(&windowBounds, POINT{ screenX, screenY })) {
        childAtPoint->vt = VT_I4;
        childAtPoint->lVal = CHILDID_SELF;
        return S_OK;
    }
    SetEmpty(childAtPoint);
    return S_FALSE;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::accDoDefaultAction(VARIANT childId) {
    NodeLookup lookup;
    if (!LookupChild(childId, lookup)) return E_INVALIDARG;
    if (!lookup.node.onDefaultAction) return S_FALSE;
    lookup.node.onDefaultAction(lookup.node.id);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::put_accName(VARIANT, BSTR) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE AccessibilityTree::put_accValue(VARIANT, BSTR) { return E_NOTIMPL; }

// ============================================================================
// UIA Root: IRawElementProviderSimple Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_ProviderOptions(ProviderOptions* pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::GetPatternProvider(PATTERNID, IUnknown** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) {
    if (!pRetVal) return E_POINTER;
    VariantInit(pRetVal);
    switch (propertyId) {
    case UIA_ControlTypePropertyId:
        pRetVal->vt = VT_I4;
        pRetVal->lVal = UIA_WindowControlTypeId;
        return S_OK;
    case UIA_NamePropertyId:
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(L"Aperture");
        return S_OK;
    case UIA_AutomationIdPropertyId:
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(L"ApertureMainWindow");
        return S_OK;
    case UIA_ClassNamePropertyId:
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(L"ApertureWindowClass");
        return S_OK;
    case UIA_HelpTextPropertyId:
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(L"Aperture media player");
        return S_OK;
    case UIA_ItemTypePropertyId:
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(L"ApplicationWindow");
        return S_OK;
    case UIA_IsEnabledPropertyId:
        pRetVal->vt = VT_BOOL;
        pRetVal->boolVal = VARIANT_TRUE;
        return S_OK;
    case UIA_IsKeyboardFocusablePropertyId:
        pRetVal->vt = VT_BOOL;
        pRetVal->boolVal = VARIANT_TRUE;
        return S_OK;
    case UIA_HasKeyboardFocusPropertyId: {
        HWND hwnd = GetWindow();
        pRetVal->vt = VT_BOOL;
        pRetVal->boolVal = (hwnd && ::GetFocus() == hwnd) ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }
    case UIA_LiveSettingPropertyId:
        pRetVal->vt = VT_I4;
        pRetVal->lVal = 0;
        return S_OK;
    case UIA_NativeWindowHandlePropertyId: {
        HWND hwnd = GetWindow();
        if (hwnd) { pRetVal->vt = VT_I4; pRetVal->lVal = static_cast<LONG>(reinterpret_cast<LONG_PTR>(hwnd)); return S_OK; }
        return S_OK;
    }
    case UIA_BoundingRectanglePropertyId: {
        HWND hwnd = GetWindow();
        if (hwnd) {
            RECT rc{};
            if (GetWindowRect(hwnd, &rc)) {
                double bounds[4] = {static_cast<double>(rc.left), static_cast<double>(rc.top),
                                    static_cast<double>(std::max<LONG>(0, rc.right - rc.left)),
                                    static_cast<double>(std::max<LONG>(0, rc.bottom - rc.top))};
                pRetVal->vt = VT_ARRAY | VT_R8;
                pRetVal->parray = CreateDoubleArray(bounds, 4);
                return S_OK;
            }
        }
        return S_OK;
    }
    default: return S_OK;
    }
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    HWND hwnd = GetWindow();
    if (hwnd) { return UiaHostProviderFromHwnd(hwnd, pRetVal); }
    return S_OK;
}

// ============================================================================
// UIA Root: IRawElementProviderFragment Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE AccessibilityTree::Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.empty()) return S_OK;
    if (direction == NavigateDirection_FirstChild) {
        AccessibilityChildProvider* child = GetOrCreateChildProviderLocked(nodes_.front().id);
        if (child) { child->AddRef(); *pRetVal = child; }
        return S_OK;
    }
    if (direction == NavigateDirection_LastChild) {
        AccessibilityChildProvider* child = GetOrCreateChildProviderLocked(nodes_.back().id);
        if (child) { child->AddRef(); *pRetVal = child; }
        return S_OK;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::GetRuntimeId(SAFEARRAY** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_BoundingRectangle(UiaRect* pRetVal) {
    if (!pRetVal) return E_POINTER;
    HWND hwnd = GetWindow();
    if (hwnd) {
        RECT rc{};
        if (GetWindowRect(hwnd, &rc)) {
            pRetVal->left = static_cast<double>(rc.left);
            pRetVal->top = static_cast<double>(rc.top);
            pRetVal->width = static_cast<double>(std::max<LONG>(0, rc.right - rc.left));
            pRetVal->height = static_cast<double>(std::max<LONG>(0, rc.bottom - rc.top));
            return S_OK;
        }
    }
    pRetVal->left = 0.0;
    pRetVal->top = 0.0;
    pRetVal->width = 0.0;
    pRetVal->height = 0.0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::SetFocus() {
    HWND hwnd = GetWindow();
    if (hwnd) { ::SetFocus(hwnd); return S_OK; }
    return E_FAIL;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = static_cast<IRawElementProviderFragmentRoot*>(this);
    AddRef();
    return S_OK;
}

// ============================================================================
// UIA Root: IRawElementProviderFragmentRoot Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE AccessibilityTree::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    const LONG screenX = static_cast<LONG>(std::lround(x));
    const LONG screenY = static_cast<LONG>(std::lround(y));
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
        if (screenX >= it->bounds.left && screenX < it->bounds.right && screenY >= it->bounds.top && screenY < it->bounds.bottom) {
            AccessibilityChildProvider* child = GetOrCreateChildProviderLocked(it->id);
            if (child) { child->AddRef(); *pRetVal = child; return S_OK; }
        }
    }
    HWND hwnd = hwnd_;
    if (hwnd) {
        RECT windowBounds{};
        if (GetWindowRect(hwnd, &windowBounds) && PtInRect(&windowBounds, POINT{ screenX, screenY })) {
            *pRetVal = static_cast<IRawElementProviderFragment*>(this);
            AddRef();
            return S_OK;
        }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityTree::GetFocus(IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const AccessibilityNode& node : nodes_) {
        if (node.focused) {
            AccessibilityChildProvider* child = GetOrCreateChildProviderLocked(node.id);
            if (child) { child->AddRef(); *pRetVal = child; return S_OK; }
        }
    }
    *pRetVal = static_cast<IRawElementProviderFragment*>(this);
    AddRef();
    return S_OK;
}

// ============================================================================
// UIA Root: IRawElementProviderAdviseEvents Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE AccessibilityTree::AdviseEventAdded(EVENTID, SAFEARRAY*) { return S_OK; }

HRESULT STDMETHODCALLTYPE AccessibilityTree::AdviseEventRemoved(EVENTID, SAFEARRAY*) { return S_OK; }

// ============================================================================
// AccessibilityChildProvider Implementation
// ============================================================================

AccessibilityChildProvider::AccessibilityChildProvider(LONG childId, AccessibilityTree* tree) : childId_(childId), tree_(tree) {}

AccessibilityChildProvider::~AccessibilityChildProvider() = default;

void AccessibilityChildProvider::DetachTree() { std::lock_guard<std::recursive_mutex> lock(detachMutex_); tree_ = nullptr; }

bool AccessibilityChildProvider::GetNode(AccessibilityNode& outNode) const {
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (!tree_) return false;
    return tree_->GetNodeCopy(childId_, outNode);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IRawElementProviderSimple)) {
        *object = static_cast<IRawElementProviderSimple*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IRawElementProviderFragment)) { *object = static_cast<IRawElementProviderFragment*>(this); AddRef(); return S_OK; }
    AccessibilityNode node;
    if (GetNode(node)) {
        if (riid == __uuidof(IInvokeProvider) && node.IsInvokeSupported()) {
            *object = static_cast<IInvokeProvider*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IToggleProvider) && node.IsToggleSupported()) {
            *object = static_cast<IToggleProvider*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IValueProvider) && node.IsValueSupported()) {
            *object = static_cast<IValueProvider*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IRangeValueProvider) && node.IsRangeValueSupported()) {
            *object = static_cast<IRangeValueProvider*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(ISelectionProvider) && node.IsSelectionSupported()) {
            *object = static_cast<ISelectionProvider*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(ISelectionItemProvider) && node.IsSelectionItemSupported()) {
            *object = static_cast<ISelectionItemProvider*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IScrollItemProvider) && node.IsScrollItemSupported()) {
            *object = static_cast<IScrollItemProvider*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IScrollProvider) && node.IsScrollSupported()) {
            *object = static_cast<IScrollProvider*>(this);
            AddRef();
            return S_OK;
        }
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE AccessibilityChildProvider::AddRef() { return ++refCount_; }

ULONG STDMETHODCALLTYPE AccessibilityChildProvider::Release() {
    const ULONG remaining = --refCount_;
    if (remaining == 0) { delete this; }
    return remaining;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_ProviderOptions(ProviderOptions* pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    switch (patternId) {
    case UIA_InvokePatternId:
        if (node.IsInvokeSupported()) { *pRetVal = static_cast<IInvokeProvider*>(this); AddRef(); }
        break;
    case UIA_TogglePatternId:
        if (node.IsToggleSupported()) { *pRetVal = static_cast<IToggleProvider*>(this); AddRef(); }
        break;
    case UIA_ValuePatternId:
        if (node.IsValueSupported()) { *pRetVal = static_cast<IValueProvider*>(this); AddRef(); }
        break;
    case UIA_RangeValuePatternId:
        if (node.IsRangeValueSupported()) { *pRetVal = static_cast<IRangeValueProvider*>(this); AddRef(); }
        break;
    case UIA_SelectionPatternId:
        if (node.IsSelectionSupported()) { *pRetVal = static_cast<ISelectionProvider*>(this); AddRef(); }
        break;
    case UIA_SelectionItemPatternId:
        if (node.IsSelectionItemSupported()) { *pRetVal = static_cast<ISelectionItemProvider*>(this); AddRef(); }
        break;
    case UIA_ScrollItemPatternId:
        if (node.IsScrollItemSupported()) { *pRetVal = static_cast<IScrollItemProvider*>(this); AddRef(); }
        break;
    case UIA_ScrollPatternId:
        if (node.IsScrollSupported()) { *pRetVal = static_cast<IScrollProvider*>(this); AddRef(); }
        break;
    default: break;
    }
    return S_OK;
}

SAFEARRAY* AccessibilityChildProvider::CreateProviderArrayForIds(const std::vector<LONG>& ids) const {
    if (ids.empty()) return nullptr;
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (!tree_) return nullptr;
    std::lock_guard<std::mutex> treeLock(tree_->mutex_);
    std::vector<IRawElementProviderSimple*> providers;
    providers.reserve(ids.size());
    for (LONG id : ids) {
        if (id <= 0) continue;
        const auto it = std::find_if(tree_->nodes_.begin(), tree_->nodes_.end(), [id](const AccessibilityNode& n) { return n.id == id; });
        if (it != tree_->nodes_.end()) {
            AccessibilityChildProvider* provider = tree_->GetOrCreateChildProviderLocked(id);
            if (provider) { providers.push_back(provider); }
        }
    }
    if (providers.empty()) return nullptr;
    return CreateProviderArray(providers);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) {
    if (!pRetVal) return E_POINTER;
    VariantInit(pRetVal);
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    auto setBoolean = [&](bool value, bool supported = true) {
        if (supported) { pRetVal->vt = VT_BOOL; pRetVal->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; }
        return S_OK;
    };
    auto setInteger = [&](LONG value, bool supported = true) {
        if (supported) { pRetVal->vt = VT_I4; pRetVal->lVal = value; }
        return S_OK;
    };
    auto setNumber = [&](double value, bool supported = true) {
        if (supported) { pRetVal->vt = VT_R8; pRetVal->dblVal = value; }
        return S_OK;
    };
    auto setText = [&](const std::wstring& value, bool supported = true) {
        if (supported) { pRetVal->vt = VT_BSTR; pRetVal->bstrVal = SysAllocString(value.c_str()); }
        return S_OK;
    };
    auto setProviders = [&](const std::vector<LONG>& ids) {
        if (auto* providers = CreateProviderArrayForIds(ids)) { pRetVal->vt = VT_ARRAY | VT_UNKNOWN; pRetVal->parray = providers; }
        return S_OK;
    };
    switch (propertyId) {
    case UIA_ControlTypePropertyId: return setInteger(node.GetEffectiveControlTypeId());
    case UIA_NamePropertyId: return setText(node.name);
    case UIA_AutomationIdPropertyId: {
        std::wstring autoId = node.automationId;
        if (autoId.empty()) { autoId = L"Node_" + std::to_wstring(node.id); }
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(autoId.c_str());
        return S_OK;
    }
    case UIA_ClassNamePropertyId: {
        std::wstring cls = node.className.empty() ? L"ApertureControl" : node.className;
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(cls.c_str());
        return S_OK;
    }
    case UIA_LocalizedControlTypePropertyId: return setText(node.localizedControlType, !node.localizedControlType.empty());
    case UIA_HelpTextPropertyId: {
        std::wstring help = node.helpText.empty() ? node.description : node.helpText;
        if (!help.empty()) { pRetVal->vt = VT_BSTR; pRetVal->bstrVal = SysAllocString(help.c_str()); }
        return S_OK;
    }
    case UIA_ItemStatusPropertyId: return setText(node.itemStatus, !node.itemStatus.empty());
    case UIA_AccessKeyPropertyId: {
        std::wstring key = node.accessKey.empty() ? node.keyboardShortcut : node.accessKey;
        if (!key.empty()) { pRetVal->vt = VT_BSTR; pRetVal->bstrVal = SysAllocString(key.c_str()); }
        return S_OK;
    }
    case UIA_AcceleratorKeyPropertyId: return setText(node.keyboardShortcut, !node.keyboardShortcut.empty());
    case UIA_IsEnabledPropertyId: return setBoolean(((node.state & STATE_SYSTEM_UNAVAILABLE) == 0));
    case UIA_IsKeyboardFocusablePropertyId: return setBoolean(((node.state & STATE_SYSTEM_FOCUSABLE) != 0));
    case UIA_HasKeyboardFocusPropertyId: return setBoolean(node.focused);
    case UIA_IsOffscreenPropertyId: return setBoolean(((node.state & STATE_SYSTEM_OFFSCREEN) != 0));
    case UIA_IsPasswordPropertyId: return setBoolean(node.isPassword);
    case UIA_BoundingRectanglePropertyId: {
        double bounds[4] = {static_cast<double>(node.bounds.left), static_cast<double>(node.bounds.top),
            static_cast<double>(std::max<LONG>(0, node.bounds.right - node.bounds.left)),
            static_cast<double>(std::max<LONG>(0, node.bounds.bottom - node.bounds.top))};
        pRetVal->vt = VT_ARRAY | VT_R8;
        pRetVal->parray = CreateDoubleArray(bounds, 4);
        return S_OK;
    }
    case UIA_LiveSettingPropertyId: return setInteger(static_cast<LONG>(node.liveSetting));
    // Pattern availability properties
    case UIA_IsInvokePatternAvailablePropertyId: return setBoolean(node.IsInvokeSupported());
    case UIA_IsTogglePatternAvailablePropertyId: return setBoolean(node.IsToggleSupported());
    case UIA_IsValuePatternAvailablePropertyId: return setBoolean(node.IsValueSupported());
    case UIA_IsRangeValuePatternAvailablePropertyId: return setBoolean(node.IsRangeValueSupported());
    case UIA_IsSelectionPatternAvailablePropertyId: return setBoolean(node.IsSelectionSupported());
    case UIA_SelectionCanSelectMultiplePropertyId: return setBoolean(node.canSelectMultiple, node.IsSelectionSupported());
    case UIA_SelectionIsSelectionRequiredPropertyId: return setBoolean(node.isSelectionRequired, node.IsSelectionSupported());
    case UIA_SelectionSelectionPropertyId:
        if (node.IsSelectionSupported()) {
            SAFEARRAY* sa = nullptr;
            if (SUCCEEDED(GetSelection(&sa)) && sa) { pRetVal->vt = VT_ARRAY | VT_UNKNOWN; pRetVal->parray = sa; return S_OK; }
        }
        return S_OK;
    case UIA_IsSelectionItemPatternAvailablePropertyId: return setBoolean(node.IsSelectionItemSupported());
    case UIA_IsScrollItemPatternAvailablePropertyId: return setBoolean(node.IsScrollItemSupported());
    case UIA_IsScrollPatternAvailablePropertyId: return setBoolean(node.IsScrollSupported());
    // Pattern direct properties
    case UIA_ToggleToggleStatePropertyId:
        if (node.IsToggleSupported()) {
            pRetVal->vt = VT_I4;
            if (node.hasToggle) {
                pRetVal->lVal = node.toggleState;
            } else { pRetVal->lVal = (node.state & STATE_SYSTEM_CHECKED) ? ToggleState_On : ToggleState_Off; }
            return S_OK;
        }
        return S_OK;
    case UIA_ValueValuePropertyId: return setText(node.value, node.IsValueSupported());
    case UIA_ValueIsReadOnlyPropertyId: return setBoolean(node.isReadOnly, node.IsValueSupported());
    case UIA_RangeValueValuePropertyId: return setNumber(node.rangeValue, node.IsRangeValueSupported());
    case UIA_RangeValueIsReadOnlyPropertyId: return setBoolean(node.rangeIsReadOnly, node.IsRangeValueSupported());
    case UIA_RangeValueMinimumPropertyId: return setNumber(node.rangeMin, node.IsRangeValueSupported());
    case UIA_RangeValueMaximumPropertyId: return setNumber(node.rangeMax, node.IsRangeValueSupported());
    case UIA_RangeValueLargeChangePropertyId: return setNumber(node.rangeLargeChange, node.IsRangeValueSupported());
    case UIA_RangeValueSmallChangePropertyId: return setNumber(node.rangeSmallChange, node.IsRangeValueSupported());
    case UIA_SelectionItemIsSelectedPropertyId: return setBoolean(node.selected, node.IsSelectionItemSupported());
    case UIA_SelectionItemSelectionContainerPropertyId:
        if (node.IsSelectionItemSupported()) {
            IRawElementProviderSimple* container = nullptr;
            if (SUCCEEDED(get_SelectionContainer(&container)) && container) {
                pRetVal->vt = VT_UNKNOWN;
                pRetVal->punkVal = container;
                return S_OK;
            }
        }
        return S_OK;
    case UIA_ScrollHorizontalScrollPercentPropertyId: return setNumber(node.horizontalScrollPercent, node.IsScrollSupported());
    case UIA_ScrollVerticalScrollPercentPropertyId: return setNumber(node.verticalScrollPercent, node.IsScrollSupported());
    case UIA_ScrollHorizontalViewSizePropertyId: return setNumber(node.horizontalViewSize, node.IsScrollSupported());
    case UIA_ScrollVerticalViewSizePropertyId: return setNumber(node.verticalViewSize, node.IsScrollSupported());
    case UIA_ScrollHorizontallyScrollablePropertyId: return setBoolean(node.horizontallyScrollable, node.IsScrollSupported());
    case UIA_ScrollVerticallyScrollablePropertyId: return setBoolean(node.verticallyScrollable, node.IsScrollSupported());
    // --- Semantic Relationships & Grouping Properties ---
    case UIA_LabeledByPropertyId: {
        if (node.labeledById > 0) {
            std::lock_guard<std::recursive_mutex> lock(detachMutex_);
            if (tree_) {
                std::lock_guard<std::mutex> treeLock(tree_->mutex_);
                const auto it = std::find_if(tree_->nodes_.begin(), tree_->nodes_.end(),
                                             [tId = node.labeledById](const AccessibilityNode &n) { return n.id == tId; });
                if (it != tree_->nodes_.end()) {
                    AccessibilityChildProvider* provider = tree_->GetOrCreateChildProviderLocked(node.labeledById);
                    if (provider) {
                        pRetVal->vt = VT_UNKNOWN;
                        provider->AddRef();
                        pRetVal->punkVal = static_cast<IUnknown*>(static_cast<IRawElementProviderSimple*>(provider));
                        return S_OK;
                    }
                }
            }
        }
        pRetVal->vt = VT_EMPTY;
        return S_OK;
    }
    case UIA_DescribedByPropertyId: return setProviders(node.describedByIds);
    case UIA_ControllerForPropertyId: return setProviders(node.controllerForIds);
    case UIA_FlowsToPropertyId: return setProviders(node.flowsToIds);
    case UIA_FlowsFromPropertyId: return setProviders(node.flowsFromIds);
    case UIA_ItemTypePropertyId: return setText(node.itemType, !node.itemType.empty());
    case UIA_PositionInSetPropertyId: return setInteger(node.positionInSet, node.positionInSet > 0);
    case UIA_SizeOfSetPropertyId: return setInteger(node.sizeOfSet, node.sizeOfSet > 0);
    case UIA_LevelPropertyId: return setInteger(node.level, node.level > 0);
    case UIA_AriaRolePropertyId: return setText(node.ariaRole, !node.ariaRole.empty());
    case UIA_AriaPropertiesPropertyId: return setText(node.ariaProperties, !node.ariaProperties.empty());
    default: return S_OK;
    }
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    return S_OK;
}

// ============================================================================
// UIA Child: IRawElementProviderFragment Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (!tree_) return UIA_E_ELEMENTNOTAVAILABLE;
    std::lock_guard<std::mutex> treeLock(tree_->mutex_);
    const auto& nodes = tree_->nodes_;
    const auto it = std::find_if(nodes.begin(), nodes.end(), [id = childId_](const AccessibilityNode& n) { return n.id == id; });
    if (it == nodes.end()) return UIA_E_ELEMENTNOTAVAILABLE;
    const size_t index = static_cast<size_t>(std::distance(nodes.begin(), it));
    switch (direction) {
    case NavigateDirection_Parent:
        *pRetVal = static_cast<IRawElementProviderFragment*>(tree_);
        tree_->AddRef();
        return S_OK;
    case NavigateDirection_NextSibling:
        if (index + 1 < nodes.size()) {
            AccessibilityChildProvider* next = tree_->GetOrCreateChildProviderLocked(nodes[index + 1].id);
            if (next) { next->AddRef(); *pRetVal = next; }
        }
        return S_OK;
    case NavigateDirection_PreviousSibling:
        if (index > 0) {
            AccessibilityChildProvider* prev = tree_->GetOrCreateChildProviderLocked(nodes[index - 1].id);
            if (prev) { prev->AddRef(); *pRetVal = prev; }
        }
        return S_OK;
    case NavigateDirection_FirstChild:
    case NavigateDirection_LastChild: return S_OK;
    default: return S_OK;
    }
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::GetRuntimeId(SAFEARRAY** pRetVal) {
    if (!pRetVal) return E_POINTER;
    int ids[2] = { UiaAppendRuntimeId, static_cast<int>(childId_) };
    *pRetVal = CreateIntArray(ids, 2);
    return *pRetVal ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_BoundingRectangle(UiaRect* pRetVal) {
    if (!pRetVal) return E_POINTER;
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    pRetVal->left = static_cast<double>(node.bounds.left);
    pRetVal->top = static_cast<double>(node.bounds.top);
    pRetVal->width = static_cast<double>(std::max<LONG>(0, node.bounds.right - node.bounds.left));
    pRetVal->height = static_cast<double>(std::max<LONG>(0, node.bounds.bottom - node.bounds.top));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::SetFocus() {
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (!tree_) return UIA_E_ELEMENTNOTAVAILABLE;
    if (tree_->SetFocusedNode(childId_)) {
        HWND hwnd = tree_->GetWindow();
        if (hwnd && GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()) { ::SetFocus(hwnd); }
        return S_OK;
    }
    return E_FAIL;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) {
    if (!pRetVal) return E_POINTER;
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (!tree_) return UIA_E_ELEMENTNOTAVAILABLE;
    *pRetVal = static_cast<IRawElementProviderFragmentRoot*>(tree_);
    tree_->AddRef();
    return S_OK;
}

// ============================================================================
// UIA Pattern Implementations on Child Provider
// ============================================================================

// --- IInvokeProvider ---
HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::Invoke() {
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsInvokeSupported()) return UIA_E_NOTSUPPORTED;
    if (node.state & STATE_SYSTEM_UNAVAILABLE) { return UIA_E_ELEMENTNOTENABLED; }
    if (node.onInvoke) { node.onInvoke(childId_); } else if (node.onDefaultAction) {
        node.onDefaultAction(childId_);
    } else { return S_FALSE; }
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (tree_) { tree_->RaiseUiaEvent(childId_, UIA_Invoke_InvokedEventId); }
    return S_OK;
}

// --- IToggleProvider ---
HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::Toggle() {
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsToggleSupported()) return UIA_E_NOTSUPPORTED;
    if (node.state & STATE_SYSTEM_UNAVAILABLE) { return UIA_E_ELEMENTNOTENABLED; }
    ToggleState oldState = node.toggleState;
    if (!node.hasToggle) { oldState = (node.state & STATE_SYSTEM_CHECKED) ? ToggleState_On : ToggleState_Off; }
    ToggleState newState = (oldState == ToggleState_On) ? ToggleState_Off : ToggleState_On;
    if (node.onToggle) { node.onToggle(childId_, newState); } else if (node.onDefaultAction) { node.onDefaultAction(childId_); }
    // Update node state in tree
    {
        std::lock_guard<std::recursive_mutex> lock(detachMutex_);
        if (tree_) {
            std::lock_guard<std::mutex> treeLock(tree_->mutex_);
            for (auto& n : tree_->nodes_) {
                if (n.id == childId_) {
                    n.toggleState = newState;
                    if (newState == ToggleState_On) n.state |= STATE_SYSTEM_CHECKED;
                    else n.state &= ~STATE_SYSTEM_CHECKED;
                    break;
                }
            }
        }
    }
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (tree_) {
        tree_->NotifySimple(EVENT_OBJECT_STATECHANGE, childId_);
        RaiseValueChange(*tree_, childId_, UIA_ToggleToggleStatePropertyId, oldState, newState);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_ToggleState(ToggleState* pRetVal) {
    if (!pRetVal) return E_POINTER;
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsToggleSupported()) return UIA_E_NOTSUPPORTED;
    if (node.hasToggle) { *pRetVal = node.toggleState; } else {
        *pRetVal = (node.state & STATE_SYSTEM_CHECKED) ? ToggleState_On : ToggleState_Off;
    }
    return S_OK;
}

// --- Shared get_IsReadOnly ---
HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_IsReadOnly(BOOL* pRetVal) {
    if (!pRetVal) return E_POINTER;
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (node.IsRangeValueSupported()) { *pRetVal = node.rangeIsReadOnly ? TRUE : FALSE; return S_OK; }
    if (node.IsValueSupported()) { *pRetVal = node.isReadOnly ? TRUE : FALSE; return S_OK; }
    return UIA_E_NOTSUPPORTED;
}

// --- IValueProvider ---
HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::SetValue(LPCWSTR val) {
    if (!val) return E_INVALIDARG;
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsValueSupported()) return UIA_E_NOTSUPPORTED;
    if (node.isReadOnly && !node.onSetValue) return UIA_E_NOTSUPPORTED;
    std::wstring oldValue = node.value;
    std::wstring newValue = val;
    if (node.onSetValue) { node.onSetValue(childId_, newValue); }
    {
        std::lock_guard<std::recursive_mutex> lock(detachMutex_);
        if (tree_) {
            std::lock_guard<std::mutex> treeLock(tree_->mutex_);
            for (auto& n : tree_->nodes_) {
                if (n.id == childId_) { n.value = newValue; break; }
            }
        }
    }
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (tree_) {
        tree_->NotifySimple(EVENT_OBJECT_VALUECHANGE, childId_);
        RaiseValueChange(*tree_, childId_, UIA_ValueValuePropertyId, oldValue, newValue);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_Value(BSTR* pRetVal) {
    if (!pRetVal) return E_POINTER;
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsValueSupported()) return UIA_E_NOTSUPPORTED;
    *pRetVal = SysAllocString(node.value.c_str());
    return *pRetVal || node.value.empty() ? S_OK : E_OUTOFMEMORY;
}

// --- IRangeValueProvider ---
HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::SetValue(double val) {
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsRangeValueSupported()) return UIA_E_NOTSUPPORTED;
    if (node.rangeIsReadOnly && !node.onSetRangeValue) return UIA_E_NOTSUPPORTED;
    double clamped = std::max(node.rangeMin, std::min(node.rangeMax, val));
    double oldVal = node.rangeValue;
    if (node.onSetRangeValue) { node.onSetRangeValue(childId_, clamped); }
    {
        std::lock_guard<std::recursive_mutex> lock(detachMutex_);
        if (tree_) {
            std::lock_guard<std::mutex> treeLock(tree_->mutex_);
            for (auto& n : tree_->nodes_) {
                if (n.id == childId_) { n.rangeValue = clamped; break; }
            }
        }
    }
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (tree_) {
        tree_->NotifySimple(EVENT_OBJECT_VALUECHANGE, childId_);
        RaiseValueChange(*tree_, childId_, UIA_RangeValueValuePropertyId, oldVal, clamped);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_Value(double* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::rangeValue, &AccessibilityNode::IsRangeValueSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_Maximum(double* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::rangeMax, &AccessibilityNode::IsRangeValueSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_Minimum(double* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::rangeMin, &AccessibilityNode::IsRangeValueSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_LargeChange(double* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::rangeLargeChange, &AccessibilityNode::IsRangeValueSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_SmallChange(double* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::rangeSmallChange, &AccessibilityNode::IsRangeValueSupported);
}

// --- ISelectionProvider ---
HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::GetSelection(SAFEARRAY** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsSelectionSupported()) return UIA_E_NOTSUPPORTED;
    std::vector<LONG> selectedIds;
    if (node.onGetSelection) { selectedIds = node.onGetSelection(childId_); } else {
        std::lock_guard<std::recursive_mutex> lock(detachMutex_);
        if (tree_) {
            std::lock_guard<std::mutex> treeLock(tree_->mutex_);
            for (const AccessibilityNode& n : tree_->nodes_) {
                if (n.selectionContainerId == childId_ && n.selected && n.id > 0) { selectedIds.push_back(n.id); }
            }
        }
    }
    std::vector<IRawElementProviderSimple*> providers;
    if (!selectedIds.empty()) {
        std::lock_guard<std::recursive_mutex> lock(detachMutex_);
        if (tree_) {
            std::lock_guard<std::mutex> treeLock(tree_->mutex_);
            providers.reserve(selectedIds.size());
            for (LONG id : selectedIds) {
                if (id <= 0) continue;
                const auto it =
                    std::find_if(tree_->nodes_.begin(), tree_->nodes_.end(), [id](const AccessibilityNode &n) { return n.id == id; });
                if (it != tree_->nodes_.end()) {
                    AccessibilityChildProvider* p = tree_->GetOrCreateChildProviderLocked(id);
                    if (p) { providers.push_back(p); }
                }
            }
        }
    }
    SAFEARRAY* sa = CreateProviderArray(providers);
    if (!sa) return E_OUTOFMEMORY;
    *pRetVal = sa;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_CanSelectMultiple(BOOL* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::canSelectMultiple, &AccessibilityNode::IsSelectionSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_IsSelectionRequired(BOOL* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::isSelectionRequired, &AccessibilityNode::IsSelectionSupported);
}

// --- ISelectionItemProvider ---
HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::Select() {
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsSelectionItemSupported()) return UIA_E_NOTSUPPORTED;
    if (node.onSelect) { node.onSelect(childId_); } else if (node.onDefaultAction) { node.onDefaultAction(childId_); }
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (tree_) {
        LONG containerId = node.selectionContainerId;
        std::vector<LONG> deselected;
        {
            std::lock_guard<std::mutex> treeLock(tree_->mutex_);
            for (AccessibilityNode& n : tree_->nodes_) {
                if (n.id != childId_ && (containerId == 0 || n.selectionContainerId == containerId) && n.selected) {
                    n.selected = false;
                    deselected.push_back(n.id);
                } else if (n.id == childId_) { n.selected = true; }
            }
        }
        for (LONG id : deselected) {
            tree_->NotifySimple(EVENT_OBJECT_STATECHANGE, id);
            RaiseValueChange(*tree_, id, UIA_SelectionItemIsSelectedPropertyId, true, false);
            tree_->RaiseUiaEvent(id, UIA_SelectionItem_ElementRemovedFromSelectionEventId);
        }
        tree_->SetSelectedNode(childId_, true);
        tree_->RaiseUiaEvent(childId_, UIA_SelectionItem_ElementSelectedEventId);
        if (containerId > 0) { tree_->RaiseUiaEvent(containerId, UIA_Selection_InvalidatedEventId); }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::AddToSelection() {
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsSelectionItemSupported()) return UIA_E_NOTSUPPORTED;
    if (node.onAddToSelection) { node.onAddToSelection(childId_); } else if (node.onSelect) {
        node.onSelect(childId_);
    } else if (node.onDefaultAction) { node.onDefaultAction(childId_); }
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (tree_) {
        tree_->SetSelectedNode(childId_, true);
        tree_->RaiseUiaEvent(childId_, UIA_SelectionItem_ElementAddedToSelectionEventId);
        if (node.selectionContainerId > 0) { tree_->RaiseUiaEvent(node.selectionContainerId, UIA_Selection_InvalidatedEventId); }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::RemoveFromSelection() {
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsSelectionItemSupported()) return UIA_E_NOTSUPPORTED;
    if (node.onRemoveFromSelection) { node.onRemoveFromSelection(childId_); }
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (tree_) {
        tree_->SetSelectedNode(childId_, false);
        tree_->RaiseUiaEvent(childId_, UIA_SelectionItem_ElementRemovedFromSelectionEventId);
        if (node.selectionContainerId > 0) { tree_->RaiseUiaEvent(node.selectionContainerId, UIA_Selection_InvalidatedEventId); }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_IsSelected(BOOL* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::selected, &AccessibilityNode::IsSelectionItemSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_SelectionContainer(IRawElementProviderSimple** pRetVal) {
    if (!pRetVal) return E_POINTER;
    *pRetVal = nullptr;
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    std::lock_guard<std::recursive_mutex> lock(detachMutex_);
    if (!tree_) return UIA_E_ELEMENTNOTAVAILABLE;
    if (node.selectionContainerId > 0) {
        std::lock_guard<std::mutex> treeLock(tree_->mutex_);
        const auto it = std::find_if(tree_->nodes_.begin(), tree_->nodes_.end(),
                                     [id = node.selectionContainerId](const AccessibilityNode &n) { return n.id == id; });
        if (it != tree_->nodes_.end()) {
            AccessibilityChildProvider* container = tree_->GetOrCreateChildProviderLocked(node.selectionContainerId);
            if (container) { *pRetVal = static_cast<IRawElementProviderSimple*>(container); container->AddRef(); return S_OK; }
        }
    }
    *pRetVal = static_cast<IRawElementProviderSimple*>(tree_);
    tree_->AddRef();
    return S_OK;
}

// --- IScrollItemProvider ---
HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::ScrollIntoView() {
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsScrollItemSupported()) return UIA_E_NOTSUPPORTED;
    if (node.onScrollIntoView) { node.onScrollIntoView(childId_); return S_OK; }
    return S_FALSE;
}

// --- IScrollProvider ---
HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::Scroll(ScrollAmount horizontalAmount, ScrollAmount verticalAmount) {
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsScrollSupported()) return UIA_E_NOTSUPPORTED;
    if (static_cast<int>(horizontalAmount) < ScrollAmount_LargeDecrement || static_cast<int>(horizontalAmount) > ScrollAmount_SmallIncrement ||
        static_cast<int>(verticalAmount) < ScrollAmount_LargeDecrement || static_cast<int>(verticalAmount) > ScrollAmount_SmallIncrement) {
        return E_INVALIDARG;
    }
    if (horizontalAmount != ScrollAmount_NoAmount && !node.horizontallyScrollable) { return UIA_E_INVALIDOPERATION; }
    if (verticalAmount != ScrollAmount_NoAmount && !node.verticallyScrollable) { return UIA_E_INVALIDOPERATION; }
    if (horizontalAmount == ScrollAmount_NoAmount && verticalAmount == ScrollAmount_NoAmount) { return S_OK; }
    if (node.onScroll) { node.onScroll(childId_, horizontalAmount, verticalAmount); return S_OK; }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::SetScrollPercent(double horizontalPercent, double verticalPercent) {
    AccessibilityNode node;
    if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!node.IsScrollSupported()) return UIA_E_NOTSUPPORTED;
    const bool hasHScroll = (horizontalPercent != UIA_ScrollPatternNoScroll);
    const bool hasVScroll = (verticalPercent != UIA_ScrollPatternNoScroll);
    if (hasHScroll) {
        if (horizontalPercent < 0.0 || horizontalPercent > 100.0) { return E_INVALIDARG; }
        if (!node.horizontallyScrollable) { return UIA_E_INVALIDOPERATION; }
    }
    if (hasVScroll) {
        if (verticalPercent < 0.0 || verticalPercent > 100.0) { return E_INVALIDARG; }
        if (!node.verticallyScrollable) { return UIA_E_INVALIDOPERATION; }
    }
    if (!hasHScroll && !hasVScroll) { return S_OK; }
    if (node.onSetScrollPercent) { node.onSetScrollPercent(childId_, horizontalPercent, verticalPercent); return S_OK; }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_HorizontalScrollPercent(double* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::horizontalScrollPercent, &AccessibilityNode::IsScrollSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_VerticalScrollPercent(double* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::verticalScrollPercent, &AccessibilityNode::IsScrollSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_HorizontalViewSize(double* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::horizontalViewSize, &AccessibilityNode::IsScrollSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_VerticalViewSize(double* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::verticalViewSize, &AccessibilityNode::IsScrollSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_HorizontallyScrollable(BOOL* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::horizontallyScrollable, &AccessibilityNode::IsScrollSupported);
}

HRESULT STDMETHODCALLTYPE AccessibilityChildProvider::get_VerticallyScrollable(BOOL* pRetVal) {
    return ReadNodeProperty(pRetVal, &AccessibilityNode::verticallyScrollable, &AccessibilityNode::IsScrollSupported);
}

} // namespace Aperture
