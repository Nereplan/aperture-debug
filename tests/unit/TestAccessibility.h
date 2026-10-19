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

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(T* p) : ptr_(p) {}
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr& other) : ptr_(other.ptr_) {
        if (ptr_) ptr_->AddRef();
    }
    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            if (ptr_) ptr_->AddRef();
        }
        return *this;
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
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
private:
    T* ptr_ = nullptr;
};

class SafeArrayScope {
public:
    explicit SafeArrayScope(SAFEARRAY* sa = nullptr) : sa_(sa) {}
    ~SafeArrayScope() { Reset(); }
    SAFEARRAY* Get() const { return sa_; }
    SAFEARRAY** operator&() { Reset(); return &sa_; }
    void Reset(SAFEARRAY* sa = nullptr) {
        if (sa_) {
            SafeArrayDestroy(sa_);
            sa_ = nullptr;
        }
        sa_ = sa;
    }
    SAFEARRAY* sa_ = nullptr;
};

class BstrScope {
public:
    explicit BstrScope(BSTR bstr = nullptr) : bstr_(bstr) {}
    ~BstrScope() { Reset(); }
    BSTR* operator&() { Reset(); return &bstr_; }
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

class VariantScope {
public:
    VariantScope() { VariantInit(&var_); }
    ~VariantScope() { VariantClear(&var_); }
    VARIANT* operator&() { Reset(); return &var_; }
    const VARIANT& Ref() const { return var_; }
    void Reset() { VariantClear(&var_); VariantInit(&var_); }
private:
    VARIANT var_{};
};

} // namespace

TEST_CASE(Accessibility, ProviderAndHierarchyContract) {
    AccessibilityTree tree;
    bool defaultActionTriggered = false;
    double callbackRangeVal = 0.0;

    std::vector<AccessibilityNode> nodes;
    {
        AccessibilityNode n;
        n.id = 1;
        n.name = L"Play";
        n.role = ROLE_SYSTEM_PUSHBUTTON;
        n.state = STATE_SYSTEM_FOCUSABLE;
        n.defaultAction = L"Press";
        n.keyboardShortcut = L"Space";
        n.bounds = { 10, 20, 110, 60 };
        n.hasInvoke = true;
        n.onInvoke = [](LONG) {};
        n.onDefaultAction = [&](LONG) { defaultActionTriggered = true; };
        nodes.push_back(n);
    }
    {
        AccessibilityNode n;
        n.id = 2;
        n.name = L"Mute";
        n.role = ROLE_SYSTEM_CHECKBUTTON;
        n.hasToggle = true;
        n.toggleState = ToggleState_Off;
        nodes.push_back(n);
    }
    {
        AccessibilityNode n;
        n.id = 3;
        n.name = L"Timeline";
        n.role = ROLE_SYSTEM_SLIDER;
        n.value = L"25%";
        n.hasRangeValue = true;
        n.rangeMin = 0.0;
        n.rangeMax = 100.0;
        n.rangeValue = 25.0;
        n.onSetRangeValue = [&](LONG, double val) { callbackRangeVal = val; };
        nodes.push_back(n);
    }
    {
        AccessibilityNode n;
        n.id = 4;
        n.name = L"Playlist";
        n.role = ROLE_SYSTEM_LIST;
        n.hasSelection = true;
        n.canSelectMultiple = false;
        n.isSelectionRequired = true;
        nodes.push_back(n);
    }
    {
        AccessibilityNode n;
        n.id = 5;
        n.name = L"Track 1";
        n.role = ROLE_SYSTEM_LISTITEM;
        n.hasSelectionItem = true;
        n.selectionContainerId = 4;
        nodes.push_back(n);
    }

    TEST_ASSERT(tree.ReplaceNodes(nodes));
    TEST_ASSERT(tree.SetFocusedNode(1));

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

        ComPtr<IUnknown> pToggleUnk;
        TEST_ASSERT_EQ(pProv->GetPatternProvider(UIA_TogglePatternId, &pToggleUnk), S_OK);
        TEST_ASSERT(pToggleUnk.Get() == nullptr);
    }

    {
        ComPtr<IRawElementProviderSimple> pSlider(tree.GetProviderForNode(3));
        ComPtr<IRangeValueProvider> pRange;
        TEST_ASSERT_EQ(pSlider->QueryInterface(__uuidof(IRangeValueProvider), reinterpret_cast<void**>(&pRange)), S_OK);
        TEST_ASSERT_EQ(pRange->SetValue(150.0), S_OK);
        TEST_ASSERT_EQ(callbackRangeVal, 100.0);
        TEST_ASSERT_EQ(pRange->SetValue(-20.0), S_OK);
        TEST_ASSERT_EQ(callbackRangeVal, 0.0);
    }

    {
        ComPtr<IRawElementProviderSimple> pItem(tree.GetProviderForNode(5));
        ComPtr<ISelectionItemProvider> pSelItem;
        TEST_ASSERT_EQ(pItem->QueryInterface(__uuidof(ISelectionItemProvider), reinterpret_cast<void**>(&pSelItem)), S_OK);
        TEST_ASSERT_EQ(pSelItem->Select(), S_OK);
        BOOL isSel = FALSE;
        TEST_ASSERT_EQ(pSelItem->get_IsSelected(&isSel), S_OK);
        TEST_ASSERT_EQ(isSel, TRUE);
    }

    {
        ComPtr<IAccessible> pAcc;
        TEST_ASSERT_EQ(tree.QueryInterface(IID_IAccessible, reinterpret_cast<void**>(&pAcc)), S_OK);
        long childCount = 0;
        TEST_ASSERT_EQ(pAcc->get_accChildCount(&childCount), S_OK);
        TEST_ASSERT_EQ(childCount, 5L);

        VARIANT varChild{};
        varChild.vt = VT_I4;
        varChild.lVal = 1;

        BstrScope bstrName;
        TEST_ASSERT_EQ(pAcc->get_accName(varChild, &bstrName), S_OK);
        TEST_ASSERT_EQ(bstrName.ToWString(), std::wstring(L"Play"));
        TEST_ASSERT_EQ(pAcc->accDoDefaultAction(varChild), S_OK);
        TEST_ASSERT(defaultActionTriggered);

        VariantScope varHit;
        TEST_ASSERT_EQ(pAcc->accHitTest(50, 40, &varHit), S_OK);
        TEST_ASSERT_EQ(varHit.Ref().vt, VT_I4);
        TEST_ASSERT_EQ(varHit.Ref().lVal, 1L);
    }

    {
        ComPtr<IRawElementProviderFragmentRoot> pRoot;
        TEST_ASSERT_EQ(tree.QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(&pRoot)), S_OK);
        ComPtr<IRawElementProviderSimple> pOldPlayProv(tree.GetProviderForNode(1));

        AccessibilityNode modalBtn;
        modalBtn.id = 8001;
        modalBtn.name = L"Confirm";
        modalBtn.role = ROLE_SYSTEM_PUSHBUTTON;
        TEST_ASSERT(tree.ReplaceNodes({ modalBtn }));
        TEST_ASSERT(tree.SetFocusedNode(8001));

        AccessibilityNode checkNode;
        TEST_ASSERT(!tree.GetNodeCopy(1, checkNode));
        VariantScope varName;
        TEST_ASSERT_EQ(pOldPlayProv->GetPropertyValue(UIA_NamePropertyId, &varName), UIA_E_ELEMENTNOTAVAILABLE);

        TEST_ASSERT(tree.ReplaceNodes(nodes));
        TEST_ASSERT(tree.SetFocusedNode(1));
        TEST_ASSERT(tree.GetNodeCopy(1, checkNode));
        TEST_ASSERT(checkNode.focused);
        TEST_ASSERT(!tree.GetNodeCopy(8001, checkNode));

        AccessibilityNode duplicate = nodes[0];
        TEST_ASSERT(!tree.ReplaceNodes({ duplicate, duplicate }));
        duplicate.id = 0;
        TEST_ASSERT(!tree.ReplaceNodes({ duplicate }));
        TEST_ASSERT(tree.GetNodeCopy(1, checkNode));
        TEST_ASSERT_EQ(checkNode.name, std::wstring(L"Play"));
    }
}

} // namespace Aperture::Test
