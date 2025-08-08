/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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

#include "Device.h"

#include <cstdio>
#include <lib/support/CHIPMemString.h>
#include <platform/CHIPDeviceLayer.h>

using namespace ::chip::Platform;

Device::Device(const char * szDeviceName, const char * szLocation, DeviceType_t deviceType)
{
    CopyString(mName, sizeof(mName), szDeviceName);
    CopyString(mLocation, sizeof(mLocation), szLocation);
    mState      = kState_Off;
    mReachable  = false;
    mEndpointId = 0;
    mDeviceType = deviceType;
    mCurrentLevel = 254; // 默认满亮度（1-254 范围）
    mMinLevel = 1;       // 最小亮度，符合 Matter 规范
    mMaxLevel = 254;     // 最大亮度，符合 Matter 规范
    mChanged_CB = nullptr;
}

bool Device::IsOn() const
{
    return mState == kState_On;
}

bool Device::IsReachable() const
{
    return mReachable;
}

void Device::SetOnOff(bool aOn)
{
    bool changed = (mState != (aOn ? kState_On : kState_Off));
    mState = aOn ? kState_On : kState_Off;
    
    ChipLogProgress(DeviceLayer, "Device[%s]: %s", mName, aOn ? "ON" : "OFF");

    // 移除与 Level 的联动，交由 OnOff/LevelControl 插件处理

    if (changed && mChanged_CB)
    {
        mChanged_CB(this, kChanged_State);
    }
}

void Device::SetReachable(bool aReachable)
{
    bool changed = (mReachable != aReachable);

    mReachable = aReachable;

    if (aReachable)
    {
        ChipLogProgress(DeviceLayer, "Device[%s]: ONLINE", mName);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "Device[%s]: OFFLINE", mName);
    }

    if (changed && mChanged_CB)
    {
        mChanged_CB(this, kChanged_Reachable);
    }
}

void Device::SetName(const char * szName)
{
    bool changed = (strncmp(mName, szName, sizeof(mName)) != 0);

    ChipLogProgress(DeviceLayer, "Device[%s]: New Name=\"%s\"", mName, szName);

    CopyString(mName, sizeof(mName), szName);

    if (changed && mChanged_CB)
    {
        mChanged_CB(this, kChanged_Name);
    }
}

void Device::SetLocation(const char * szLocation)
{
    bool changed = (strncmp(mLocation, szLocation, sizeof(mLocation)) != 0);

    CopyString(mLocation, sizeof(mLocation), szLocation);

    ChipLogProgress(DeviceLayer, "Device[%s]: Location=\"%s\"", mName, mLocation);

    if (changed && mChanged_CB)
    {
        mChanged_CB(this, kChanged_Location);
    }
}

void Device::SetChangeCallback(DeviceCallback_fn aChanged_CB)
{
    mChanged_CB = aChanged_CB;
}

void Device::SetLevel(uint8_t aLevel)
{
    // Clamp level to valid range (1-254)
    if (aLevel < mMinLevel)
    {
        aLevel = mMinLevel;
    }
    else if (aLevel > mMaxLevel)
    {
        aLevel = mMaxLevel;
    }

    bool changed = (mCurrentLevel != aLevel);
    mCurrentLevel = aLevel;

    ChipLogProgress(DeviceLayer, "Device[%s]: Level=%d", mName, aLevel);

    if (changed && mChanged_CB)
    {
        mChanged_CB(this, Device::kChanged_Level);
    }
}

uint8_t Device::GetCurrentLevel() const
{
    return mCurrentLevel;
}

uint8_t Device::GetMinLevel() const
{
    return mMinLevel;
}

uint8_t Device::GetMaxLevel() const
{
    return mMaxLevel;
}

bool Device::SupportsLevelControl() const
{
    return mDeviceType == kDeviceType_DimmableLight; // 仅可调光灯支持 Level Control
}
