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

#include "./include/Device.h"

#include <cstdio>
#include <lib/support/CHIPMemString.h>
#include <platform/CHIPDeviceLayer.h>

using namespace ::chip::Platform;

Device::Device(const char * szDeviceName, const char * szLocation, DeviceType_t type)
{
    CopyString(mName, sizeof(mName), szDeviceName);
    CopyString(mLocation, sizeof(mLocation), szLocation);
    mType       = type;
    mState      = kState_Off;
    mReachable  = false;
    mEndpointId = 0;
    mChanged_CB = nullptr;
    // 新增: 在设备对象被创建时，将其亮度初始化为 0 (关闭状态)。
    mLevel      = 0;
    // 新增: 将设备关闭前的最后一次亮度值初始化为最大亮度 254。
    mLastLevel  = 254;
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
    ChipLogProgress(DeviceLayer, "Device[%s]: SetOnOff called with aOn=%s (current state=%s, current level=%d, type=%d)", 
                    mName, aOn ? "ON" : "OFF", mState == kState_On ? "ON" : "OFF", mLevel, (int)mType);
    
    // 增加：修改 on/off 逻辑
    State_t newState = aOn ? kState_On : kState_Off;
    if (mState != newState)
    {
        ChipLogProgress(DeviceLayer, "Device[%s]: State changing from %s to %s", 
                        mName, mState == kState_On ? "ON" : "OFF", newState == kState_On ? "ON" : "OFF");
        
        mState = newState;
        // 如果开灯且当前level为0, 恢复上次亮度 (仅限调光设备)
        if (aOn && mType == kType_Dimmable && mLevel == 0)
        {
            ChipLogProgress(DeviceLayer, "Device[%s]: Turning ON - restoring level from %d to %d", mName, mLevel, mLastLevel);
            mLevel = mLastLevel;
            if (mChanged_CB)
            {
                mChanged_CB(this, kChanged_Level);
            }
        }
        // 如果关灯, 保存当前亮度并设置level为0 (仅限调光设备)
        else if (!aOn && mType == kType_Dimmable)
        {
            ChipLogProgress(DeviceLayer, "Device[%s]: Turning OFF - saving level %d and setting to 0", mName, mLevel);
            mLastLevel = mLevel;
            mLevel = 0;
            if (mChanged_CB)
            {
                mChanged_CB(this, kChanged_Level);
            }
        }

        ChipLogProgress(DeviceLayer, "Device[%s]: Final state after SetOnOff - state=%s, level=%d, lastLevel=%d", 
                        mName, mState == kState_On ? "ON" : "OFF", mLevel, mLastLevel);

        if (mChanged_CB)
        {
            mChanged_CB(this, kChanged_State);
        }
    }
    else
    {
        ChipLogProgress(DeviceLayer, "Device[%s]: SetOnOff - no state change needed", mName);
    }
}

// 新增: SetLevel 方法的具体实现。
// 当Matter SDK收到来自控制器的亮度设置请求时，会调用此方法。
void Device::SetLevel(uint8_t aLevel)
{
    ChipLogProgress(DeviceLayer, "Device[%s]: SetLevel called with aLevel=%d, mType=%d", mName, aLevel, (int)mType);
    
    // 此方法仅对调光设备有效
    if (mType != kType_Dimmable)
    {
        ChipLogProgress(DeviceLayer, "Device[%s]: SetLevel ignored - not a dimmable device (type=%d)", mName, (int)mType);
        return;
    }

    if (aLevel > 254) {
        aLevel = 254;
    }

    ChipLogProgress(DeviceLayer, "Device[%s]: SetLevel processing - old mLevel=%d, new aLevel=%d, mState=%d", 
                    mName, mLevel, aLevel, (int)mState);

    if (mLevel != aLevel) {
        mLevel = aLevel;
        if (mLevel > 0)
        {
            mLastLevel = mLevel;
            if (mState != kState_On)
            {
                ChipLogProgress(DeviceLayer, "Device[%s]: SetLevel turning device ON (level=%d)", mName, mLevel);
                mState = kState_On;
                if (mChanged_CB) {
                    mChanged_CB(this, kChanged_State);
                }
            }
        }
        else // mLevel == 0
        {
            if (mState != kState_Off)
            {
                ChipLogProgress(DeviceLayer, "Device[%s]: SetLevel turning device OFF (level=0)", mName);
                mState = kState_Off;
                if (mChanged_CB) {
                    mChanged_CB(this, kChanged_State);
                }
            }
        }
        
        ChipLogProgress(DeviceLayer, "Device[%s]: SetLevel completed - mLevel=%d, mState=%d", mName, mLevel, (int)mState);
        if (mChanged_CB)
    {
        mChanged_CB(this, kChanged_Level);
        }
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
