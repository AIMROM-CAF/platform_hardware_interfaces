/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <VtsHalHidlTargetTestBase.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android/hardware/automotive/can/1.0/ICanBus.h>
#include <android/hardware/automotive/can/1.0/ICanController.h>
#include <android/hardware/automotive/can/1.0/types.h>
#include <android/hidl/manager/1.2/IServiceManager.h>
#include <can-vts-utils/can-hal-printers.h>
#include <can-vts-utils/environment-utils.h>
#include <gmock/gmock.h>
#include <hidl-utils/hidl-utils.h>

namespace android {
namespace hardware {
namespace automotive {
namespace can {
namespace V1_0 {
namespace vts {

using hardware::hidl_vec;
using InterfaceType = ICanController::InterfaceType;

static utils::SimpleHidlEnvironment<ICanController>* gEnv = nullptr;

class CanControllerHalTest : public ::testing::VtsHalHidlTargetTestBase {
  protected:
    virtual void SetUp() override;
    virtual void TearDown() override;

    hidl_vec<InterfaceType> getSupportedInterfaceTypes();
    bool isSupported(InterfaceType iftype);

    bool up(InterfaceType iftype, const std::string srvname, std::string ifname,
            ICanController::Result expected);
    void assertRegistered(const std::string srvname, bool expectRegistered);

    sp<ICanController> mCanController;
};

void CanControllerHalTest::SetUp() {
    const auto serviceName = gEnv->getServiceName<ICanController>();
    mCanController = getService<ICanController>(serviceName);
    ASSERT_TRUE(mCanController) << "Couldn't open CAN Controller: " << serviceName;
}

void CanControllerHalTest::TearDown() {
    mCanController.clear();
}

hidl_vec<InterfaceType> CanControllerHalTest::getSupportedInterfaceTypes() {
    hidl_vec<InterfaceType> iftypesResult;
    mCanController->getSupportedInterfaceTypes(hidl_utils::fill(&iftypesResult)).assertOk();
    return iftypesResult;
}

bool CanControllerHalTest::isSupported(InterfaceType iftype) {
    const auto supported = getSupportedInterfaceTypes();
    return std::find(supported.begin(), supported.end(), iftype) != supported.end();
}

bool CanControllerHalTest::up(InterfaceType iftype, std::string srvname, std::string ifname,
                              ICanController::Result expected) {
    ICanController::BusConfiguration config = {};
    config.name = srvname;
    config.iftype = iftype;
    config.interfaceId.address(ifname);

    const auto upresult = mCanController->upInterface(config);

    if (!isSupported(iftype)) {
        LOG(INFO) << iftype << " interfaces not supported";
        EXPECT_EQ(ICanController::Result::NOT_SUPPORTED, upresult);
        return false;
    }

    EXPECT_EQ(expected, upresult);
    return true;
}

void CanControllerHalTest::assertRegistered(std::string srvname, bool expectRegistered) {
    /* Not using ICanBus::tryGetService here, since it ignores interfaces not in the manifest
     * file -- this is a test, so we don't want to add dummy services to a device manifest. */
    auto manager = hidl::manager::V1_2::IServiceManager::getService();
    auto busService = manager->get(ICanBus::descriptor, srvname);
    ASSERT_EQ(expectRegistered, busService.withDefault(nullptr) != nullptr)
            << "ICanBus/" << srvname << (expectRegistered ? " is not " : " is ") << "registered"
            << " (should be otherwise)";
}

TEST_F(CanControllerHalTest, SupportsSomething) {
    const auto supported = getSupportedInterfaceTypes();
    ASSERT_GT(supported.size(), 0u);
}

TEST_F(CanControllerHalTest, BringUpDown) {
    const std::string name = "dummy";

    assertRegistered(name, false);
    if (!up(InterfaceType::VIRTUAL, name, "vcan57", ICanController::Result::OK)) GTEST_SKIP();
    assertRegistered(name, true);

    const auto dnresult = mCanController->downInterface(name);
    ASSERT_TRUE(dnresult);

    assertRegistered(name, false);
}

TEST_F(CanControllerHalTest, DownDummy) {
    const auto result = mCanController->downInterface("imnotup");
    ASSERT_FALSE(result);
}

TEST_F(CanControllerHalTest, UpTwice) {
    const std::string name = "dummy";

    assertRegistered(name, false);
    if (!up(InterfaceType::VIRTUAL, name, "vcan72", ICanController::Result::OK)) GTEST_SKIP();
    assertRegistered(name, true);
    if (!up(InterfaceType::VIRTUAL, name, "vcan73", ICanController::Result::INVALID_STATE)) {
        GTEST_SKIP();
    }
    assertRegistered(name, true);

    const auto result = mCanController->downInterface(name);
    ASSERT_TRUE(result);
    assertRegistered(name, false);
}

TEST_F(CanControllerHalTest, IdentifierCompatibility) {
    using IdDisc = ICanController::BusConfiguration::InterfaceIdentifier::hidl_discriminator;
    static const std::map<InterfaceType, std::vector<IdDisc>> compatMatrix = {
            {InterfaceType::VIRTUAL, {IdDisc::address}},
            {InterfaceType::SOCKETCAN, {IdDisc::address, IdDisc::serialno}},
            {InterfaceType::SLCAN, {IdDisc::address, IdDisc::serialno}},
            {InterfaceType::INDEXED, {IdDisc::index}},
    };
    static const std::vector<IdDisc> allDisc = {IdDisc::address, IdDisc::index, IdDisc::serialno};

    for (const auto [iftype, supported] : compatMatrix) {
        for (const auto iddisc : allDisc) {
            LOG(INFO) << "Compatibility testing: " << iftype << " / " << iddisc;

            ICanController::BusConfiguration config = {};
            config.name = "compattestsrv";
            config.iftype = iftype;
            config.baudrate = 125000;

            // using random-ish addresses, which may not be valid - we can't test the success case
            if (iddisc == IdDisc::address) {
                config.interfaceId.address("can0");
            } else if (iddisc == IdDisc::index) {
                config.interfaceId.index(0);
            } else if (iddisc == IdDisc::serialno) {
                config.interfaceId.serialno({"dummy", "dummier"});
            }

            const auto upresult = mCanController->upInterface(config);

            if (!isSupported(iftype)) {
                ASSERT_EQ(ICanController::Result::NOT_SUPPORTED, upresult);
                continue;
            }
            ASSERT_NE(ICanController::Result::NOT_SUPPORTED, upresult);

            bool isSupportedDisc =
                    std::find(supported.begin(), supported.end(), iddisc) != supported.end();
            if (!isSupportedDisc) {
                ASSERT_EQ(ICanController::Result::BAD_ADDRESS, upresult);
                continue;
            }

            if (upresult == ICanController::Result::OK) {
                const auto dnresult = mCanController->downInterface(config.name);
                ASSERT_TRUE(dnresult);
                continue;
            }
        }
    }
}

TEST_F(CanControllerHalTest, FailEmptyName) {
    const std::string name = "";

    assertRegistered(name, false);
    if (!up(InterfaceType::VIRTUAL, name, "vcan57", ICanController::Result::UNKNOWN_ERROR)) {
        GTEST_SKIP();
    }
    assertRegistered(name, false);
}

TEST_F(CanControllerHalTest, FailBadName) {
    // 33 characters (name can be at most 32 characters long)
    const std::string name = "ab012345678901234567890123456789c";

    assertRegistered(name, false);
    if (!up(InterfaceType::VIRTUAL, name, "vcan57", ICanController::Result::UNKNOWN_ERROR)) {
        GTEST_SKIP();
    }
    assertRegistered(name, false);
}

TEST_F(CanControllerHalTest, FailBadVirtualAddress) {
    const std::string name = "dummy";

    assertRegistered(name, false);
    if (!up(InterfaceType::VIRTUAL, name, "", ICanController::Result::BAD_ADDRESS)) GTEST_SKIP();
    assertRegistered(name, false);
}

TEST_F(CanControllerHalTest, FailBadSocketcanAddress) {
    const std::string name = "dummy";

    assertRegistered(name, false);
    if (!up(InterfaceType::SOCKETCAN, name, "can87", ICanController::Result::BAD_ADDRESS)) {
        GTEST_SKIP();
    }
    assertRegistered(name, false);
}

}  // namespace vts
}  // namespace V1_0
}  // namespace can
}  // namespace automotive
}  // namespace hardware
}  // namespace android

/**
 * Example manual invocation:
 * adb shell /data/nativetest64/VtsHalCanControllerV1_0TargetTest/VtsHalCanControllerV1_0TargetTest\
 *     --hal_service_instance=android.hardware.automotive.can@1.0::ICanController/socketcan
 */
int main(int argc, char** argv) {
    using android::hardware::automotive::can::V1_0::ICanController;
    using android::hardware::automotive::can::V1_0::vts::gEnv;
    using android::hardware::automotive::can::V1_0::vts::utils::SimpleHidlEnvironment;
    android::base::SetDefaultTag("CanControllerVts");
    android::base::SetMinimumLogSeverity(android::base::VERBOSE);
    gEnv = new SimpleHidlEnvironment<ICanController>;
    ::testing::AddGlobalTestEnvironment(gEnv);
    ::testing::InitGoogleTest(&argc, argv);
    gEnv->init(&argc, argv);
    return RUN_ALL_TESTS();
}
