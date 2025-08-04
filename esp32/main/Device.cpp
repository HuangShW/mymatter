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
    mCurrentLevel = 254; // Full brightness (1-254 range)
    mMinLevel = 1;       // Minimum level per Matter spec
    mMaxLevel = 254;     // Maximum level per Matter spec
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
    bool changed;

    if (aOn)
    {
        changed = (mState != kState_On);
        mState  = kState_On;
        ChipLogProgress(DeviceLayer, "Device[%s]: ON", mName);
        
        // According to Matter spec, when turning ON, if current level is too low,
        // we should set it to a reasonable level. This implements OnOff/Level sync.
        if (changed && mCurrentLevel == 0)
        {
            // If level is 0 (which shouldn't happen in our 1-254 range, but just in case)
            // or if we want to ensure a minimum brightness when turning on
            uint8_t onLevel = 254; // Default to full brightness
            if (mCurrentLevel != onLevel)
            {
                mCurrentLevel = onLevel;
                ChipLogProgress(DeviceLayer, "Device[%s]: Level synced to %d on ON", mName, onLevel);
                
                // Trigger Level change callback with both State and Level changed
                if (mChanged_CB)
                {
                    mChanged_CB(this, static_cast<Changed_t>(Device::kChanged_State | Device::kChanged_Level));
                    return; // Early return since we already called the callback
                }
            }
        }
    }
    else
    {
        changed = (mState != kState_Off);
        mState  = kState_Off;
        ChipLogProgress(DeviceLayer, "Device[%s]: OFF", mName);
        
        // When turning OFF, we keep the current level value
        // This allows resuming to the same brightness when turned back on
    }

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
