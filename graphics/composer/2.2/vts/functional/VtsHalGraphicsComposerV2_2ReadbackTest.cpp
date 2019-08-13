/*
 * Copyright 2018 The Android Open Source Project
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

#define LOG_TAG "graphics_composer_hidl_hal_readback_tests@2.2"

#include <VtsHalHidlTargetTestBase.h>
#include <VtsHalHidlTargetTestEnvBase.h>
#include <composer-command-buffer/2.2/ComposerCommandBuffer.h>
#include <composer-vts/2.1/GraphicsComposerCallback.h>
#include <composer-vts/2.1/TestCommandReader.h>
#include <composer-vts/2.2/ReadbackVts.h>

namespace android {
namespace hardware {
namespace graphics {
namespace composer {
namespace V2_2 {
namespace vts {
namespace {

using android::hardware::hidl_handle;
using common::V1_1::BufferUsage;
using common::V1_1::Dataspace;
using common::V1_1::PixelFormat;
using mapper::V2_1::IMapper;
using V2_1::Display;
using V2_1::Layer;
using V2_1::vts::AccessRegion;
using V2_1::vts::TestCommandReader;

// Test environment for graphics.composer
class GraphicsComposerHidlEnvironment : public ::testing::VtsHalHidlTargetTestEnvBase {
   public:
    // get the test environment singleton
    static GraphicsComposerHidlEnvironment* Instance() {
        static GraphicsComposerHidlEnvironment* instance = new GraphicsComposerHidlEnvironment;
        return instance;
    }
    virtual void registerTestServices() override { registerTestService<IComposer>(); }

   private:
    GraphicsComposerHidlEnvironment() {}
    GTEST_DISALLOW_COPY_AND_ASSIGN_(GraphicsComposerHidlEnvironment);
};

class GraphicsComposerReadbackTest : public ::testing::VtsHalHidlTargetTestBase {
   protected:
    using PowerMode = V2_1::IComposerClient::PowerMode;
    void SetUp() override {
        VtsHalHidlTargetTestBase::SetUp();
        ASSERT_NO_FATAL_FAILURE(
            mComposer = std::make_unique<Composer>(
                GraphicsComposerHidlEnvironment::Instance()->getServiceName<IComposer>()));
        ASSERT_NO_FATAL_FAILURE(mComposerClient = mComposer->createClient());
        mComposerCallback = new V2_1::vts::GraphicsComposerCallback;
        mComposerClient->registerCallback(mComposerCallback);

        // assume the first display is primary and is never removed
        mPrimaryDisplay = waitForFirstDisplay();
        Config activeConfig;
        ASSERT_NO_FATAL_FAILURE(activeConfig = mComposerClient->getActiveConfig(mPrimaryDisplay));
        ASSERT_NO_FATAL_FAILURE(
            mDisplayWidth = mComposerClient->getDisplayAttribute(
                mPrimaryDisplay, activeConfig, IComposerClient::Attribute::WIDTH));
        ASSERT_NO_FATAL_FAILURE(
            mDisplayHeight = mComposerClient->getDisplayAttribute(
                mPrimaryDisplay, activeConfig, IComposerClient::Attribute::HEIGHT));

        // explicitly disable vsync
        ASSERT_NO_FATAL_FAILURE(mComposerClient->setVsyncEnabled(mPrimaryDisplay, false));
        mComposerCallback->setVsyncAllowed(false);

        // set up command writer/reader and gralloc
        mWriter = std::make_shared<CommandWriterBase>(1024);
        mReader = std::make_unique<TestCommandReader>();
        mGralloc = std::make_shared<Gralloc>();

        std::vector<ColorMode> colorModes = mComposerClient->getColorModes(mPrimaryDisplay);
        if (std::find(colorModes.begin(), colorModes.end(), ColorMode::SRGB) == colorModes.end()) {
            mHasReadbackBuffer = false;
            return;
        }
        mWriter->selectDisplay(mPrimaryDisplay);
        ASSERT_NO_FATAL_FAILURE(mComposerClient->setColorMode(mPrimaryDisplay, ColorMode::SRGB,
                                                              RenderIntent::COLORIMETRIC));
        mComposerClient->getRaw()->getReadbackBufferAttributes(
                mPrimaryDisplay,
                [&](const auto& tmpError, const auto& tmpPixelFormat, const auto& tmpDataspace) {
                    mHasReadbackBuffer = ReadbackHelper::readbackSupported(tmpPixelFormat,
                                                                           tmpDataspace, tmpError);
                    mPixelFormat = tmpPixelFormat;
                    mDataspace = tmpDataspace;
                });
        ASSERT_NO_FATAL_FAILURE(mComposerClient->setPowerMode(mPrimaryDisplay, PowerMode::ON));
    }

    void TearDown() override {
        ASSERT_NO_FATAL_FAILURE(mComposerClient->setPowerMode(mPrimaryDisplay, PowerMode::OFF));
        EXPECT_EQ(0, mReader->mErrors.size());
        EXPECT_EQ(0, mReader->mCompositionChanges.size());
        if (mComposerCallback != nullptr) {
            EXPECT_EQ(0, mComposerCallback->getInvalidHotplugCount());
            EXPECT_EQ(0, mComposerCallback->getInvalidRefreshCount());
            EXPECT_EQ(0, mComposerCallback->getInvalidVsyncCount());
        }
        VtsHalHidlTargetTestBase::TearDown();
    }

    void clearCommandReaderState() {
        mReader->mCompositionChanges.clear();
        mReader->mErrors.clear();
    }

    void writeLayers(const std::vector<std::shared_ptr<TestLayer>>& layers) {
        for (auto layer : layers) {
            layer->write(mWriter);
        }
        execute();
    }

    void execute() {
        ASSERT_NO_FATAL_FAILURE(mComposerClient->execute(mReader.get(), mWriter.get()));
    }

    std::unique_ptr<Composer> mComposer;
    std::shared_ptr<ComposerClient> mComposerClient;

    sp<V2_1::vts::GraphicsComposerCallback> mComposerCallback;
    // the first display and is assumed never to be removed
    Display mPrimaryDisplay;
    int32_t mDisplayWidth;
    int32_t mDisplayHeight;
    std::shared_ptr<CommandWriterBase> mWriter;
    std::unique_ptr<TestCommandReader> mReader;
    std::shared_ptr<Gralloc> mGralloc;

    bool mHasReadbackBuffer;
    PixelFormat mPixelFormat;
    Dataspace mDataspace;

    static constexpr uint32_t kClientTargetSlotCount = 64;

   private:
    Display waitForFirstDisplay() {
        while (true) {
            std::vector<Display> displays = mComposerCallback->getDisplays();
            if (displays.empty()) {
                usleep(5 * 1000);
                continue;
            }
            return displays[0];
        }
    }
};

TEST_F(GraphicsComposerReadbackTest, SingleSolidColorLayer) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    auto layer = std::make_shared<TestColorLayer>(mComposerClient, mPrimaryDisplay);
    IComposerClient::Rect coloredSquare({0, 0, mDisplayWidth, mDisplayHeight});
    layer->setColor(BLUE);
    layer->setDisplayFrame(coloredSquare);
    layer->setZOrder(10);

    std::vector<std::shared_ptr<TestLayer>> layers = {layer};

    // expected color for each pixel
    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth, coloredSquare, BLUE);

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    writeLayers(layers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    // if hwc cannot handle and asks for composition change,
    // just succeed the test
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerReadbackTest, SetLayerBuffer) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());
    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, 0, mDisplayWidth, mDisplayHeight / 4}, RED);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, mDisplayHeight / 4, mDisplayWidth, mDisplayHeight / 2},
                                   GREEN);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, mDisplayHeight / 2, mDisplayWidth, mDisplayHeight}, BLUE);

    auto layer =
        std::make_shared<TestBufferLayer>(mComposerClient, mGralloc, mPrimaryDisplay, mDisplayWidth,
                                          mDisplayHeight, PixelFormat::RGBA_8888);
    layer->setDisplayFrame({0, 0, mDisplayWidth, mDisplayHeight});
    layer->setZOrder(10);
    ASSERT_NO_FATAL_FAILURE(layer->setBuffer(expectedColors));

    std::vector<std::shared_ptr<TestLayer>> layers = {layer};

    writeLayers(layers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();

    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());

    mWriter->presentDisplay();
    execute();

    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerReadbackTest, SetLayerBufferNoEffect) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }


    auto layer = std::make_shared<TestColorLayer>(mComposerClient, mPrimaryDisplay);
    IComposerClient::Rect coloredSquare({0, 0, mDisplayWidth, mDisplayHeight});
    layer->setColor(BLUE);
    layer->setDisplayFrame(coloredSquare);
    layer->setZOrder(10);
    layer->write(mWriter);

    // This following buffer call should have no effect
    PixelFormat format = PixelFormat::RGBA_8888;
    uint64_t usage =
            static_cast<uint64_t>(BufferUsage::CPU_READ_OFTEN | BufferUsage::CPU_WRITE_OFTEN);
    const native_handle_t* bufferHandle =
            mGralloc->allocate(mDisplayWidth, mDisplayHeight, 1, format, usage);
    mWriter->setLayerBuffer(0, bufferHandle, -1);

    // expected color for each pixel
    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth, coloredSquare, BLUE);

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    mWriter->validateDisplay();
    execute();

    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerReadbackTest, ClientComposition) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, 0, mDisplayWidth, mDisplayHeight / 4}, RED);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, mDisplayHeight / 4, mDisplayWidth, mDisplayHeight / 2},
                                   GREEN);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, mDisplayHeight / 2, mDisplayWidth, mDisplayHeight}, BLUE);

    auto layer =
        std::make_shared<TestBufferLayer>(mComposerClient, mGralloc, mPrimaryDisplay, mDisplayWidth,
                                          mDisplayHeight, PixelFormat::RGBA_FP16);
    layer->setDisplayFrame({0, 0, mDisplayWidth, mDisplayHeight});
    layer->setZOrder(10);

    std::vector<std::shared_ptr<TestLayer>> layers = {layer};

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());
    writeLayers(layers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();

    if (mReader->mCompositionChanges.size() != 0) {
        ASSERT_EQ(1, mReader->mCompositionChanges.size());
        ASSERT_EQ(1, mReader->mCompositionChanges[0].second);

        ASSERT_NO_FATAL_FAILURE(
            mComposerClient->setClientTargetSlotCount(mPrimaryDisplay, kClientTargetSlotCount));

        // create client target buffer
        uint32_t clientStride;
        PixelFormat clientFormat = PixelFormat::RGBA_8888;
        uint64_t clientUsage =
                static_cast<uint64_t>(BufferUsage::CPU_READ_OFTEN | BufferUsage::CPU_WRITE_OFTEN |
                                      BufferUsage::COMPOSER_CLIENT_TARGET);
        const native_handle_t* clientBufferHandle =
                mGralloc->allocate(layer->mWidth, layer->mHeight, layer->mLayerCount, clientFormat,
                                   clientUsage, /*import*/ true, &clientStride);
        ASSERT_NE(nullptr, clientBufferHandle);

        void* clientBufData =
                mGralloc->lock(clientBufferHandle, clientUsage, layer->mAccessRegion, -1);

        ASSERT_NO_FATAL_FAILURE(ReadbackHelper::fillBuffer(layer->mWidth, layer->mHeight,
                                                           clientStride, clientBufData,
                                                           clientFormat, expectedColors));
        int clientFence = mGralloc->unlock(clientBufferHandle);
        if (clientFence != -1) {
            sync_wait(clientFence, -1);
            close(clientFence);
        }

        IComposerClient::Rect damage{0, 0, mDisplayWidth, mDisplayHeight};
        mWriter->setClientTarget(0, clientBufferHandle, clientFence, Dataspace::UNKNOWN,
                                 std::vector<IComposerClient::Rect>(1, damage));

        layer->setToClientComposition(mWriter);
        mWriter->validateDisplay();
        execute();
        ASSERT_EQ(0, mReader->mCompositionChanges.size());
    }
    ASSERT_EQ(0, mReader->mErrors.size());

    mWriter->presentDisplay();
    execute();

    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerReadbackTest, DeviceAndClientComposition) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    ASSERT_NO_FATAL_FAILURE(
        mComposerClient->setClientTargetSlotCount(mPrimaryDisplay, kClientTargetSlotCount));

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, 0, mDisplayWidth, mDisplayHeight / 2}, GREEN);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, mDisplayHeight / 2, mDisplayWidth, mDisplayHeight}, RED);

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    auto deviceLayer =
        std::make_shared<TestBufferLayer>(mComposerClient, mGralloc, mPrimaryDisplay, mDisplayWidth,
                                          mDisplayHeight / 2, PixelFormat::RGBA_8888);
    std::vector<IComposerClient::Color> deviceColors(deviceLayer->mWidth * deviceLayer->mHeight);
    ReadbackHelper::fillColorsArea(deviceColors, deviceLayer->mWidth,
                                   {0, 0, static_cast<int32_t>(deviceLayer->mWidth),
                                    static_cast<int32_t>(deviceLayer->mHeight)},
                                   GREEN);
    deviceLayer->setDisplayFrame({0, 0, static_cast<int32_t>(deviceLayer->mWidth),
                                  static_cast<int32_t>(deviceLayer->mHeight)});
    deviceLayer->setZOrder(10);
    ASSERT_NO_FATAL_FAILURE(deviceLayer->setBuffer(deviceColors));
    deviceLayer->write(mWriter);

    auto clientLayer = std::make_shared<TestBufferLayer>(
        mComposerClient, mGralloc, mPrimaryDisplay, mDisplayWidth, mDisplayHeight / 2,
        PixelFormat::RGBA_8888, IComposerClient::Composition::CLIENT);
    IComposerClient::Rect clientFrame = {0, mDisplayHeight / 2, mDisplayWidth, mDisplayHeight};
    clientLayer->setDisplayFrame(clientFrame);
    clientLayer->setZOrder(0);
    clientLayer->write(mWriter);
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    uint64_t clientUsage =
            static_cast<uint64_t>(BufferUsage::CPU_READ_OFTEN | BufferUsage::CPU_WRITE_OFTEN |
                                  BufferUsage::COMPOSER_CLIENT_TARGET);
    uint32_t clientStride;
    const native_handle_t* clientBufferHandle =
            mGralloc->allocate(mDisplayWidth, mDisplayHeight, 1, PixelFormat::RGBA_8888,
                               clientUsage, /*import*/ true, &clientStride);
    ASSERT_NE(nullptr, clientBufferHandle);

    AccessRegion clientAccessRegion;
    clientAccessRegion.left = 0;
    clientAccessRegion.top = 0;
    clientAccessRegion.width = mDisplayWidth;
    clientAccessRegion.height = mDisplayHeight;
    void* clientData = mGralloc->lock(clientBufferHandle, clientUsage, clientAccessRegion, -1);
    std::vector<IComposerClient::Color> clientColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(clientColors, mDisplayWidth, clientFrame, RED);
    ASSERT_NO_FATAL_FAILURE(ReadbackHelper::fillBuffer(mDisplayWidth, mDisplayHeight, clientStride,
                                                       clientData, PixelFormat::RGBA_8888,
                                                       clientColors));
    int clientFence = mGralloc->unlock(clientBufferHandle);
    if (clientFence != -1) {
        sync_wait(clientFence, -1);
        close(clientFence);
    }

    mWriter->setClientTarget(0, clientBufferHandle, clientFence, Dataspace::UNKNOWN,
                             std::vector<IComposerClient::Rect>(1, clientFrame));
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerReadbackTest, SetLayerDamage) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelformat/dataspace";
        return;
    }

    IComposerClient::Rect redRect = {0, 0, mDisplayWidth / 4, mDisplayHeight / 4};

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth, redRect, RED);

    auto layer =
        std::make_shared<TestBufferLayer>(mComposerClient, mGralloc, mPrimaryDisplay, mDisplayWidth,
                                          mDisplayHeight, PixelFormat::RGBA_8888);
    layer->setDisplayFrame({0, 0, mDisplayWidth, mDisplayHeight});
    layer->setZOrder(10);
    ASSERT_NO_FATAL_FAILURE(layer->setBuffer(expectedColors));

    std::vector<std::shared_ptr<TestLayer>> layers = {layer};

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    writeLayers(layers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));

    // update surface damage and recheck
    redRect = {mDisplayWidth / 4, mDisplayHeight / 4, mDisplayWidth / 2, mDisplayHeight / 2};
    ReadbackHelper::clearColors(expectedColors, mDisplayWidth, mDisplayHeight, mDisplayWidth);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth, redRect, RED);

    ASSERT_NO_FATAL_FAILURE(layer->fillBuffer(expectedColors));
    layer->setSurfaceDamage(
        std::vector<IComposerClient::Rect>(1, {0, 0, mDisplayWidth / 2, mDisplayWidth / 2}));

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    writeLayers(layers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());
    ASSERT_EQ(0, mReader->mCompositionChanges.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerReadbackTest, SetLayerPlaneAlpha) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    auto layer = std::make_shared<TestColorLayer>(mComposerClient, mPrimaryDisplay);
    layer->setColor(RED);
    layer->setDisplayFrame({0, 0, mDisplayWidth, mDisplayHeight});
    layer->setZOrder(10);
    layer->setAlpha(0);
    layer->setBlendMode(IComposerClient::BlendMode::PREMULTIPLIED);

    std::vector<std::shared_ptr<TestLayer>> layers = {layer};

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    writeLayers(layers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());

    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerReadbackTest, SetLayerSourceCrop) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, 0, mDisplayWidth, mDisplayHeight / 4}, RED);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, mDisplayHeight / 2, mDisplayWidth, mDisplayHeight}, BLUE);

    auto layer =
        std::make_shared<TestBufferLayer>(mComposerClient, mGralloc, mPrimaryDisplay, mDisplayWidth,
                                          mDisplayHeight, PixelFormat::RGBA_8888);
    layer->setDisplayFrame({0, 0, mDisplayWidth, mDisplayHeight});
    layer->setZOrder(10);
    layer->setSourceCrop({0, static_cast<float>(mDisplayHeight / 2),
                          static_cast<float>(mDisplayWidth), static_cast<float>(mDisplayHeight)});
    ASSERT_NO_FATAL_FAILURE(layer->setBuffer(expectedColors));

    std::vector<std::shared_ptr<TestLayer>> layers = {layer};

    // update expected colors to match crop
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, 0, mDisplayWidth, mDisplayHeight}, BLUE);
    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());
    writeLayers(layers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerReadbackTest, SetLayerZOrder) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    IComposerClient::Rect redRect = {0, 0, mDisplayWidth, mDisplayHeight / 2};
    IComposerClient::Rect blueRect = {0, mDisplayHeight / 4, mDisplayWidth, mDisplayHeight};
    auto redLayer = std::make_shared<TestColorLayer>(mComposerClient, mPrimaryDisplay);
    redLayer->setColor(RED);
    redLayer->setDisplayFrame(redRect);

    auto blueLayer = std::make_shared<TestColorLayer>(mComposerClient, mPrimaryDisplay);
    blueLayer->setColor(BLUE);
    blueLayer->setDisplayFrame(blueRect);
    blueLayer->setZOrder(5);

    std::vector<std::shared_ptr<TestLayer>> layers = {redLayer, blueLayer};
    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);

    // red in front of blue
    redLayer->setZOrder(10);

    // fill blue first so that red will overwrite on overlap
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth, blueRect, BLUE);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth, redRect, RED);

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    writeLayers(layers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));

    redLayer->setZOrder(1);
    ReadbackHelper::clearColors(expectedColors, mDisplayWidth, mDisplayHeight, mDisplayWidth);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth, redRect, RED);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth, blueRect, BLUE);

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    writeLayers(layers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    ASSERT_EQ(0, mReader->mCompositionChanges.size());
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

class GraphicsComposerBlendModeReadbackTest : public GraphicsComposerReadbackTest,
                                              public ::testing::WithParamInterface<float> {
   public:
    void SetUp() override {
        GraphicsComposerReadbackTest::SetUp();
        mBackgroundColor = BLACK;
        mTopLayerColor = RED;
    }

    void TearDown() override { GraphicsComposerReadbackTest::TearDown(); }

    void setBackgroundColor(IComposerClient::Color color) { mBackgroundColor = color; }

    void setTopLayerColor(IComposerClient::Color color) { mTopLayerColor = color; }

    void setUpLayers(IComposerClient::BlendMode blendMode) {
        mLayers.clear();
        std::vector<IComposerClient::Color> topLayerPixelColors(mDisplayWidth * mDisplayHeight);
        ReadbackHelper::fillColorsArea(topLayerPixelColors, mDisplayWidth,
                                       {0, 0, mDisplayWidth, mDisplayHeight}, mTopLayerColor);

        auto backgroundLayer = std::make_shared<TestColorLayer>(mComposerClient, mPrimaryDisplay);
        backgroundLayer->setDisplayFrame({0, 0, mDisplayWidth, mDisplayHeight});
        backgroundLayer->setZOrder(0);
        backgroundLayer->setColor(mBackgroundColor);

        auto layer = std::make_shared<TestBufferLayer>(mComposerClient, mGralloc, mPrimaryDisplay,
                                                       mDisplayWidth, mDisplayHeight,
                                                       PixelFormat::RGBA_8888);
        layer->setDisplayFrame({0, 0, mDisplayWidth, mDisplayHeight});
        layer->setZOrder(10);
        ASSERT_NO_FATAL_FAILURE(layer->setBuffer(topLayerPixelColors));

        layer->setBlendMode(blendMode);
        layer->setAlpha(GetParam());

        mLayers.push_back(backgroundLayer);
        mLayers.push_back(layer);
    }

    void setExpectedColors(std::vector<IComposerClient::Color>& expectedColors) {
        ASSERT_EQ(2, mLayers.size());
        ReadbackHelper::clearColors(expectedColors, mDisplayWidth, mDisplayHeight, mDisplayWidth);

        auto layer = mLayers[1];
        IComposerClient::BlendMode blendMode = layer->mBlendMode;
        float alpha = mTopLayerColor.a / 255.0 * layer->mAlpha;
        if (blendMode == IComposerClient::BlendMode::NONE) {
            for (int i = 0; i < expectedColors.size(); i++) {
                expectedColors[i].r = mTopLayerColor.r * layer->mAlpha;
                expectedColors[i].g = mTopLayerColor.g * layer->mAlpha;
                expectedColors[i].b = mTopLayerColor.b * layer->mAlpha;
                expectedColors[i].a = alpha * 255.0;
            }
        } else if (blendMode == IComposerClient::BlendMode::PREMULTIPLIED) {
            for (int i = 0; i < expectedColors.size(); i++) {
                expectedColors[i].r =
                    mTopLayerColor.r * layer->mAlpha + mBackgroundColor.r * (1.0 - alpha);
                expectedColors[i].g =
                    mTopLayerColor.g * layer->mAlpha + mBackgroundColor.g * (1.0 - alpha);
                expectedColors[i].b =
                    mTopLayerColor.b * layer->mAlpha + mBackgroundColor.b * (1.0 - alpha);
                expectedColors[i].a = alpha + mBackgroundColor.a * (1.0 - alpha);
            }
        } else if (blendMode == IComposerClient::BlendMode::COVERAGE) {
            for (int i = 0; i < expectedColors.size(); i++) {
                expectedColors[i].r = mTopLayerColor.r * alpha + mBackgroundColor.r * (1.0 - alpha);
                expectedColors[i].g = mTopLayerColor.g * alpha + mBackgroundColor.g * (1.0 - alpha);
                expectedColors[i].b = mTopLayerColor.b * alpha + mBackgroundColor.b * (1.0 - alpha);
                expectedColors[i].a = mTopLayerColor.a * alpha + mBackgroundColor.a * (1.0 - alpha);
            }
        }
    }

   protected:
    std::vector<std::shared_ptr<TestLayer>> mLayers;
    IComposerClient::Color mBackgroundColor;
    IComposerClient::Color mTopLayerColor;
};

TEST_P(GraphicsComposerBlendModeReadbackTest, None) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);

    setBackgroundColor(BLACK);
    setTopLayerColor(TRANSLUCENT_RED);
    setUpLayers(IComposerClient::BlendMode::NONE);
    setExpectedColors(expectedColors);

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());
    writeLayers(mLayers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

// TODO: bug 116865056: Readback returns (245, 0, 0) for layer plane
// alpha of .2, expected 10.2
TEST_P(GraphicsComposerBlendModeReadbackTest, DISABLED_Coverage) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);

    setBackgroundColor(BLACK);
    setTopLayerColor(TRANSLUCENT_RED);

    setUpLayers(IComposerClient::BlendMode::COVERAGE);
    setExpectedColors(expectedColors);

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());
    writeLayers(mLayers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_P(GraphicsComposerBlendModeReadbackTest, Premultiplied) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);

    setBackgroundColor(BLACK);
    setTopLayerColor(TRANSLUCENT_RED);
    setUpLayers(IComposerClient::BlendMode::PREMULTIPLIED);
    setExpectedColors(expectedColors);

    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());
    writeLayers(mLayers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

INSTANTIATE_TEST_CASE_P(BlendModeTest, GraphicsComposerBlendModeReadbackTest,
                        ::testing::Values(.2, 1.0));

class GraphicsComposerTransformReadbackTest : public GraphicsComposerReadbackTest {
   protected:
    void SetUp() override {
        GraphicsComposerReadbackTest::SetUp();

        auto backgroundLayer = std::make_shared<TestColorLayer>(mComposerClient, mPrimaryDisplay);
        backgroundLayer->setColor({0, 0, 0, 0});
        backgroundLayer->setDisplayFrame({0, 0, mDisplayWidth, mDisplayHeight});
        backgroundLayer->setZOrder(0);

        mSideLength = mDisplayWidth < mDisplayHeight ? mDisplayWidth : mDisplayHeight;
        IComposerClient::Rect redRect = {0, 0, mSideLength / 2, mSideLength / 2};
        IComposerClient::Rect blueRect = {mSideLength / 2, mSideLength / 2, mSideLength,
                                          mSideLength};

        mLayer =
            std::make_shared<TestBufferLayer>(mComposerClient, mGralloc, mPrimaryDisplay,
                                              mSideLength, mSideLength, PixelFormat::RGBA_8888);
        mLayer->setDisplayFrame({0, 0, mSideLength, mSideLength});
        mLayer->setZOrder(10);

        std::vector<IComposerClient::Color> baseColors(mSideLength * mSideLength);
        ReadbackHelper::fillColorsArea(baseColors, mSideLength, redRect, RED);
        ReadbackHelper::fillColorsArea(baseColors, mSideLength, blueRect, BLUE);
        ASSERT_NO_FATAL_FAILURE(mLayer->setBuffer(baseColors));

        mLayers = {backgroundLayer, mLayer};
    }

   protected:
    std::shared_ptr<TestBufferLayer> mLayer;
    std::vector<IComposerClient::Color> baseColors;
    std::vector<std::shared_ptr<TestLayer>> mLayers;
    int mSideLength;
};

TEST_F(GraphicsComposerTransformReadbackTest, FLIP_H) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }
    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());
    mLayer->setTransform(Transform::FLIP_H);
    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {mSideLength / 2, 0, mSideLength, mSideLength / 2}, RED);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, mSideLength / 2, mSideLength / 2, mSideLength}, BLUE);

    writeLayers(mLayers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());

    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerTransformReadbackTest, FLIP_V) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }
    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    mLayer->setTransform(Transform::FLIP_V);

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, mSideLength / 2, mSideLength / 2, mSideLength}, RED);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {mSideLength / 2, 0, mSideLength, mSideLength / 2}, BLUE);

    writeLayers(mLayers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

TEST_F(GraphicsComposerTransformReadbackTest, ROT_180) {
    if (!mHasReadbackBuffer) {
        std::cout << "Readback not supported or unsuppported pixelFormat/dataspace or SRGB not a "
                     "valid color mode"
                  << std::endl;
        GTEST_SUCCEED() << "Readback not supported or unsupported pixelFormat/dataspace";
        return;
    }
    ReadbackBuffer readbackBuffer(mPrimaryDisplay, mComposerClient, mGralloc, mDisplayWidth,
                                  mDisplayHeight, mPixelFormat, mDataspace);
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.setReadbackBuffer());

    mLayer->setTransform(Transform::ROT_180);

    std::vector<IComposerClient::Color> expectedColors(mDisplayWidth * mDisplayHeight);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {mSideLength / 2, mSideLength / 2, mSideLength, mSideLength},
                                   RED);
    ReadbackHelper::fillColorsArea(expectedColors, mDisplayWidth,
                                   {0, 0, mSideLength / 2, mSideLength / 2}, BLUE);

    writeLayers(mLayers);
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->validateDisplay();
    execute();
    if (mReader->mCompositionChanges.size() != 0) {
        clearCommandReaderState();
        GTEST_SUCCEED();
        return;
    }
    ASSERT_EQ(0, mReader->mErrors.size());
    mWriter->presentDisplay();
    execute();
    ASSERT_EQ(0, mReader->mErrors.size());
    ASSERT_NO_FATAL_FAILURE(readbackBuffer.checkReadbackBuffer(expectedColors));
}

}  // anonymous namespace
}  // namespace vts
}  // namespace V2_2
}  // namespace composer
}  // namespace graphics
}  // namespace hardware
}  // namespace android
