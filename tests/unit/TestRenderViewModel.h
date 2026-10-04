#pragma once

#include "../support/TestFramework.h"
#include "../../src/RenderViewModel.h"

namespace Aperture::Test {

TEST_CASE(RenderViewModel, FlyoutBorrowsLargeQueueForTheFrame) {
    SettingsFlyoutViewModel vm;
    TEST_ASSERT(vm.QueueItems().empty());
    std::vector<std::wstring> queue(25000, L"C:\\Media\\episode.mkv");
    vm.pQueueItems = &queue;
    SettingsFlyoutViewModel nextFrame = vm;
    TEST_ASSERT(nextFrame.QueueItems().data() == queue.data());
    TEST_ASSERT_EQ(nextFrame.QueueItems().size(), size_t(25000));
}

TEST_CASE(RenderViewModel, SettingsTabSystemActionMatching) {
    SettingsTabViewModel vm;
    TEST_ASSERT(!vm.pressState.IsSystemActionPressed(2));
    vm.pressState = { SettingsPressState::Target::SystemAction, 2, -1 };
    TEST_ASSERT(vm.pressState.IsSystemActionPressed(2));
    TEST_ASSERT(!vm.pressState.IsSystemActionPressed(0));
    vm.pressState = { SettingsPressState::Target::OptionRow, 2, -1 };
    TEST_ASSERT(!vm.pressState.IsSystemActionPressed(2));
}

} // namespace Aperture::Test
