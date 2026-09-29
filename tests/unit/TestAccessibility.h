#pragma once

#include "../support/TestFramework.h"
#include "../../src/Accessibility.h"

#include <windows.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <uiautomationcoreapi.h>

#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <cmath>

namespace Aperture::Test {

namespace {

// Lightweight RAII COM Smart Pointer for unit test assertions
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(T* p) : ptr_(p) {}
    ~ComPtr() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }
    ComPtr(const ComPtr& other) : ptr_(other.ptr_) {
        if (ptr_) ptr_->AddRef();
    }
    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            if (ptr_) ptr_->Release();
            ptr_ = other.ptr_;
            if (ptr_) ptr_->AddRef();
        }
        return *this;
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_) ptr_->Release();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* Get() const { return ptr_; }
    T** GetAddressOf() { Reset(); return &ptr_; }
    T** operator&() { Reset(); return &ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void Reset(T* p = nullptr) {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
        ptr_ = p;
    }

    T* Detach() {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }
private:
    T* ptr_ = nullptr;
};

// RAII Wrapper for SAFEARRAY
class SafeArrayScope {
public:
    explicit SafeArrayScope(SAFEARRAY* sa = nullptr) : sa_(sa) {}
    ~SafeArrayScope() {
        if (sa_) {
            SafeArrayDestroy(sa_);
            sa_ = nullptr;
        }
    }
    SAFEARRAY* Get() const { return sa_; }
    SAFEARRAY** GetAddressOf() { Reset(); return &sa_; }
    SAFEARRAY** operator&() { Reset(); return &sa_; }
    explicit operator bool() const { return sa_ != nullptr; }
    void Reset(SAFEARRAY* sa = nullptr) {
        if (sa_) {
            SafeArrayDestroy(sa_);
            sa_ = nullptr;
        }
        sa_ = sa;
    }
    SAFEARRAY* sa_ = nullptr;
};

// RAII Wrapper for BSTR
class BstrScope {
public:
    explicit BstrScope(BSTR bstr = nullptr) : bstr_(bstr) {}
    ~BstrScope() {
        if (bstr_) {
            SysFreeString(bstr_);
            bstr_ = nullptr;
        }
    }
    BSTR Get() const { return bstr_; }
    BSTR* GetAddressOf() { Reset(); return &bstr_; }
    BSTR* operator&() { Reset(); return &bstr_; }
    explicit operator bool() const { return bstr_ != nullptr; }
    std::wstring ToWString() const { return bstr_ ? std::wstring(bstr_) : std::wstring(); }
    void Reset(BSTR bstr = nullptr) {
        if (bstr_) {
            SysFreeString(bstr_);
            bstr_ = nullptr;
        }
        bstr_ = bstr;
    }
    BSTR bstr_ = nullptr;
};

// RAII Wrapper for VARIANT
class VariantScope {
public:
    VariantScope() { VariantInit(&var_); }
    ~VariantScope() { VariantClear(&var_); }
    VARIANT* Get() { return &var_; }
    VARIANT* GetAddressOf() { Reset(); return &var_; }
    VARIANT* operator&() { Reset(); return &var_; }
    const VARIANT& Ref() const { return var_; }
    void Reset() { VariantClear(&var_); VariantInit(&var_); }
private:
    VARIANT var_{};
};

} // namespace

// ============================================================================
// 1. Pattern Exposure Tests for Representative Controls
// ============================================================================

TEST_CASE(Accessibility, PatternExposure) {
    AccessibilityTree tree;

    std::vector<AccessibilityNode> nodes;

    // Node 1: Push Button (Invoke pattern)
    {
        AccessibilityNode n;
        n.id = 1;
        n.name = L"Play";
        n.role = ROLE_SYSTEM_PUSHBUTTON;
        n.hasInvoke = true;
        n.onInvoke = [](LONG) {};
        nodes.push_back(n);
    }

    // Node 2: Checkbox (Toggle pattern)
    {
        AccessibilityNode n;
        n.id = 2;
        n.name = L"Mute";
        n.role = ROLE_SYSTEM_CHECKBUTTON;
        n.hasToggle = true;
        n.toggleState = ToggleState_Off;
        nodes.push_back(n);
    }

    // Node 3: Radio Button (SelectionItem + Toggle when checked)
    {
        AccessibilityNode n;
        n.id = 3;
        n.name = L"Option A";
        n.role = ROLE_SYSTEM_RADIOBUTTON;
        n.state = STATE_SYSTEM_CHECKED;
        n.hasSelectionItem = true;
        nodes.push_back(n);
    }

    // Node 4: Text Box / Search (Value pattern)
    {
        AccessibilityNode n;
        n.id = 4;
        n.name = L"Search";
        n.role = ROLE_SYSTEM_TEXT;
        n.hasValue = true;
        n.value = L"Initial Query";
        nodes.push_back(n);
    }

    // Node 5: Slider / Timeline (RangeValue pattern)
    {
        AccessibilityNode n;
        n.id = 5;
        n.name = L"Timeline";
        n.role = ROLE_SYSTEM_SLIDER;
        n.hasRangeValue = true;
        n.rangeMin = 0.0;
        n.rangeMax = 100.0;
        n.rangeValue = 25.0;
        nodes.push_back(n);
    }

    // Node 6: List / Tab Container (Selection pattern)
    {
        AccessibilityNode n;
        n.id = 6;
        n.name = L"Playlist";
        n.role = ROLE_SYSTEM_LIST;
        n.hasSelection = true;
        nodes.push_back(n);
    }

    // Node 7: List Item (SelectionItem pattern)
    {
        AccessibilityNode n;
        n.id = 7;
        n.name = L"Track 1";
        n.role = ROLE_SYSTEM_LISTITEM;
        n.hasSelectionItem = true;
        n.selectionContainerId = 6;
        nodes.push_back(n);
    }

    // Node 8: Scrollable View (Scroll pattern)
    {
        AccessibilityNode n;
        n.id = 8;
        n.name = L"Scrollable Pane";
        n.role = ROLE_SYSTEM_PANE;
        n.verticallyScrollable = true;
        n.hasScroll = true;
        nodes.push_back(n);
    }

    // Node 9: Scroll Item (ScrollItem pattern)
    {
        AccessibilityNode n;
        n.id = 9;
        n.name = L"Deep Item";
        n.role = ROLE_SYSTEM_LISTITEM;
        n.hasScrollItem = true;
        n.onScrollIntoView = [](LONG) {};
        nodes.push_back(n);
    }

    TEST_ASSERT(tree.ReplaceNodes(nodes));

    // Verify Push Button (Node 1)
    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(1));
        TEST_ASSERT(pProv.Get() != nullptr);

        ProviderOptions options{};
        TEST_ASSERT_EQ(pProv->get_ProviderOptions(&options), S_OK);
        TEST_ASSERT_EQ(static_cast<int>(options),
            static_cast<int>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading));

        ComPtr<IUnknown> pUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_InvokePatternId, &pUnk), S_OK);
        TEST_ASSERT(pUnk.Get() != nullptr);

        ComPtr<IInvokeProvider> pInvoke;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IInvokeProvider), reinterpret_cast<void**>(&pInvoke)), S_OK);
        TEST_ASSERT(pInvoke.Get() != nullptr);

        // Negative check: Push button must not expose Toggle or RangeValue
        ComPtr<IUnknown> pToggleUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_TogglePatternId, &pToggleUnk), S_OK);
        TEST_ASSERT(pToggleUnk.Get() == nullptr);

        ComPtr<IToggleProvider> pToggle;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IToggleProvider), reinterpret_cast<void**>(&pToggle)), E_NOINTERFACE);
    }

    // Verify Checkbox (Node 2)
    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(2));
        TEST_ASSERT(pProv.Get() != nullptr);

        ComPtr<IUnknown> pUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_TogglePatternId, &pUnk), S_OK);
        TEST_ASSERT(pUnk.Get() != nullptr);

        // Must not expose Invoke
        ComPtr<IUnknown> pInvokeUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_InvokePatternId, &pInvokeUnk), S_OK);
        TEST_ASSERT(pInvokeUnk.Get() == nullptr);
    }

    // Verify Radio Button (Node 3)
    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(3));
        TEST_ASSERT(pProv.Get() != nullptr);

        ComPtr<IUnknown> pSelItemUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_SelectionItemPatternId, &pSelItemUnk), S_OK);
        TEST_ASSERT(pSelItemUnk.Get() != nullptr);

        ComPtr<IUnknown> pToggleUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_TogglePatternId, &pToggleUnk), S_OK);
        TEST_ASSERT(pToggleUnk.Get() != nullptr);
    }

    // Verify Text Input (Node 4)
    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(4));
        TEST_ASSERT(pProv.Get() != nullptr);

        ComPtr<IUnknown> pValUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_ValuePatternId, &pValUnk), S_OK);
        TEST_ASSERT(pValUnk.Get() != nullptr);

        ComPtr<IValueProvider> pValue;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IValueProvider), reinterpret_cast<void**>(&pValue)), S_OK);
        TEST_ASSERT(pValue.Get() != nullptr);
    }

    // Verify Slider (Node 5)
    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(5));
        TEST_ASSERT(pProv.Get() != nullptr);

        ComPtr<IUnknown> pRangeUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_RangeValuePatternId, &pRangeUnk), S_OK);
        TEST_ASSERT(pRangeUnk.Get() != nullptr);

        ComPtr<IRangeValueProvider> pRange;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IRangeValueProvider), reinterpret_cast<void**>(&pRange)), S_OK);
        TEST_ASSERT(pRange.Get() != nullptr);
    }

    // Verify List Container (Node 6)
    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(6));
        TEST_ASSERT(pProv.Get() != nullptr);

        ComPtr<IUnknown> pSelUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_SelectionPatternId, &pSelUnk), S_OK);
        TEST_ASSERT(pSelUnk.Get() != nullptr);
    }

    // Verify Scrollable Container (Node 8)
    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(8));
        TEST_ASSERT(pProv.Get() != nullptr);

        ComPtr<IUnknown> pScrollUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_ScrollPatternId, &pScrollUnk), S_OK);
        TEST_ASSERT(pScrollUnk.Get() != nullptr);
    }

    // Verify Scroll Item (Node 9)
    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(9));
        TEST_ASSERT(pProv.Get() != nullptr);

        ComPtr<IUnknown> pScrollItemUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_ScrollItemPatternId, &pScrollItemUnk), S_OK);
        TEST_ASSERT(pScrollItemUnk.Get() != nullptr);
    }
}

// ============================================================================
// 2. Selection Pattern: Enumeration, Single-Select, Multi-Select, Add/Remove
// ============================================================================

TEST_CASE(Accessibility, SelectionEnumerationAddRemove) {
    AccessibilityTree tree;

    std::vector<AccessibilityNode> nodes;

    // Container 100
    {
        AccessibilityNode n;
        n.id = 100;
        n.name = L"Media Playlist";
        n.role = ROLE_SYSTEM_LIST;
        n.hasSelection = true;
        n.canSelectMultiple = false;
        n.isSelectionRequired = true;
        nodes.push_back(n);
    }

    // Items 101, 102, 103
    for (LONG id = 101; id <= 103; ++id) {
        AccessibilityNode n;
        n.id = id;
        n.name = L"Item " + std::to_wstring(id);
        n.role = ROLE_SYSTEM_LISTITEM;
        n.hasSelectionItem = true;
        n.selectionContainerId = 100;
        n.selected = false;
        nodes.push_back(n);
    }

    TEST_ASSERT(tree.ReplaceNodes(nodes));

    ComPtr<IRawElementProviderSimple> pContProv(tree.GetProviderForNode(100));
    TEST_ASSERT(pContProv.Get() != nullptr);
    ComPtr<ISelectionProvider> pSelection;
    TEST_ASSERT_EQ(pContProv->QueryInterface(__uuidof(ISelectionProvider), reinterpret_cast<void**>(&pSelection)), S_OK);
    TEST_ASSERT(pSelection.Get() != nullptr);

    BOOL canMulti = TRUE;
    TEST_ASSERT_EQ(pSelection->get_CanSelectMultiple(&canMulti), S_OK);
    TEST_ASSERT_EQ(canMulti, FALSE);

    BOOL isReq = FALSE;
    TEST_ASSERT_EQ(pSelection->get_IsSelectionRequired(&isReq), S_OK);
    TEST_ASSERT_EQ(isReq, TRUE);

    // Initially no items selected
    {
        SafeArrayScope sa;
        TEST_ASSERT_EQ(pSelection->GetSelection(&sa), S_OK);
        TEST_ASSERT(sa.Get() != nullptr);
        LONG lBound = 0, uBound = -1;
        SafeArrayGetLBound(sa.Get(), 1, &lBound);
        SafeArrayGetUBound(sa.Get(), 1, &uBound);
        LONG count = (uBound >= lBound) ? (uBound - lBound + 1) : 0;
        TEST_ASSERT_EQ(count, 0);
    }

    // 1. Select Item 101
    {
        ComPtr<IRawElementProviderSimple> pItem1(tree.GetProviderForNode(101));
        ComPtr<ISelectionItemProvider> pSelItem1;
        TEST_ASSERT_EQ(pItem1->QueryInterface(__uuidof(ISelectionItemProvider), reinterpret_cast<void**>(&pSelItem1)), S_OK);
        TEST_ASSERT_EQ(pSelItem1->Select(), S_OK);

        BOOL isSel = FALSE;
        TEST_ASSERT_EQ(pSelItem1->get_IsSelected(&isSel), S_OK);
        TEST_ASSERT_EQ(isSel, TRUE);

        // Verify selection container query
        SafeArrayScope sa;
        TEST_ASSERT_EQ(pSelection->GetSelection(&sa), S_OK);
        TEST_ASSERT(sa.Get() != nullptr);
        LONG lBound = 0, uBound = -1;
        SafeArrayGetLBound(sa.Get(), 1, &lBound);
        SafeArrayGetUBound(sa.Get(), 1, &uBound);
        LONG count = (uBound >= lBound) ? (uBound - lBound + 1) : 0;
        TEST_ASSERT_EQ(count, 1);
    }

    // 2. Select Item 102 (Single-selection: 101 must be deselected, 102 selected)
    {
        ComPtr<IRawElementProviderSimple> pItem2(tree.GetProviderForNode(102));
        ComPtr<ISelectionItemProvider> pSelItem2;
        TEST_ASSERT_EQ(pItem2->QueryInterface(__uuidof(ISelectionItemProvider), reinterpret_cast<void**>(&pSelItem2)), S_OK);
        TEST_ASSERT_EQ(pSelItem2->Select(), S_OK);

        BOOL isSel2 = FALSE;
        TEST_ASSERT_EQ(pSelItem2->get_IsSelected(&isSel2), S_OK);
        TEST_ASSERT_EQ(isSel2, TRUE);

        ComPtr<IRawElementProviderSimple> pItem1(tree.GetProviderForNode(101));
        ComPtr<ISelectionItemProvider> pSelItem1;
        TEST_ASSERT_EQ(pItem1->QueryInterface(__uuidof(ISelectionItemProvider), reinterpret_cast<void**>(&pSelItem1)), S_OK);
        BOOL isSel1 = TRUE;
        TEST_ASSERT_EQ(pSelItem1->get_IsSelected(&isSel1), S_OK);
        TEST_ASSERT_EQ(isSel1, FALSE);

        // Container GetSelection must return only item 102
        SafeArrayScope sa;
        TEST_ASSERT_EQ(pSelection->GetSelection(&sa), S_OK);
        LONG lBound = 0, uBound = -1;
        SafeArrayGetLBound(sa.Get(), 1, &lBound);
        SafeArrayGetUBound(sa.Get(), 1, &uBound);
        LONG count = (uBound >= lBound) ? (uBound - lBound + 1) : 0;
        TEST_ASSERT_EQ(count, 1);
    }

    // 3. Multi-selection: Update container to canSelectMultiple = true
    {
        AccessibilityNode contNode;
        TEST_ASSERT(tree.GetNodeCopy(100, contNode));
        contNode.canSelectMultiple = true;
        TEST_ASSERT(tree.UpdateNode(contNode));

        ComPtr<IRawElementProviderSimple> pItem3(tree.GetProviderForNode(103));
        ComPtr<ISelectionItemProvider> pSelItem3;
        TEST_ASSERT_EQ(pItem3->QueryInterface(__uuidof(ISelectionItemProvider), reinterpret_cast<void**>(&pSelItem3)), S_OK);
        TEST_ASSERT_EQ(pSelItem3->AddToSelection(), S_OK);

        BOOL isSel3 = FALSE;
        TEST_ASSERT_EQ(pSelItem3->get_IsSelected(&isSel3), S_OK);
        TEST_ASSERT_EQ(isSel3, TRUE);

        // Both 102 and 103 are now selected
        SafeArrayScope sa;
        TEST_ASSERT_EQ(pSelection->GetSelection(&sa), S_OK);
        LONG lBound = 0, uBound = -1;
        SafeArrayGetLBound(sa.Get(), 1, &lBound);
        SafeArrayGetUBound(sa.Get(), 1, &uBound);
        LONG count = (uBound >= lBound) ? (uBound - lBound + 1) : 0;
        TEST_ASSERT_EQ(count, 2);

        // Remove 102 from selection
        ComPtr<IRawElementProviderSimple> pItem2(tree.GetProviderForNode(102));
        ComPtr<ISelectionItemProvider> pSelItem2;
        TEST_ASSERT_EQ(pItem2->QueryInterface(__uuidof(ISelectionItemProvider), reinterpret_cast<void**>(&pSelItem2)), S_OK);
        TEST_ASSERT_EQ(pSelItem2->RemoveFromSelection(), S_OK);

        BOOL isSel2 = TRUE;
        TEST_ASSERT_EQ(pSelItem2->get_IsSelected(&isSel2), S_OK);
        TEST_ASSERT_EQ(isSel2, FALSE);

        // Only 103 remains selected
        SafeArrayScope saAfter;
        TEST_ASSERT_EQ(pSelection->GetSelection(&saAfter), S_OK);
        SafeArrayGetLBound(saAfter.Get(), 1, &lBound);
        SafeArrayGetUBound(saAfter.Get(), 1, &uBound);
        count = (uBound >= lBound) ? (uBound - lBound + 1) : 0;
        TEST_ASSERT_EQ(count, 1);

        // Verify get_SelectionContainer on item 103 returns container 100
        ComPtr<IRawElementProviderSimple> pContainer;
        TEST_ASSERT_EQ(pSelItem3->get_SelectionContainer(&pContainer), S_OK);
        TEST_ASSERT(pContainer.Get() != nullptr);
    }
}

// ============================================================================
// 3. Vertical Scroll and No-Scroll Semantics
// ============================================================================

TEST_CASE(Accessibility, VerticalScrollAndNoScrollSemantics) {
    AccessibilityTree tree;

    int scrollCallCount = 0;
    ScrollAmount lastHScrollAmount = ScrollAmount_NoAmount;
    ScrollAmount lastVScrollAmount = ScrollAmount_NoAmount;

    int setPercentCallCount = 0;
    double lastHPercent = 0.0;
    double lastVPercent = 0.0;

    AccessibilityNode n;
    n.id = 200;
    n.name = L"Playlist Scroll Container";
    n.role = ROLE_SYSTEM_PANE;
    n.verticallyScrollable = true;
    n.horizontallyScrollable = false;
    n.verticalScrollPercent = 15.0;
    n.horizontalScrollPercent = UIA_ScrollPatternNoScroll; // -1.0
    n.verticalViewSize = 25.0;
    n.horizontalViewSize = 100.0;
    n.hasScroll = true;
    n.onScroll = [&](LONG, ScrollAmount h, ScrollAmount v) {
        scrollCallCount++;
        lastHScrollAmount = h;
        lastVScrollAmount = v;
    };
    n.onSetScrollPercent = [&](LONG, double h, double v) {
        setPercentCallCount++;
        lastHPercent = h;
        lastVPercent = v;
    };

    TEST_ASSERT(tree.ReplaceNodes({ n }));

    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(200));
        TEST_ASSERT(pProv.Get() != nullptr);

        ComPtr<IScrollProvider> pScroll;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IScrollProvider), reinterpret_cast<void**>(&pScroll)), S_OK);
        TEST_ASSERT(pScroll.Get() != nullptr);

        // Capabilities inspection
        BOOL vScrollable = FALSE;
        TEST_ASSERT_EQ(pScroll->get_VerticallyScrollable(&vScrollable), S_OK);
        TEST_ASSERT_EQ(vScrollable, TRUE);

        BOOL hScrollable = TRUE;
        TEST_ASSERT_EQ(pScroll->get_HorizontallyScrollable(&hScrollable), S_OK);
        TEST_ASSERT_EQ(hScrollable, FALSE);

        double vPercent = 0.0;
        TEST_ASSERT_EQ(pScroll->get_VerticalScrollPercent(&vPercent), S_OK);
        TEST_ASSERT_EQ(vPercent, 15.0);

        double hPercent = 0.0;
        TEST_ASSERT_EQ(pScroll->get_HorizontalScrollPercent(&hPercent), S_OK);
        TEST_ASSERT_EQ(hPercent, UIA_ScrollPatternNoScroll);

        double vViewSize = 0.0;
        TEST_ASSERT_EQ(pScroll->get_VerticalViewSize(&vViewSize), S_OK);
        TEST_ASSERT_EQ(vViewSize, 25.0);

        double hViewSize = 0.0;
        TEST_ASSERT_EQ(pScroll->get_HorizontalViewSize(&hViewSize), S_OK);
        TEST_ASSERT_EQ(hViewSize, 100.0);

        // 1. Valid Vertical Scroll call
        TEST_ASSERT_EQ(pScroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallIncrement), S_OK);
        TEST_ASSERT_EQ(scrollCallCount, 1);
        TEST_ASSERT_EQ(static_cast<int>(lastVScrollAmount), static_cast<int>(ScrollAmount_SmallIncrement));
        TEST_ASSERT_EQ(static_cast<int>(lastHScrollAmount), static_cast<int>(ScrollAmount_NoAmount));

        // 2. Horizontal Scroll on non-horizontally-scrollable view must fail with UIA_E_INVALIDOPERATION
        TEST_ASSERT_EQ(pScroll->Scroll(ScrollAmount_SmallIncrement, ScrollAmount_NoAmount), UIA_E_INVALIDOPERATION);

        // 3. NoAmount on both axes succeeds with no-op
        TEST_ASSERT_EQ(pScroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_NoAmount), S_OK);

        // 5. Valid SetScrollPercent for vertical axis with UIA_ScrollPatternNoScroll for horizontal
        TEST_ASSERT_EQ(pScroll->SetScrollPercent(UIA_ScrollPatternNoScroll, 50.0), S_OK);
        TEST_ASSERT_EQ(setPercentCallCount, 1);
        TEST_ASSERT_EQ(lastVPercent, 50.0);
        TEST_ASSERT_EQ(lastHPercent, UIA_ScrollPatternNoScroll);

        // 6. Setting horizontal percent on non-scrollable axis returns UIA_E_INVALIDOPERATION
        TEST_ASSERT_EQ(pScroll->SetScrollPercent(50.0, 50.0), UIA_E_INVALIDOPERATION);

        // 7. Out of bounds percentages (< 0 or > 100, excluding UIA_ScrollPatternNoScroll) return E_INVALIDARG
        TEST_ASSERT_EQ(pScroll->SetScrollPercent(UIA_ScrollPatternNoScroll, 120.0), E_INVALIDARG);
        TEST_ASSERT_EQ(pScroll->SetScrollPercent(UIA_ScrollPatternNoScroll, -5.0), E_INVALIDARG);
    }

    // 8. Non-scrollable control does not expose IScrollProvider
    {
        AccessibilityTree nonScrollTree;
        AccessibilityNode nonScrollNode;
        nonScrollNode.id = 201;
        nonScrollNode.name = L"Static Panel";
        nonScrollNode.role = ROLE_SYSTEM_PANE;
        nonScrollNode.verticallyScrollable = false;
        nonScrollNode.horizontallyScrollable = false;
        TEST_ASSERT(nonScrollTree.ReplaceNodes({ nonScrollNode }));

        ComPtr<IRawElementProviderSimple> pNonScrollProv(nonScrollTree.GetProviderForNode(201));
        ComPtr<IUnknown> pUnk;
        TEST_ASSERT_EQ(pNonScrollProv->GetPatternProvider(UIA_ScrollPatternId, &pUnk), S_OK);
        TEST_ASSERT(pUnk.Get() == nullptr);
    }
}

// ============================================================================
// 4. Timeline / RangeValue Semantics and Value Clamping
// ============================================================================

TEST_CASE(Accessibility, TimelineRangeValueSemantics) {
    AccessibilityTree tree;

    double callbackValue = 0.0;
    int callbackCount = 0;

    AccessibilityNode n;
    n.id = 300;
    n.name = L"Playback Seekbar";
    n.role = ROLE_SYSTEM_SLIDER;
    n.hasRangeValue = true;
    n.rangeMin = 0.0;
    n.rangeMax = 120.0;
    n.rangeValue = 30.0;
    n.rangeSmallChange = 1.0;
    n.rangeLargeChange = 10.0;
    n.rangeIsReadOnly = false;
    n.onSetRangeValue = [&](LONG, double val) {
        callbackCount++;
        callbackValue = val;
    };

    TEST_ASSERT(tree.ReplaceNodes({ n }));

    {
        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(300));
        TEST_ASSERT(pProv.Get() != nullptr);

        ComPtr<IRangeValueProvider> pRange;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IRangeValueProvider), reinterpret_cast<void**>(&pRange)), S_OK);
        TEST_ASSERT(pRange.Get() != nullptr);

        // Property inspection
        double minVal = 0.0, maxVal = 0.0, curVal = 0.0, smallChg = 0.0, largeChg = 0.0;
        BOOL isReadOnly = TRUE;

        TEST_ASSERT_EQ(pRange->get_Minimum(&minVal), S_OK);
        TEST_ASSERT_EQ(minVal, 0.0);

        TEST_ASSERT_EQ(pRange->get_Maximum(&maxVal), S_OK);
        TEST_ASSERT_EQ(maxVal, 120.0);

        TEST_ASSERT_EQ(pRange->get_Value(&curVal), S_OK);
        TEST_ASSERT_EQ(curVal, 30.0);

        TEST_ASSERT_EQ(pRange->get_SmallChange(&smallChg), S_OK);
        TEST_ASSERT_EQ(smallChg, 1.0);

        TEST_ASSERT_EQ(pRange->get_LargeChange(&largeChg), S_OK);
        TEST_ASSERT_EQ(largeChg, 10.0);

        TEST_ASSERT_EQ(pRange->get_IsReadOnly(&isReadOnly), S_OK);
        TEST_ASSERT_EQ(isReadOnly, FALSE);

        // 1. Valid SetValue inside [min, max]
        TEST_ASSERT_EQ(pRange->SetValue(65.5), S_OK);
        TEST_ASSERT_EQ(callbackCount, 1);
        TEST_ASSERT_EQ(callbackValue, 65.5);

        double updatedVal = 0.0;
        TEST_ASSERT_EQ(pRange->get_Value(&updatedVal), S_OK);
        TEST_ASSERT_EQ(updatedVal, 65.5);

        // 2. SetValue below minimum (must clamp to min = 0.0)
        TEST_ASSERT_EQ(pRange->SetValue(-25.0), S_OK);
        TEST_ASSERT_EQ(callbackCount, 2);
        TEST_ASSERT_EQ(callbackValue, 0.0);
        TEST_ASSERT_EQ(pRange->get_Value(&updatedVal), S_OK);
        TEST_ASSERT_EQ(updatedVal, 0.0);

        // 3. SetValue above maximum (must clamp to max = 120.0)
        TEST_ASSERT_EQ(pRange->SetValue(250.0), S_OK);
        TEST_ASSERT_EQ(callbackCount, 3);
        TEST_ASSERT_EQ(callbackValue, 120.0);
        TEST_ASSERT_EQ(pRange->get_Value(&updatedVal), S_OK);
        TEST_ASSERT_EQ(updatedVal, 120.0);
    }

    // 4. Read-Only Range Value Node
    {
        AccessibilityTree readOnlyTree;
        AccessibilityNode roNode;
        roNode.id = 301;
        roNode.name = L"Buffer Progress";
        roNode.role = ROLE_SYSTEM_PROGRESSBAR;
        roNode.hasRangeValue = true;
        roNode.rangeMin = 0.0;
        roNode.rangeMax = 100.0;
        roNode.rangeValue = 40.0;
        roNode.rangeIsReadOnly = true;
        roNode.onSetRangeValue = nullptr; // Read-only: no setter callback

        TEST_ASSERT(readOnlyTree.ReplaceNodes({ roNode }));
        ComPtr<IRawElementProviderSimple> pRoProv(readOnlyTree.GetProviderForNode(301));
        ComPtr<IRangeValueProvider> pRoRange;
        TEST_ASSERT_EQ(pRoProv->QueryInterface(__uuidof(IRangeValueProvider), reinterpret_cast<void**>(&pRoRange)), S_OK);

        BOOL roFlag = FALSE;
        TEST_ASSERT_EQ(pRoRange->get_IsReadOnly(&roFlag), S_OK);
        TEST_ASSERT_EQ(roFlag, TRUE);

        // SetValue on read-only control without handler returns UIA_E_NOTSUPPORTED
        TEST_ASSERT_EQ(pRoRange->SetValue(50.0), UIA_E_NOTSUPPORTED);
    }
}

// ============================================================================
// 5. Live-Region Metadata and Deduplication Seam
// ============================================================================

TEST_CASE(Accessibility, LiveRegionMetadataAndDeduplication) {
    AccessibilityTree tree;

    // 1. Invalid child ID <= 0 returns false
    TEST_ASSERT(!tree.AnnounceLiveRegion(0, L"Volume 100%"));
    TEST_ASSERT(!tree.AnnounceLiveRegion(-5, L"Volume 100%"));

    // 2. Announce on non-existent node ID auto-creates fallback status live region
    const LONG kLiveRegionId = 7000;
    TEST_ASSERT(tree.AnnounceLiveRegion(kLiveRegionId, L"Playback Started"));

    AccessibilityNode liveNode;
    TEST_ASSERT(tree.GetNodeCopy(kLiveRegionId, liveNode));
    TEST_ASSERT_EQ(liveNode.name, std::wstring(L"Playback Started"));
    TEST_ASSERT_EQ(static_cast<int>(liveNode.role), static_cast<int>(ROLE_SYSTEM_STATUSBAR));
    TEST_ASSERT_EQ(static_cast<int>(liveNode.controlTypeId), static_cast<int>(UIA_StatusBarControlTypeId));
    TEST_ASSERT_EQ(liveNode.automationId, std::wstring(L"AppStatusLiveRegion"));
    TEST_ASSERT_EQ(liveNode.ariaRole, std::wstring(L"status"));
    TEST_ASSERT_EQ(static_cast<int>(liveNode.liveSetting), static_cast<int>(Polite));

    // Verify properties exposed via UIA provider
    ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(kLiveRegionId));
    TEST_ASSERT(pProv.Get() != nullptr);

    VariantScope varLive;
    TEST_ASSERT_EQ(pProv->GetPropertyValue(UIA_LiveSettingPropertyId, &varLive), S_OK);
    TEST_ASSERT_EQ(varLive.Ref().vt, VT_I4);
    TEST_ASSERT_EQ(varLive.Ref().lVal, static_cast<LONG>(Polite));

    VariantScope varAria;
    TEST_ASSERT_EQ(pProv->GetPropertyValue(UIA_AriaRolePropertyId, &varAria), S_OK);
    TEST_ASSERT_EQ(varAria.Ref().vt, VT_BSTR);
    TEST_ASSERT_EQ(std::wstring(varAria.Ref().bstrVal), std::wstring(L"status"));

    // 3. Deduplication Seam: Repeating the exact same announcement returns true immediately
    TEST_ASSERT(tree.AnnounceLiveRegion(kLiveRegionId, L"Playback Started"));

    // 4. New text updates the node
    TEST_ASSERT(tree.AnnounceLiveRegion(kLiveRegionId, L"Playback Paused"));
    TEST_ASSERT(tree.GetNodeCopy(kLiveRegionId, liveNode));
    TEST_ASSERT_EQ(liveNode.name, std::wstring(L"Playback Paused"));
}

// ============================================================================
// 6. Transient Modal Isolation and Focus Restoration
// ============================================================================

TEST_CASE(Accessibility, TransientModalIsolationAndFocusRestore) {
    AccessibilityTree tree;

    // Main UI controls
    std::vector<AccessibilityNode> mainNodes;
    {
        AccessibilityNode n;
        n.id = 10;
        n.name = L"Play";
        n.role = ROLE_SYSTEM_PUSHBUTTON;
        n.focused = true;
        mainNodes.push_back(n);
    }
    {
        AccessibilityNode n;
        n.id = 20;
        n.name = L"Pause";
        n.role = ROLE_SYSTEM_PUSHBUTTON;
        mainNodes.push_back(n);
    }
    {
        AccessibilityNode n;
        n.id = 30;
        n.name = L"Timeline";
        n.role = ROLE_SYSTEM_SLIDER;
        mainNodes.push_back(n);
    }
    {
        AccessibilityNode n;
        n.id = 40;
        n.name = L"Playlist";
        n.role = ROLE_SYSTEM_LIST;
        mainNodes.push_back(n);
    }

    TEST_ASSERT(tree.ReplaceNodes(mainNodes));
    TEST_ASSERT(tree.SetFocusedNode(10));

    // Verify main UI focus
    ComPtr<IRawElementProviderFragmentRoot> pRoot;
    TEST_ASSERT_EQ(tree.QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(&pRoot)), S_OK);
    {
        ComPtr<IRawElementProviderFragment> pFocused;
        TEST_ASSERT_EQ(pRoot->GetFocus(&pFocused), S_OK);
        TEST_ASSERT(pFocused.Get() != nullptr);
    }

    // Keep reference to a background control provider before modal transition
    ComPtr<IRawElementProviderSimple> pOldPlayProv(tree.GetProviderForNode(10));
    TEST_ASSERT(pOldPlayProv.Get() != nullptr);

    // Modal Presentation: Replace tree entirely with modal nodes
    std::vector<AccessibilityNode> modalNodes;
    {
        AccessibilityNode n;
        n.id = 8050;
        n.name = L"Confirm Delete Dialog";
        n.role = ROLE_SYSTEM_GROUPING;
        n.ariaRole = L"dialog";
        modalNodes.push_back(n);
    }
    {
        AccessibilityNode n;
        n.id = 8001;
        n.name = L"Delete";
        n.role = ROLE_SYSTEM_PUSHBUTTON;
        n.groupId = 8050;
        modalNodes.push_back(n);
    }
    {
        AccessibilityNode n;
        n.id = 8002;
        n.name = L"Cancel";
        n.role = ROLE_SYSTEM_PUSHBUTTON;
        n.groupId = 8050;
        modalNodes.push_back(n);
    }

    TEST_ASSERT(tree.ReplaceNodes(modalNodes));
    TEST_ASSERT(tree.SetFocusedNode(8001));

    // 1. Modal Isolation checks:
    // Background controls (10, 20, 30, 40) must NOT exist in the tree
    AccessibilityNode checkNode;
    TEST_ASSERT(!tree.GetNodeCopy(10, checkNode));
    TEST_ASSERT(!tree.GetNodeCopy(20, checkNode));
    TEST_ASSERT(!tree.GetNodeCopy(30, checkNode));
    TEST_ASSERT(!tree.GetNodeCopy(40, checkNode));

    // Old background provider must return UIA_E_ELEMENTNOTAVAILABLE
    VariantScope varName;
    TEST_ASSERT_EQ(pOldPlayProv->GetPropertyValue(UIA_NamePropertyId, &varName), UIA_E_ELEMENTNOTAVAILABLE);

    // Current tree only contains the 3 modal nodes
    auto currentNodes = tree.GetNodes();
    TEST_ASSERT_EQ(currentNodes.size(), 3);

    // Current focused element is the modal OK button (8001)
    {
        ComPtr<IRawElementProviderFragment> pFocused;
        TEST_ASSERT_EQ(pRoot->GetFocus(&pFocused), S_OK);
        TEST_ASSERT(pFocused.Get() != nullptr);
    }

    // Modal Dismissal: Restore main UI nodes and focus back to Play button
    TEST_ASSERT(tree.ReplaceNodes(mainNodes));
    TEST_ASSERT(tree.SetFocusedNode(10));

    // 2. Focus Restoration checks:
    TEST_ASSERT(tree.GetNodeCopy(10, checkNode));
    TEST_ASSERT_EQ(checkNode.focused, true);

    // Modal controls (8001, 8002, 8050) are no longer present
    TEST_ASSERT(!tree.GetNodeCopy(8001, checkNode));
    TEST_ASSERT(!tree.GetNodeCopy(8050, checkNode));

    // Focused element is once again the Play button
    {
        ComPtr<IRawElementProviderFragment> pFocused;
        TEST_ASSERT_EQ(pRoot->GetFocus(&pFocused), S_OK);
        TEST_ASSERT(pFocused.Get() != nullptr);
    }
}

// ============================================================================
// 7. Hierarchy, Navigation, RuntimeId, and Hit-Testing
// ============================================================================

TEST_CASE(Accessibility, HierarchyAndNavigation) {
    AccessibilityTree tree;

    std::vector<AccessibilityNode> nodes;
    for (LONG id = 1; id <= 4; ++id) {
        AccessibilityNode n;
        n.id = id;
        n.name = L"Node " + std::to_wstring(id);
        n.bounds = { (id - 1) * 50, 0, id * 50, 40 };
        nodes.push_back(n);
    }

    TEST_ASSERT(tree.ReplaceNodes(nodes));

    ComPtr<IRawElementProviderFragment> pRootFrag;
    TEST_ASSERT_EQ(tree.QueryInterface(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(&pRootFrag)), S_OK);

    // Root Navigation
    ComPtr<IRawElementProviderFragment> pFirstChild;
    TEST_ASSERT_EQ(pRootFrag->Navigate(NavigateDirection_FirstChild, &pFirstChild), S_OK);
    TEST_ASSERT(pFirstChild.Get() != nullptr);

    ComPtr<IRawElementProviderFragment> pLastChild;
    TEST_ASSERT_EQ(pRootFrag->Navigate(NavigateDirection_LastChild, &pLastChild), S_OK);
    TEST_ASSERT(pLastChild.Get() != nullptr);

    // Root has no parent
    ComPtr<IRawElementProviderFragment> pRootParent;
    TEST_ASSERT_EQ(pRootFrag->Navigate(NavigateDirection_Parent, &pRootParent), S_OK);
    TEST_ASSERT(pRootParent.Get() == nullptr);

    // Child Navigation (Node 1)
    ComPtr<IRawElementProviderFragment> pParent;
    TEST_ASSERT_EQ(pFirstChild->Navigate(NavigateDirection_Parent, &pParent), S_OK);
    TEST_ASSERT(pParent.Get() != nullptr);

    // Node 1 previous sibling is null
    ComPtr<IRawElementProviderFragment> pPrev;
    TEST_ASSERT_EQ(pFirstChild->Navigate(NavigateDirection_PreviousSibling, &pPrev), S_OK);
    TEST_ASSERT(pPrev.Get() == nullptr);

    // Node 1 next sibling is Node 2
    ComPtr<IRawElementProviderFragment> pNext;
    TEST_ASSERT_EQ(pFirstChild->Navigate(NavigateDirection_NextSibling, &pNext), S_OK);
    TEST_ASSERT(pNext.Get() != nullptr);

    // Node 4 next sibling is null
    ComPtr<IRawElementProviderFragment> pLastNext;
    TEST_ASSERT_EQ(pLastChild->Navigate(NavigateDirection_NextSibling, &pLastNext), S_OK);
    TEST_ASSERT(pLastNext.Get() == nullptr);

    // Runtime ID verification: Child must return [UiaAppendRuntimeId, id]
    SafeArrayScope saRuntimeId;
    TEST_ASSERT_EQ(pFirstChild->GetRuntimeId(&saRuntimeId), S_OK);
    TEST_ASSERT(saRuntimeId.Get() != nullptr);

    int* pIds = nullptr;
    TEST_ASSERT_EQ(SafeArrayAccessData(saRuntimeId.Get(), reinterpret_cast<void**>(&pIds)), S_OK);
    TEST_ASSERT_EQ(pIds[0], UiaAppendRuntimeId);
    TEST_ASSERT_EQ(pIds[1], 1);
    SafeArrayUnaccessData(saRuntimeId.Get());

    // Bounding Rectangle
    UiaRect bounds{};
    TEST_ASSERT_EQ(pFirstChild->get_BoundingRectangle(&bounds), S_OK);
    TEST_ASSERT_EQ(bounds.left, 0.0);
    TEST_ASSERT_EQ(bounds.top, 0.0);
    TEST_ASSERT_EQ(bounds.width, 50.0);
    TEST_ASSERT_EQ(bounds.height, 40.0);

    // Hit Testing via FragmentRoot::ElementProviderFromPoint
    ComPtr<IRawElementProviderFragmentRoot> pRootFragRoot;
    TEST_ASSERT_EQ(tree.QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(&pRootFragRoot)), S_OK);

    // Point (25, 20) is inside Node 1 ([0, 0, 50, 40])
    ComPtr<IRawElementProviderFragment> pHit1;
    TEST_ASSERT_EQ(pRootFragRoot->ElementProviderFromPoint(25.0, 20.0, &pHit1), S_OK);
    TEST_ASSERT(pHit1.Get() != nullptr);

    // Point (75, 20) is inside Node 2 ([50, 0, 100, 40])
    ComPtr<IRawElementProviderFragment> pHit2;
    TEST_ASSERT_EQ(pRootFragRoot->ElementProviderFromPoint(75.0, 20.0, &pHit2), S_OK);
    TEST_ASSERT(pHit2.Get() != nullptr);

    // Point (500, 500) is outside all nodes
    ComPtr<IRawElementProviderFragment> pHitOut;
    TEST_ASSERT_EQ(pRootFragRoot->ElementProviderFromPoint(500.0, 500.0, &pHitOut), S_OK);
    TEST_ASSERT(pHitOut.Get() == nullptr);
}

// ============================================================================
// 8. Node Validation, Invalid Arguments, and Disabled State Bounds
// ============================================================================

TEST_CASE(Accessibility, NodeValidationAndErrorHandling) {
    AccessibilityTree tree;

    // 1. Tree Validation: Node ID <= 0 rejected
    {
        AccessibilityNode invalidNode;
        invalidNode.id = 0;
        TEST_ASSERT(!tree.ReplaceNodes({ invalidNode }));

        invalidNode.id = -10;
        TEST_ASSERT(!tree.ReplaceNodes({ invalidNode }));
    }

    // 2. Tree Validation: Duplicate Node IDs rejected
    {
        AccessibilityNode n1; n1.id = 50;
        AccessibilityNode n2; n2.id = 50;
        TEST_ASSERT(!tree.ReplaceNodes({ n1, n2 }));
    }

    // 3. Null pointer out-parameter assertions (E_POINTER)
    {
        AccessibilityNode validNode;
        validNode.id = 1;
        validNode.hasInvoke = true;
        validNode.hasToggle = true;
        validNode.hasValue = true;
        validNode.hasRangeValue = true;
        validNode.hasSelection = true;
        validNode.hasSelectionItem = true;
        validNode.hasScroll = true;
        validNode.verticallyScrollable = true;
        TEST_ASSERT(tree.ReplaceNodes({ validNode }));

        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(1));
        TEST_ASSERT(pProv.Get() != nullptr);

        TEST_ASSERT_EQ(pProv->get_ProviderOptions(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_InvokePatternId, nullptr), E_POINTER);
        TEST_ASSERT_EQ(pProv->GetPropertyValue(UIA_NamePropertyId, nullptr), E_POINTER);

        ComPtr<IRawElementProviderFragment> pFrag;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(&pFrag)), S_OK);
        TEST_ASSERT_EQ(pFrag->Navigate(NavigateDirection_Parent, nullptr), E_POINTER);
        TEST_ASSERT_EQ(pFrag->GetRuntimeId(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pFrag->get_BoundingRectangle(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pFrag->get_FragmentRoot(nullptr), E_POINTER);

        ComPtr<IToggleProvider> pToggle;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IToggleProvider), reinterpret_cast<void**>(&pToggle)), S_OK);
        TEST_ASSERT_EQ(pToggle->get_ToggleState(nullptr), E_POINTER);

        ComPtr<IValueProvider> pValue;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IValueProvider), reinterpret_cast<void**>(&pValue)), S_OK);
        TEST_ASSERT_EQ(pValue->SetValue(nullptr), E_INVALIDARG);
        TEST_ASSERT_EQ(pValue->get_Value(static_cast<BSTR*>(nullptr)), E_POINTER);

        ComPtr<IRangeValueProvider> pRange;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IRangeValueProvider), reinterpret_cast<void**>(&pRange)), S_OK);
        TEST_ASSERT_EQ(pRange->get_Value(static_cast<double*>(nullptr)), E_POINTER);
        TEST_ASSERT_EQ(pRange->get_Minimum(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pRange->get_Maximum(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pRange->get_SmallChange(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pRange->get_LargeChange(nullptr), E_POINTER);

        ComPtr<ISelectionProvider> pSelection;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(ISelectionProvider), reinterpret_cast<void**>(&pSelection)), S_OK);
        TEST_ASSERT_EQ(pSelection->GetSelection(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pSelection->get_CanSelectMultiple(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pSelection->get_IsSelectionRequired(nullptr), E_POINTER);

        ComPtr<ISelectionItemProvider> pSelItem;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(ISelectionItemProvider), reinterpret_cast<void**>(&pSelItem)), S_OK);
        TEST_ASSERT_EQ(pSelItem->get_IsSelected(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pSelItem->get_SelectionContainer(nullptr), E_POINTER);

        ComPtr<IScrollProvider> pScroll;
        TEST_ASSERT_EQ(pProv->QueryInterface(__uuidof(IScrollProvider), reinterpret_cast<void**>(&pScroll)), S_OK);
        TEST_ASSERT_EQ(pScroll->get_HorizontalScrollPercent(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pScroll->get_VerticalScrollPercent(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pScroll->get_HorizontalViewSize(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pScroll->get_VerticalViewSize(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pScroll->get_HorizontallyScrollable(nullptr), E_POINTER);
        TEST_ASSERT_EQ(pScroll->get_VerticallyScrollable(nullptr), E_POINTER);
    }

    // 4. Disabled Controls: STATE_SYSTEM_UNAVAILABLE returns UIA_E_ELEMENTNOTENABLED
    {
        AccessibilityNode disabledNode;
        disabledNode.id = 500;
        disabledNode.name = L"Disabled Button";
        disabledNode.role = ROLE_SYSTEM_PUSHBUTTON;
        disabledNode.state = STATE_SYSTEM_UNAVAILABLE;
        disabledNode.hasInvoke = true;
        disabledNode.onInvoke = [](LONG) {};

        AccessibilityNode disabledToggle;
        disabledToggle.id = 501;
        disabledToggle.name = L"Disabled Check";
        disabledToggle.role = ROLE_SYSTEM_CHECKBUTTON;
        disabledToggle.state = STATE_SYSTEM_UNAVAILABLE;
        disabledToggle.hasToggle = true;

        TEST_ASSERT(tree.ReplaceNodes({ disabledNode, disabledToggle }));

        ComPtr<IRawElementProviderSimple> pInvProv(tree.GetProviderForNode(500));
        ComPtr<IInvokeProvider> pInvoke;
        TEST_ASSERT_EQ(pInvProv->QueryInterface(__uuidof(IInvokeProvider), reinterpret_cast<void**>(&pInvoke)), S_OK);
        TEST_ASSERT_EQ(pInvoke->Invoke(), UIA_E_ELEMENTNOTENABLED);

        ComPtr<IRawElementProviderSimple> pTogProv(tree.GetProviderForNode(501));
        ComPtr<IToggleProvider> pToggle;
        TEST_ASSERT_EQ(pTogProv->QueryInterface(__uuidof(IToggleProvider), reinterpret_cast<void**>(&pToggle)), S_OK);
        TEST_ASSERT_EQ(pToggle->Toggle(), UIA_E_ELEMENTNOTENABLED);
    }

    // 5. Removed node returns UIA_E_ELEMENTNOTAVAILABLE on subsequent method calls
    {
        AccessibilityNode n;
        n.id = 600;
        n.name = L"Ephemeral Node";
        n.hasInvoke = true;
        TEST_ASSERT(tree.ReplaceNodes({ n }));

        ComPtr<IRawElementProviderSimple> pProv(tree.GetProviderForNode(600));
        TEST_ASSERT(pProv.Get() != nullptr);

        // Remove node from tree
        TEST_ASSERT(tree.RemoveNode(600));

        VariantScope varVal;
        TEST_ASSERT_EQ(pProv->GetPropertyValue(UIA_NamePropertyId, &varVal), UIA_E_ELEMENTNOTAVAILABLE);
    }
}

// ============================================================================
// 9. MSAA / IAccessible Contract Verification
// ============================================================================

TEST_CASE(Accessibility, MsaaIAccessibleContract) {
    AccessibilityTree tree;

    bool defaultActionTriggered = false;

    AccessibilityNode n1;
    n1.id = 1;
    n1.name = L"Play Media";
    n1.role = ROLE_SYSTEM_PUSHBUTTON;
    n1.state = STATE_SYSTEM_FOCUSABLE;
    n1.value = L"";
    n1.defaultAction = L"Press";
    n1.keyboardShortcut = L"Space";
    n1.bounds = { 10, 20, 110, 60 };
    n1.onDefaultAction = [&](LONG) { defaultActionTriggered = true; };

    AccessibilityNode n2;
    n2.id = 2;
    n2.name = L"Volume Slider";
    n2.role = ROLE_SYSTEM_SLIDER;
    n2.state = STATE_SYSTEM_FOCUSABLE;
    n2.value = L"80%";
    n2.bounds = { 120, 20, 220, 60 };

    TEST_ASSERT(tree.ReplaceNodes({ n1, n2 }));

    ComPtr<IAccessible> pAcc;
    TEST_ASSERT_EQ(tree.QueryInterface(IID_IAccessible, reinterpret_cast<void**>(&pAcc)), S_OK);
    TEST_ASSERT(pAcc.Get() != nullptr);

    // Child Count
    long childCount = 0;
    TEST_ASSERT_EQ(pAcc->get_accChildCount(&childCount), S_OK);
    TEST_ASSERT_EQ(childCount, 2L);

    // Name Property for Child 1
    {
        VARIANT varChild;
        VariantInit(&varChild);
        varChild.vt = VT_I4;
        varChild.lVal = 1;

        BstrScope bstrName;
        TEST_ASSERT_EQ(pAcc->get_accName(varChild, &bstrName), S_OK);
        TEST_ASSERT_EQ(bstrName.ToWString(), std::wstring(L"Play Media"));

        VariantScope varRole;
        TEST_ASSERT_EQ(pAcc->get_accRole(varChild, &varRole), S_OK);
        TEST_ASSERT_EQ(varRole.Ref().vt, VT_I4);
        TEST_ASSERT_EQ(varRole.Ref().lVal, static_cast<LONG>(ROLE_SYSTEM_PUSHBUTTON));

        BstrScope bstrAction;
        TEST_ASSERT_EQ(pAcc->get_accDefaultAction(varChild, &bstrAction), S_OK);
        TEST_ASSERT_EQ(bstrAction.ToWString(), std::wstring(L"Press"));

        BstrScope bstrShortcut;
        TEST_ASSERT_EQ(pAcc->get_accKeyboardShortcut(varChild, &bstrShortcut), S_OK);
        TEST_ASSERT_EQ(bstrShortcut.ToWString(), std::wstring(L"Space"));

        // Location query
        long left = 0, top = 0, width = 0, height = 0;
        TEST_ASSERT_EQ(pAcc->accLocation(&left, &top, &width, &height, varChild), S_OK);
        TEST_ASSERT_EQ(left, 10L);
        TEST_ASSERT_EQ(top, 20L);
        TEST_ASSERT_EQ(width, 100L);
        TEST_ASSERT_EQ(height, 40L);

        // Do Default Action
        TEST_ASSERT_EQ(pAcc->accDoDefaultAction(varChild), S_OK);
        TEST_ASSERT_EQ(defaultActionTriggered, true);
    }

    // Value Property for Child 2 (Slider)
    {
        VARIANT varChild;
        VariantInit(&varChild);
        varChild.vt = VT_I4;
        varChild.lVal = 2;

        BstrScope bstrValue;
        TEST_ASSERT_EQ(pAcc->get_accValue(varChild, &bstrValue), S_OK);
        TEST_ASSERT_EQ(bstrValue.ToWString(), std::wstring(L"80%"));
    }

    // Hit Testing via MSAA accHitTest
    {
        VariantScope varHit;
        TEST_ASSERT_EQ(pAcc->accHitTest(50, 40, &varHit), S_OK);
        TEST_ASSERT_EQ(varHit.Ref().vt, VT_I4);
        TEST_ASSERT_EQ(varHit.Ref().lVal, 1L); // Node 1
    }

    // Invalid Child ID handling
    {
        VARIANT varInvalid;
        VariantInit(&varInvalid);
        varInvalid.vt = VT_I4;
        varInvalid.lVal = 9999;

        BstrScope bstrName;
        TEST_ASSERT_NEQ(pAcc->get_accName(varInvalid, &bstrName), S_OK);
    }
}

} // namespace Aperture::Test
