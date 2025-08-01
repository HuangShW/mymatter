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

Device::Device(const char * szDeviceName, const char * szLocation)
{
    CopyString(mName, sizeof(mName), szDeviceName);
    CopyString(mLocation, sizeof(mLocation), szLocation);
    mState      = kState_Off;
    mReachable  = false;
    mEndpointId = 0;
    mChanged_CB = nullptr;
    // 新增: 在设备对象被创建时，将其亮度初始化为 0 (最暗)。
    mLevel      = 0;
}

bool Device::IsOn() const
{
    return mState == kState_On;
}

bool Device::IsReachable() const
{
    return mReachable;
}

// 新增: GetLevel 方法的具体实现。
// 当Matter SDK需要读取设备的当前亮度时，会调用此方法。
uint8_t Device::GetLevel() const
{
    return mLevel;
}

void Device::SetOnOff(bool aOn)
{
    bool changed;

    if (aOn)
    {
        changed = (mState != kState_On);
        mState  = kState_On;
        ChipLogProgress(DeviceLayer, "Device[%s]: ON", mName);
    }
    else
    {
        changed = (mState != kState_Off);
        mState  = kState_Off;
        ChipLogProgress(DeviceLayer, "Device[%s]: OFF", mName);
    }

    if (changed && mChanged_CB)
    {
        mChanged_CB(this, kChanged_State);
    }
}

// 新增: SetLevel 方法的具体实现。
// 当Matter SDK收到来自控制器的亮度设置请求时，会调用此方法。
void Device::SetLevel(uint8_t aLevel)
{
    // 检查亮度值是否真的发生了变化。
    bool changed = (mLevel != aLevel);
    mLevel       = aLevel;
    ChipLogProgress(DeviceLayer, "Device[%s]: Level=%d", mName, mLevel);

    // 如果亮度值确实改变了，并且已经注册了回调函数，
    // 则调用该回调，并传入 kChanged_Level 标志。
    // 这会触发 HandleDeviceStatusChanged 函数，进而通知Matter SDK亮度已更新。
    if (changed && mChanged_CB)
    {
        mChanged_CB(this, kChanged_Level);
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
