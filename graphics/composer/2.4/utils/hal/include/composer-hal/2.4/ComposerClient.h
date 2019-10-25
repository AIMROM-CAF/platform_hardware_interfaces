/*
 * Copyright 2019 The Android Open Source Project
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

#pragma once

#ifndef LOG_TAG
#warning "ComposerClient.h included without LOG_TAG"
#endif

#include <android/hardware/graphics/composer/2.4/IComposerClient.h>
#include <composer-hal/2.4/ComposerHal.h>

namespace android {
namespace hardware {
namespace graphics {
namespace composer {
namespace V2_4 {
namespace hal {

namespace detail {

// ComposerClientImpl implements V2_*::IComposerClient on top of V2_*::ComposerHal
template <typename Interface, typename Hal>
class ComposerClientImpl : public V2_3::hal::detail::ComposerClientImpl<Interface, Hal> {
  public:
    ComposerClientImpl(Hal* hal) : BaseType2_3(hal) {}

    Return<void> getDisplayCapabilities_2_4(
            Display display, IComposerClient::getDisplayCapabilities_2_4_cb hidl_cb) override {
        std::vector<IComposerClient::DisplayCapability> capabilities;
        Error error = mHal->getDisplayCapabilities_2_4(display, &capabilities);
        hidl_cb(error, capabilities);
        return Void();
    }

    Return<void> getDisplayConnectionType(
            Display display, IComposerClient::getDisplayConnectionType_cb hidl_cb) override {
        IComposerClient::DisplayConnectionType type;
        Error error = mHal->getDisplayConnectionType(display, &type);
        hidl_cb(error, type);
        return Void();
    }

    static std::unique_ptr<ComposerClientImpl> create(Hal* hal) {
        auto client = std::make_unique<ComposerClientImpl>(hal);
        return client->init() ? std::move(client) : nullptr;
    }

  private:
    using BaseType2_3 = V2_3::hal::detail::ComposerClientImpl<Interface, Hal>;
    using BaseType2_1 = V2_1::hal::detail::ComposerClientImpl<Interface, Hal>;
    using BaseType2_1::mHal;
};

}  // namespace detail

using ComposerClient = detail::ComposerClientImpl<IComposerClient, ComposerHal>;

}  // namespace hal
}  // namespace V2_4
}  // namespace composer
}  // namespace graphics
}  // namespace hardware
}  // namespace android
