/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

// These are the bridged devices
#include <app/util/attribute-storage.h>
#include <functional>
#include <stdbool.h>
#include <stdint.h>

class Device
{
public:
    static const int kDeviceNameSize     = 32;
    static const int kDeviceLocationSize = 32;

    enum State_t
    {
        kState_On = 0,
        kState_Off,
    } State;

    enum Changed_t
    {
        kChanged_Reachable = 0x01,
        kChanged_State     = 0x02,
        kChanged_Location  = 0x04,
        kChanged_Name      = 0x08,
        kChanged_Level     = 0x10,
    } Changed;

    enum DeviceType_t
    {
        kDeviceType_OnOffLight = 0,
        kDeviceType_DimmableLight = 1,
    };

    Device(const char * szDeviceName, const char * szLocation, DeviceType_t deviceType = kDeviceType_OnOffLight);

    bool IsOn() const;
    bool IsReachable() const;
    void SetOnOff(bool aOn);
    void SetReachable(bool aReachable);
    void SetName(const char * szDeviceName);
    void SetLocation(const char * szLocation);
    void SetLevel(uint8_t aLevel);
    uint8_t GetCurrentLevel() const;
    uint8_t GetMinLevel() const;
    uint8_t GetMaxLevel() const;
    bool SupportsLevelControl() const;
    inline void SetEndpointId(chip::EndpointId id) { mEndpointId = id; };
    inline chip::EndpointId GetEndpointId() { return mEndpointId; };
    inline char * GetName() { return mName; };
    inline char * GetLocation() { return mLocation; };

    using DeviceCallback_fn = std::function<void(Device *, Changed_t)>;
    void SetChangeCallback(DeviceCallback_fn aChanged_CB);

private:
    State_t mState;
    bool mReachable;
    char mName[kDeviceNameSize];
    char mLocation[kDeviceLocationSize];
    chip::EndpointId mEndpointId;
    DeviceType_t mDeviceType;
    uint8_t mCurrentLevel;
    uint8_t mMinLevel;
    uint8_t mMaxLevel;
    DeviceCallback_fn mChanged_CB;
};
