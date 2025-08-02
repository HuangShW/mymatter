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
    // 新增: 设备类型枚举，用于区分不同功能的设备
    enum DeviceType_t
    {
        kType_On_Off,
        kType_Dimmable,
    };
    static const int kDeviceNameSize     = 32;
    static const int kDeviceLocationSize = 32;

    enum State_t
    {
        kState_On = 0,
        kState_Off,
    } State;

    enum Changed_t
    {
        // 设备在线状态发生变化
        kChanged_Reachable = 0x01,
        // 设备开关状态发生变化
        kChanged_State     = 0x02,
        // 设备位置信息发生变化
        kChanged_Location  = 0x04,
        // 设备名称发生变化
        kChanged_Name      = 0x08,
        // 新增: 定义一个新的变更类型，用于通知外界亮度发生了变化。
        // 这是一个位掩码，允许一次性传递多个状态变更。
        // 设备亮度值发生变化
        kChanged_Level     = 0x10,
    } Changed;

    Device(const char * szDeviceName, const char * szLocation, DeviceType_t type = kType_On_Off);

    bool IsOn() const;
    bool IsReachable() const;
    // 新增: 获取当前亮度值的公有方法。
    // 返回类型为 uint8_t，符合Matter规范中LevelControl Cluster的CurrentLevel属性 (1-254)。
    uint8_t GetLevel() const;
    void SetOnOff(bool aOn);
    void SetReachable(bool aReachable);
    // 新增: 设置当前亮度值的公有方法。
    // 参数 level 的范围应该是 1-254，其中 1 表示最暗，254 表示最亮。
    void SetLevel(uint8_t level);
    void SetName(const char * szDeviceName);
    void SetLocation(const char * szLocation);
    inline void SetEndpointId(chip::EndpointId id) { mEndpointId = id; };
    inline chip::EndpointId GetEndpointId() { return mEndpointId; };
    inline char * GetName() { return mName; };
    inline char * GetLocation() { return mLocation; };
    inline DeviceType_t GetType() { return mType; };

    using DeviceCallback_fn = std::function<void(Device *, Changed_t)>;
    void SetChangeCallback(DeviceCallback_fn aChanged_CB);

private:
    // 新增：设备类型
    DeviceType_t mType;
    // 设备开关状态
    State_t mState;
    // 设备是否在线可达
    bool mReachable;
    // 新增: 私有成员变量，用于在内存中存储设备的当前亮度值。
    // 类型为 uint8_t，符合Matter规范中LevelControl Cluster的CurrentLevel属性 (0-254)。
    uint8_t mLevel;
    // 新增: 私有成员变量，用于存储设备关闭前的最后一次亮度值。
    // 当设备从关闭状态重新打开时，可以恢复到这个亮度。
    uint8_t mLastLevel;
    char mName[kDeviceNameSize];
    char mLocation[kDeviceLocationSize];
    chip::EndpointId mEndpointId;
    DeviceCallback_fn mChanged_CB;
};
