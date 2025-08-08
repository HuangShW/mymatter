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

/**
 * @file Device.h
 * @brief 设备抽象与状态变更通知的接口定义。
 * @details 封装 Matter 桥接设备的基本属性（名称、位置、开关、连通性）
 *          以及可选的调光（亮度 0-254）与色温（Mireds 153-500）控制，
 *          并提供变更回调以将状态变化通知给上层。
 */
// These are the bridged devices
#include <app/util/attribute-storage.h>
#include <functional>
#include <stdbool.h>
#include <stdint.h>
#include <map>
#include <mutex>

/**
 * @brief 桥接设备的轻量级抽象。
 * @note 线程安全：设备集合的注册/查询由内部互斥量保护（见 AddDevice / GetDevice）。
 */
class Device
{
public:
    static const int kDeviceNameSize     = 32;
    static const int kDeviceLocationSize = 32;

    /**
     * @brief 将设备注册到指定端点。
     * @param endpoint Matter 端点 ID（不得为 kInvalidEndpointId）
     * @param device   设备实例指针（不得为 nullptr）
     * @note 线程安全，内部使用互斥量保护端点映射。
     */
    static void AddDevice(chip::EndpointId endpoint, Device *device);

    /**
     * @brief 按端点查询设备。
     * @param endpoint Matter 端点 ID
     * @return Device* 若存在返回指针，否则返回 nullptr。
     */
    static Device * GetDevice(chip::EndpointId endpoint);

    /**
     * @brief 设备开关状态。
     */
    enum State_t
    {
        kState_On = 0,  ///< 设备处于开启状态
        kState_Off,     ///< 设备处于关闭状态
    } State;

    /**
     * @brief 设备属性变更标志（可按位或）。
     */
    enum Changed_t
    {
        kChanged_Reachable  = 0x01, ///< 在线/离线状态变化
        kChanged_State      = 0x02, ///< 开关状态变化
        kChanged_Location   = 0x04, ///< 位置标签变化
        kChanged_Name       = 0x08, ///< 名称变化
        kChanged_Level      = 0x10, ///< 亮度变化
        kChanged_ColorTemp  = 0x20, ///< 色温变化
    } Changed;

    /**
     * @brief 构造函数。
     * @param szDeviceName 设备名称（C 字符串，最大 32 字节，不含终止符）
     * @param szLocation   设备位置（C 字符串，最大 32 字节，不含终止符）
     */
    Device(const char * szDeviceName, const char * szLocation);

    /** @brief 是否处于开启状态。*/
    bool IsOn() const;

    /** @brief 设备是否可达（在线）。*/
    bool IsReachable() const;

    /**
     * @brief 设置开关状态。
     * @param aOn true=开启，false=关闭
     * @note 若状态发生变化，将触发变更回调（标志：kChanged_State）。
     */
    void SetOnOff(bool aOn);

    /**
     * @brief 设置可达（在线）状态。
     * @param aReachable true=在线，false=离线
     * @note 若状态发生变化，将触发变更回调（标志：kChanged_Reachable）。
     */
    void SetReachable(bool aReachable);

    /**
     * @brief 设置设备名称。
     * @param szDeviceName 新名称
     * @note 若名称变化，将触发变更回调（标志：kChanged_Name）。
     */
    void SetName(const char * szDeviceName);

    /**
     * @brief 设置设备位置。
     * @param szLocation 新位置
     * @note 若位置变化，将触发变更回调（标志：kChanged_Location）。
     */
    void SetLocation(const char * szLocation);

    /**
     * @brief 设置亮度等级。
     * @param aLevel 亮度（0-254）。0 表示最暗，同时会将状态更新为 Off；>0 会更新为 On。
     * @note 若亮度或由此引发的开关状态变化，将触发变更回调（标志：kChanged_Level 以及可选 kChanged_State）。
     */
    void SetLevel(uint8_t aLevel);

    /**
     * @brief 设置色温（Mireds）。
     * @param aColorTemp 范围 [153, 500]，约等于 [6500K, 2000K]
     * @note 若色温变化，将触发变更回调（标志：kChanged_ColorTemp）。
     */
    void SetColorTemperature(uint16_t aColorTemp);

    /** @brief 获取当前色温（Mireds）。*/
    inline uint16_t GetColorTemperature() const { return mColorTemperature; };

    /** @brief 获取当前亮度（0-254）。*/
    inline uint8_t GetLevel() const { return mLevel; };

    /** @brief 关联端点 ID。*/
    inline void SetEndpointId(chip::EndpointId id) { mEndpointId = id; };

    /** @brief 获取关联端点 ID。*/
    inline chip::EndpointId GetEndpointId() const { return mEndpointId; };

    /** @brief 获取设备名称（以 C 字符串形式返回内部缓冲区）。*/
    inline char * GetName() { return mName; };

    /** @brief 获取设备位置（以 C 字符串形式返回内部缓冲区）。*/
    inline char * GetLocation() { return mLocation; };

    /**
     * @brief 设备状态变更回调类型。
     * @param Device*   触发回调的设备实例
     * @param Changed_t 变更标志（可多位按位或）
     */
    using DeviceCallback_fn = std::function<void(Device *, Changed_t)>;

    /** @brief 设置状态变更回调。*/
    void SetChangeCallback(DeviceCallback_fn aChanged_CB);

private:
    State_t mState;                      ///< 当前开关状态
    bool mReachable;                     ///< 在线/可达状态
    char mName[kDeviceNameSize];         ///< 设备名称（最多 32 字节，含终止符）
    char mLocation[kDeviceLocationSize]; ///< 设备位置（最多 32 字节，含终止符）
    chip::EndpointId mEndpointId;        ///< 关联的 Matter 端点 ID
    DeviceCallback_fn mChanged_CB;       ///< 状态变更回调函数对象
    uint8_t mLevel = 254;                ///< 亮度等级（0-254，254 表示最大）
    uint16_t mColorTemperature = 153;    ///< 色温（Mireds，153≈6500K，500≈2000K）
};