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

/**
 * @file Device.cpp
 * @brief Device 类实现文件，包含开关、连通性、调光与色温的设置逻辑。
 */
#include "Device.h"

#include <cstdio>
#include <lib/support/CHIPMemString.h>
#include <platform/CHIPDeviceLayer.h>

using namespace ::chip::Platform;

/**
 * @brief 构造函数：初始化名称与位置，并设置默认状态。
 * @param szDeviceName 设备名称
 * @param szLocation   设备位置
 */
Device::Device(const char * szDeviceName, const char * szLocation)
{
    CopyString(mName, sizeof(mName), szDeviceName);
    CopyString(mLocation, sizeof(mLocation), szLocation);
    mState      = kState_Off;
    mReachable  = false;
    mEndpointId = 0;
    mChanged_CB = nullptr;
}

/** @brief 判断设备是否为开启状态。*/
bool Device::IsOn() const
{
    return mState == kState_On;
}

/** @brief 判断设备是否在线/可达。*/
bool Device::IsReachable() const
{
    return mReachable;
}

/**
 * @brief 设置开关状态，并在变化时回调通知。
 * @param aOn true=开启，false=关闭
 */
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

/**
 * @brief 设置设备可达（在线）状态，并在变化时回调通知。
 * @param aReachable true=在线，false=离线
 */
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

/**
 * @brief 设置设备名称，并在变化时回调通知。
 * @param szName 新名称
 */
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

/**
 * @brief 设置设备位置，并在变化时回调通知。
 * @param szLocation 新位置
 */
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

/**
 * @brief 设置状态变更回调。
 * @param aChanged_CB 回调函数
 */
void Device::SetChangeCallback(DeviceCallback_fn aChanged_CB)
{
    mChanged_CB = aChanged_CB;
}

/**
 * @brief 设置亮度等级，并按需联动开关状态；在实际变化时触发回调。
 * @param aLevel 亮度（0-254）
 * @note aLevel 为 0 时将状态置为 Off；>0 时置为 On。
 */
void Device::SetLevel(uint8_t aLevel)
{
    ChipLogProgress(DeviceLayer, "SetLevel= %x",aLevel);
    // 约束亮度值在有效范围内 (0-100)
    if (aLevel > 254) {
        aLevel = 254;
    }

    // 仅当亮度值发生变化时才更新
    if (mLevel != aLevel) {
        mLevel = aLevel;
        
        // 检查开关状态是否需要更新
        State_t newState = (aLevel > 0) ? kState_On : kState_Off;
        Changed_t changedFlags = kChanged_Level;
        
        if (mState != newState) {
            mState = newState;
            changedFlags = static_cast<Changed_t>(changedFlags | kChanged_State);
        }
        
        // 触发回调通知
        if (mChanged_CB) {
            mChanged_CB(this, changedFlags);
        }
    }
}

/**
 * @brief 设置色温（Mireds，范围 [153, 500]），在实际变化时触发回调。
 * @param aColorTemp 目标色温（Mireds）
 */
void Device::SetColorTemperature(uint16_t aColorTemp)
{
    // 约束色温在合理范围内 (2700K-6500K)
    constexpr uint16_t kMinColorTemp = 153;
    constexpr uint16_t kMaxColorTemp = 500;
    ChipLogProgress(DeviceLayer, "SetColorTemperature = %x",aColorTemp);
    if (aColorTemp < kMinColorTemp) {
        aColorTemp = kMinColorTemp;
    } else if (aColorTemp > kMaxColorTemp) {
        aColorTemp = kMaxColorTemp;
    }

    // 仅当色温值发生变化时才更新
    if (mColorTemperature != aColorTemp) {
        mColorTemperature = aColorTemp;
        
        // 触发回调通知
        if (mChanged_CB) {
            mChanged_CB(this, kChanged_ColorTemp);
        }
    }
}

/*添加设备和端点绑定关系*/
/**
 * @brief 获取端点到设备的全局映射（单例）。
 */
static std::map<chip::EndpointId, Device*>& GetEndpointMap()
{
    static std::map<chip::EndpointId, Device*> sEndpointMap;
    return sEndpointMap;
}

/**
 * @brief 获取保护端点映射的互斥量（单例）。
 */
static std::mutex& GetEndpointMapMutex()
{
    static std::mutex sMutex;
    return sMutex;
}

/**
 * @brief 将设备注册到指定端点（若端点已存在则报错）。
 * @param endpoint 端点 ID
 * @param device   设备实例
 */
void Device::AddDevice(chip::EndpointId endpoint, Device *device)
{
    std::lock_guard<std::mutex> lock(GetEndpointMapMutex());
    
    if (!device || endpoint == chip::kInvalidEndpointId) {
        ChipLogError(DeviceLayer, "Invalid device or endpoint");
        return;
    }
    
    auto& endpointMap = GetEndpointMap();
    auto it = endpointMap.find(endpoint);
    
    if (it != endpointMap.end()) {
        ChipLogError(DeviceLayer, "Endpoint %d already registered", endpoint);
        return;
    }
    
    endpointMap[endpoint] = device;
    device->SetEndpointId(endpoint);
    ChipLogProgress(DeviceLayer, "Added device %s to endpoint %d", device->GetName(), endpoint);
}

/**
 * @brief 根据端点查询设备。
 * @param endpoint 端点 ID
 * @return Device* 若存在返回指针，否则返回 nullptr。
 */
Device * Device::GetDevice(chip::EndpointId endpoint)
{
    std::lock_guard<std::mutex> lock(GetEndpointMapMutex());
    
    auto& endpointMap = GetEndpointMap();
    auto it = endpointMap.find(endpoint);
    
    if (it != endpointMap.end()) {
        return it->second;
    }
    
    ChipLogError(DeviceLayer, "No device found for endpoint %d", endpoint);
    return nullptr;
}