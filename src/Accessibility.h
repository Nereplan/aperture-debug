#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <uiautomationcoreapi.h>

#ifndef UIA_LiveSettingPropertyId
#define UIA_LiveSettingPropertyId 30135
#endif
#ifndef UIA_LiveRegionChangedEventId
#define UIA_LiveRegionChangedEventId 20024
#endif
#ifndef UIA_ScrollPatternNoScroll
#define UIA_ScrollPatternNoScroll (-1.0)
#endif

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aperture {

// A flat accessible node is sufficient for the custom-drawn controls in the
// client: the tree itself is the OBJID_CLIENT parent / UiaRootObjectId root,
// and every node is a stable positive child ID. Bounds are in screen coordinates,
// as required by IAccessible and IRawElementProviderFragment.
struct AccessibilityNode {
    LONG id = 0;
    std::wstring name, description, value, defaultAction;
    std::wstring keyboardShortcut;
    LONG role = ROLE_SYSTEM_PUSHBUTTON, state = STATE_SYSTEM_FOCUSABLE;
    RECT bounds{};
    bool selected = false, focused = false;
    std::function<void(LONG)> onDefaultAction;
    // --- UIA Semantic Metadata Extensions ---
    LONG controlTypeId = 0;                 // UIA ControlType ID (e.g. UIA_ButtonControlTypeId). 0 = auto-map from role.
    std::wstring automationId;              // Stable AutomationId. If empty, falls back to L"Node_" + id.
    std::wstring className;                 // Control class name (e.g. L"ApertureButton"). Empty = L"ApertureControl".
    std::wstring itemStatus;                // Semantic status (e.g. "Playing", "Paused", "Muted").
    std::wstring helpText;                  // HelpText property. Falls back to description if empty.
    std::wstring accessKey;                 // Access key / shortcut. Falls back to keyboardShortcut if empty.
    std::wstring localizedControlType;      // Custom localized control type string.
    std::wstring itemType;                  // Semantic item type string (e.g. L"Media Item", L"Playlist Item", L"Setting Option").
    std::wstring ariaRole;                  // ARIA role string (e.g. L"dialog", L"search", L"tablist", L"menu").
    std::wstring ariaProperties;            // ARIA properties string.
    bool isPassword = false;
    LiveSetting liveSetting = Off;
    // --- Semantic Relationships & Hierarchy ---
    LONG labeledById = 0;                   // Node ID that labels this control (UIA_LabeledByPropertyId).
    std::vector<LONG> describedByIds;       // Node IDs describing this control (UIA_DescribedByPropertyId).
    std::vector<LONG> controllerForIds;     // Node IDs controlled by this control (UIA_ControllerForPropertyId).
    std::vector<LONG> controlledPeerIds;    // Node IDs controlling this control (UIA_ControlledPeersPropertyId).
    std::vector<LONG> flowsToIds;           // Reading/logical flow successor node IDs (UIA_FlowsToPropertyId).
    std::vector<LONG> flowsFromIds;         // Reading/logical flow predecessor node IDs (UIA_FlowsFromPropertyId).
    LONG selectionContainerId = 0;          // Selection container ID for ISelectionItemProvider::get_SelectionContainer.
    LONG groupId = 0;                       // Group / container / dialog owner ID.
    int positionInSet = 0;                  // 1-based position in set (UIA_PositionInSetPropertyId).
    int sizeOfSet = 0;                      // Total count in set (UIA_SizeOfSetPropertyId).
    int level = 0;                          // Hierarchy depth level (UIA_LevelPropertyId).
    // --- Pattern Capabilities & Handlers ---
    // 1. Invoke Pattern (IInvokeProvider)
    bool hasInvoke = false;
    std::function<void(LONG)> onInvoke;
    // 2. Toggle Pattern (IToggleProvider)
    bool hasToggle = false;
    ToggleState toggleState = ToggleState_Off;
    std::function<void(LONG, ToggleState)> onToggle;
    // 3. Value Pattern (IValueProvider)
    bool hasValue = false, isReadOnly = true;
    std::function<void(LONG, const std::wstring&)> onSetValue;
    // 4. RangeValue Pattern (IRangeValueProvider)
    bool hasRangeValue = false;
    double rangeValue = 0.0, rangeMin = 0.0, rangeMax = 100.0, rangeSmallChange = 1.0;
    double rangeLargeChange = 10.0;
    bool rangeIsReadOnly = false;
    std::function<void(LONG, double)> onSetRangeValue;
    // 5. Selection Pattern (ISelectionProvider) - for containers
    bool hasSelection = false, canSelectMultiple = false, isSelectionRequired = false;
    std::function<std::vector<LONG>(LONG)> onGetSelection;
    // 6. SelectionItem Pattern (ISelectionItemProvider)
    bool hasSelectionItem = false;
    std::function<void(LONG)> onSelect;
    std::function<void(LONG)> onAddToSelection;
    std::function<void(LONG)> onRemoveFromSelection;
    // 7. ScrollItem Pattern (IScrollItemProvider)
    bool hasScrollItem = false;
    std::function<void(LONG)> onScrollIntoView;
    // 8. Scroll Pattern (IScrollProvider) - for containers
    bool hasScroll = false;
    double horizontalScrollPercent = -1.0; // UIA_ScrollPatternNoScroll
    double verticalScrollPercent = -1.0;   // UIA_ScrollPatternNoScroll
    double horizontalViewSize = 100.0, verticalViewSize = 100.0;
    bool horizontallyScrollable = false, verticallyScrollable = false;
    std::function<void(LONG, ScrollAmount, ScrollAmount)> onScroll;
    std::function<void(LONG, double, double)> onSetScrollPercent;
    // --- Capability Helpers ---
    bool IsInvokeSupported() const;
    bool IsToggleSupported() const;
    bool IsValueSupported() const;
    bool IsRangeValueSupported() const;
    bool IsSelectionSupported() const;
    bool IsSelectionItemSupported() const;
    bool IsScrollItemSupported() const;
    bool IsScrollSupported() const;
    LONG GetEffectiveControlTypeId() const;
};

class AccessibilityChildProvider;

class AccessibilityTree final : public IAccessible, public IRawElementProviderSimple, public IRawElementProviderFragment,
                                public IRawElementProviderFragmentRoot, public IRawElementProviderAdviseEvents {
public:
    explicit AccessibilityTree(HWND hwnd = nullptr);
    ~AccessibilityTree();
    AccessibilityTree(const AccessibilityTree&) = delete;
    AccessibilityTree& operator=(const AccessibilityTree&) = delete;
    // The HWND is borrowed; clear it before destroying the window. A null
    // window makes subsequent accessibility notifications no-ops.
    void SetWindow(HWND hwnd);
    HWND GetWindow() const;
    // IDs must be positive and unique. Replacing a snapshot preserves the caller's IDs, allowing Narrator / UIA focus to remain attached to
    // a control. The tree is normally a member/borrowed provider owned by App, so its owner must keep it alive for the HWND lifetime and
    // release COM clients before destroying the tree.
    bool ReplaceNodes(const std::vector<AccessibilityNode>& nodes);
    void ClearNodes();
    std::vector<AccessibilityNode> GetNodes() const;
    bool GetNodeCopy(LONG childId, AccessibilityNode& outNode) const;
    bool SetFocusedNode(LONG childId);
    bool SetSelectedNode(LONG childId, bool selected = true);
    bool AnnounceLiveRegion(LONG childId, const std::wstring& text);
    // Call from the window procedure for WM_GETOBJECT. Supports both OBJID_CLIENT (for MSAA clients) and UiaRootObjectId (for UIA clients).
    LRESULT HandleGetObject(WPARAM wParam, LPARAM lParam);
    // --- UIA Event Notification Helpers ---
    bool RaiseUiaEvent(LONG childId, EVENTID eventId) const;
    bool RaiseUiaPropertyChanged(LONG childId, PROPERTYID propertyId, const VARIANT& oldValue, const VARIANT& newValue) const;
    bool RaiseUiaStructureChanged(StructureChangeType changeType, LONG childId = 0) const;
    static bool AreUiaClientsListening();
    // Provider lookup
    IRawElementProviderSimple* GetRootProvider();
    IRawElementProviderSimple* GetProviderForNode(LONG childId);
    // IUnknown / IDispatch / IAccessible (MSAA)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override;
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT index, LCID lcid, ITypeInfo** typeInfo) override;
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID riid, LPOLESTR* names, UINT nameCount, LCID lcid, DISPID* dispIds) override;
    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispId, REFIID riid, LCID lcid, WORD flags,
        DISPPARAMS* params, VARIANT* result, EXCEPINFO* exception, UINT* argError) override;
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override;
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* childCount) override;
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT childId, IDispatch** child) override;
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT childId, BSTR* name) override;
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT childId, BSTR* value) override;
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT childId, BSTR* description) override;
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT childId, VARIANT* role) override;
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT childId, VARIANT* state) override;
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT childId, BSTR* help) override;
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* helpFile, VARIANT childId, long* topicId) override;
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT childId, BSTR* shortcut) override;
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* focusChild) override;
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* selection) override;
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT childId, BSTR* action) override;
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT childId) override;
    HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width, long* height, VARIANT childId) override;
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT startChild, VARIANT* endChild) override;
    HRESULT STDMETHODCALLTYPE accHitTest(long screenX, long screenY, VARIANT* childAtPoint) override;
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT childId) override;
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT childId, BSTR name) override;
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT childId, BSTR value) override;
    // IRawElementProviderSimple (Root)
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override;
    // IRawElementProviderFragment (Root)
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) override;
    // IRawElementProviderFragmentRoot (Root)
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** pRetVal) override;
    // IRawElementProviderAdviseEvents
    HRESULT STDMETHODCALLTYPE AdviseEventAdded(EVENTID eventId, SAFEARRAY* propertyIDs) override;
    HRESULT STDMETHODCALLTYPE AdviseEventRemoved(EVENTID eventId, SAFEARRAY* propertyIDs) override;

private:
    friend class AccessibilityChildProvider;
    struct NodeLookup { AccessibilityNode node; size_t index = 0; };
    bool LookupChild(const VARIANT& childId, NodeLookup& lookup) const;
    bool IsSelf(const VARIANT& childId) const;
    static void SetEmpty(VARIANT* value);
    static HRESULT SetBstr(const std::wstring& text, BSTR* output);
    void NotifyNodeChanges(const AccessibilityNode& old, const AccessibilityNode& node, bool uiaListening);
    void NotifySimple(DWORD eventId, LONG childId) const;
    void NotifyReorder() const;
    AccessibilityChildProvider* GetOrCreateChildProviderLocked(LONG childId);
    void InvalidateAndDisconnectChildProviders();
    std::atomic<ULONG> refCount_{1};
    mutable std::mutex mutex_;
    HWND hwnd_ = nullptr;
    std::vector<AccessibilityNode> nodes_;
    std::unordered_map<LONG, AccessibilityChildProvider*> childProviders_;
};

// Represents an accessible child item in the UI Automation fragment tree. Lifetime is managed by standard COM reference counting.
class AccessibilityChildProvider final : public IRawElementProviderSimple, public IRawElementProviderFragment, public IInvokeProvider,
                                         public IToggleProvider, public IValueProvider, public IRangeValueProvider,
                                         public ISelectionProvider, public ISelectionItemProvider, public IScrollItemProvider,
                                         public IScrollProvider {
public:
    AccessibilityChildProvider(LONG childId, AccessibilityTree* tree);
    ~AccessibilityChildProvider();
    void DetachTree();
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override;
    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* pRetVal) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) override;
    // IInvokeProvider
    HRESULT STDMETHODCALLTYPE Invoke() override;
    // IToggleProvider
    HRESULT STDMETHODCALLTYPE Toggle() override;
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* pRetVal) override;
    // Shared between IValueProvider and IRangeValueProvider
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* pRetVal) override;
    // IValueProvider
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR val) override;
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* pRetVal) override;
    // IRangeValueProvider
    HRESULT STDMETHODCALLTYPE SetValue(double val) override;
    HRESULT STDMETHODCALLTYPE get_Value(double* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_Maximum(double* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_Minimum(double* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* pRetVal) override;
    // ISelectionProvider
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* pRetVal) override;
    // ISelectionItemProvider
    HRESULT STDMETHODCALLTYPE Select() override;
    HRESULT STDMETHODCALLTYPE AddToSelection() override;
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override;
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** pRetVal) override;
    // IScrollItemProvider
    HRESULT STDMETHODCALLTYPE ScrollIntoView() override;
    // IScrollProvider
    HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount horizontalAmount, ScrollAmount verticalAmount) override;
    HRESULT STDMETHODCALLTYPE SetScrollPercent(double horizontalPercent, double verticalPercent) override;
    HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL* pRetVal) override;
    HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL* pRetVal) override;

private:
    bool GetNode(AccessibilityNode& node) const;
    template<class Out, class Value>
    HRESULT ReadNodeProperty(Out *result, Value AccessibilityNode::*property, bool (AccessibilityNode::*supported)() const) const {
        if (!result) return E_POINTER;
        AccessibilityNode node;
        if (!GetNode(node)) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!(node.*supported)()) return UIA_E_NOTSUPPORTED;
        *result = static_cast<Out>(node.*property);
        return S_OK;
    }
    SAFEARRAY* CreateProviderArrayForIds(const std::vector<LONG>& ids) const;
    LONG childId_ = 0;
    // UIA event publication can synchronously request this provider again.
    // Keep detachment excluded while allowing same-thread property reentry.
    mutable std::recursive_mutex detachMutex_;
    AccessibilityTree* tree_ = nullptr;
    std::atomic<ULONG> refCount_{1};
};

} // namespace Aperture
