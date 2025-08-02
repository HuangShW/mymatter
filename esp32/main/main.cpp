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

#include "include/Device.h"
#include "DeviceCallbacks.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/reporting/reporting.h>
#include <app/server/OnboardingCodesUtil.h>
#include <app/util/attribute-storage.h>
#include <app/util/endpoint-config-api.h>
// #include <bridged-actions-stub.h>
#include <common/Esp32AppServer.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/core/CHIPError.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CHIPMemString.h>
#include <lib/support/ZclString.h>
#include <platform/ESP32/ESP32Utils.h>
// #include <setup_payload/OnboardingCodesUtil.h>

#include <app/InteractionModelEngine.h>
#include <app/server/Server.h>

#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

#if CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
#include <platform/ESP32/ESP32DeviceInfoProvider.h>
#else
#include <DeviceInfoProviderImpl.h>
#endif // CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER

namespace {
#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
chip::DeviceLayer::ESP32FactoryDataProvider sFactoryDataProvider;
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

#if CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER
chip::DeviceLayer::ESP32DeviceInfoProvider gExampleDeviceInfoProvider;
#else
chip::DeviceLayer::DeviceInfoProviderImpl gExampleDeviceInfoProvider;
#endif // CONFIG_ENABLE_ESP32_DEVICE_INFO_PROVIDER

// sdk修改
// std::unique_ptr<chip::app::Clusters::Actions::ActionsDelegateImpl> sActionsDelegateImpl;
// std::unique_ptr<chip::app::Clusters::Actions::ActionsServer> sActionsServer;
} // namespace

extern const char TAG[] = "bridge-app";

using namespace ::chip;
using namespace ::chip::DeviceManager;
using namespace ::chip::Platform;
using namespace ::chip::Credentials;
using namespace ::chip::app::Clusters;

static AppDeviceCallbacks AppCallback;

static const int kNodeLabelSize = 32;
// Current ZCL implementation of Struct uses a max-size array of 254 bytes
static const int kDescriptorAttributeArraySize = 254;

static EndpointId gCurrentEndpointId;
static EndpointId gFirstDynamicEndpointId;
static Device * gDevices[CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT]; // number of dynamic endpoints count

// 4 Bridged devices
// 删除
static Device gLight1("Light 1", "Office", Device::kType_On_Off);
// static Device gLight2("Light 2", "Office");
// static Device gLight3("Light 3", "Kitchen");
// static Device gLight4("Light 4", "Den");
// 新增: 创建一个全局的 Device 对象实例，并明确其类型为可调光灯。
static Device gDimmableLight("Dimmable Light 1", "Living Room", Device::kType_Dimmable);

// 新增
// static Device gThermostat("Thermostat", "Office");

// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGED_NODE 0x0013
// (taken from lo-devices.xml)
#define DEVICE_TYPE_LO_ON_OFF_LIGHT 0x0100
// 新增: 这是 Matter "Luminance-only" (LO) 设备类型规范中为"可调光灯"定义的设备类型ID。
// 我们用这个ID来告诉Matter网络，我们桥接的是一个什么样的设备。
#define DEVICE_TYPE_LO_DIMMABLE_LIGHT 0x0101

// (taken from chip-devices.xml)
#define DEVICE_TYPE_ROOT_NODE 0x0016
// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGE 0x000e

// Device Version for dynamic endpoints:
#define DEVICE_VERSION_DEFAULT 1

/* BRIDGED DEVICE ENDPOINT: contains the following clusters:
   - On/Off
   - Descriptor
   - Bridged Device Basic Information
*/

// Declare On/Off cluster attributes
// 删除
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(onOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnOff::Id, BOOLEAN, 1, 0), /* on/off */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// 新增: 声明 LevelControl Cluster (亮度控制集群) 包含的属性。
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(levelControlAttrs)
    // 声明 CurrentLevel 属性，类型为 uint8_t (INT8U)，可读写。
    // 当控制器需要读取或写入当前亮度时，会访问这个属性。范围：1-254
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::CurrentLevel::Id, INT8U, 1, ZAP_ATTRIBUTE_MASK(WRITABLE)), /* Level */
    // 声明 MinLevel 属性，代表最小亮度。固定值：1
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MinLevel::Id, INT8U, 1, 0),   /* MinLevel */
    // 声明 MaxLevel 属性，代表最大亮度。固定值：254
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MaxLevel::Id, INT8U, 1, 0),   /* MaxLevel */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare On/Off cluster attributes
// 新增
// DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(ThermostatAttrs)
// DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::LocalTemperature::Id,INT16U,2,0), /* 读取本地温度 */
// DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::OccupiedCoolingSetpoint::Id,INT16S,2,MATTER_ATTRIBUTE_FLAG_WRITABLE), /* 设置制冷温度 */
// DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::SystemMode::Id,INT8U,1,MATTER_ATTRIBUTE_FLAG_WRITABLE), /* 设置系统模式 */
// DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::ThermostatRunningMode::Id,INT8U,1,0), /* 读取系统模式 */
// DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::HVACSystemTypeConfiguration::Id,INT8U,1,MATTER_ATTRIBUTE_FLAG_WRITABLE), /* 冷暖模式设置 */
// DECLARE_DYNAMIC_ATTRIBUTE(Thermostat::Attributes::FeatureMap::Id,INT8U,1,0), /* 支持的功能 */
//     DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();


// Declare Descriptor cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::DeviceTypeList::Id, ARRAY, kDescriptorAttributeArraySize, 0), /* device list */
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ServerList::Id, ARRAY, kDescriptorAttributeArraySize, 0), /* server list */
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ClientList::Id, ARRAY, kDescriptorAttributeArraySize, 0), /* client list */
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::PartsList::Id, ARRAY, kDescriptorAttributeArraySize, 0),  /* parts list */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Bridged Device Basic Information cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(bridgedDeviceBasicAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(BridgedDeviceBasicInformation::Attributes::NodeLabel::Id, CHAR_STRING, kNodeLabelSize, 0), /* NodeLabel */
    DECLARE_DYNAMIC_ATTRIBUTE(BridgedDeviceBasicInformation::Attributes::Reachable::Id, BOOLEAN, 1, 0),              /* Reachable */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Cluster List for Bridged Light endpoint
// TODO: It's not clear whether it would be better to get the command lists from
// the ZAP config on our last fixed endpoint instead.
// 删除
constexpr CommandId onOffIncomingCommands[] = {
    app::Clusters::OnOff::Commands::Off::Id,
    app::Clusters::OnOff::Commands::On::Id,
    app::Clusters::OnOff::Commands::Toggle::Id,
    app::Clusters::OnOff::Commands::OffWithEffect::Id,
    app::Clusters::OnOff::Commands::OnWithRecallGlobalScene::Id,
    app::Clusters::OnOff::Commands::OnWithTimedOff::Id,
    kInvalidCommandId,
};

// 新增: 声明 LevelControl Cluster 支持的命令列表。
// 控制器 (如手机App) 可以发送这些命令来控制灯的亮度。
constexpr CommandId levelControlIncomingCommands[] = {
    app::Clusters::LevelControl::Commands::MoveToLevel::Id, // 移动到指定亮度
    app::Clusters::LevelControl::Commands::Move::Id,        // 朝某个方向增/减亮度
    app::Clusters::LevelControl::Commands::Step::Id,        // 步进增/减亮度
    app::Clusters::LevelControl::Commands::Stop::Id,        // 停止移动
    app::Clusters::LevelControl::Commands::MoveToLevelWithOnOff::Id, // 移动到指定亮度并同时开关灯
    app::Clusters::LevelControl::Commands::MoveWithOnOff::Id,        // 朝某个方向增/减亮度并同时开关灯
    app::Clusters::LevelControl::Commands::StepWithOnOff::Id,        // 步进增/减亮度并同时开关灯
    app::Clusters::LevelControl::Commands::StopWithOnOff::Id,        // 停止移动并同时开关灯
    kInvalidCommandId, // 列表结束符
};

// constexpr CommandId thermostatIncomingCommands[] = {
//     app::Clusters::Thermostat::Commands::AtomicRequest::Id,
//     app::Clusters::Thermostat::Commands::AtomicResponse::Id,
//     app::Clusters::Thermostat::Commands::ClearWeeklySchedule::Id,
//     app::Clusters::Thermostat::Commands::GetWeeklySchedule::Id,
//     app::Clusters::Thermostat::Commands::GetWeeklyScheduleResponse::Id,
//     app::Clusters::Thermostat::Commands::SetActivePresetRequest::Id,
//     app::Clusters::Thermostat::Commands::SetActiveScheduleRequest::Id,
//     app::Clusters::Thermostat::Commands::SetpointRaiseLower::Id,
//     app::Clusters::Thermostat::Commands::SetWeeklySchedule::Id,
//     kInvalidCommandId,
// };

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr) DECLARE_DYNAMIC_CLUSTER_LIST_END;

// 新增: 为"可调光灯"声明一个新的 Cluster 列表。
// 这个列表定义了我们的新设备具备的所有功能。
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedDimmableLightClusters)
    // 包含 OnOff Cluster，使其具备开关功能。
    DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    // 包含 LevelControl Cluster，使其具备调光功能。
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelControlAttrs, ZAP_CLUSTER_MASK(SERVER), levelControlIncomingCommands, nullptr),
    // 包含 Descriptor Cluster，这是所有端点都必须有的，用于描述自身。
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    // 包含 BridgedDeviceBasicInformation Cluster，用于报告被桥接设备的基本信息。
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr)
DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedLightEndpoint, bridgedLightClusters);

// 新增: 使用上面定义的 Cluster 列表，声明一个新的"端点模板"。
// 这个模板可以被复用，快速创建多个可调光灯设备。
DECLARE_DYNAMIC_ENDPOINT(bridgedDimmableLightEndpoint, bridgedDimmableLightClusters);

// 删除
DataVersion gLight1DataVersions[MATTER_ARRAY_SIZE(bridgedLightClusters)];
DataVersion gLight2DataVersions[MATTER_ARRAY_SIZE(bridgedLightClusters)];
DataVersion gLight3DataVersions[MATTER_ARRAY_SIZE(bridgedLightClusters)];
DataVersion gLight4DataVersions[MATTER_ARRAY_SIZE(bridgedLightClusters)];
// 新增: 定义可调光灯设备的DataVersion存储数组。
// Matter用它来追踪每个Cluster属性的版本，实现可靠的数据同步。
DataVersion gDimmableLightDataVersions[MATTER_ARRAY_SIZE(bridgedDimmableLightClusters)];

// 新增
// DataVersion gThermostatDataVersions[MATTER_ARRAY_SIZE(bridgedThermostatClusters)];

// const EmberAfDeviceType gRootDeviceTypes[]          = { { DEVICE_TYPE_ROOT_NODE, DEVICE_VERSION_DEFAULT } };
// const EmberAfDeviceType gAggregateNodeDeviceTypes[] = { { DEVICE_TYPE_BRIDGE, DEVICE_VERSION_DEFAULT } };

// const EmberAfDeviceType gBridgedThermostatDeviceTypes[] = { { DEVICE_TYPE_THERMOSTAT, DEVICE_VERSION_DEFAULT },
//                                                        { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

/* REVISION definitions:
 */

#define ZCL_DESCRIPTOR_CLUSTER_REVISION (1u)
#define ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_REVISION (2u)
#define ZCL_FIXED_LABEL_CLUSTER_REVISION (1u)
#define ZCL_ON_OFF_CLUSTER_REVISION (4u)
// 新增: 定义 LevelControl Cluster 的协议版本号。
#define ZCL_LEVEL_CONTROL_CLUSTER_REVISION (5u)

int AddDeviceEndpoint(Device * dev, EmberAfEndpointType * ep, const Span<const EmberAfDeviceType> & deviceTypeList,
                      const Span<DataVersion> & dataVersionStorage, chip::EndpointId parentEndpointId)
{
    ChipLogProgress(DeviceLayer, "AddDeviceEndpoint: Adding device [%s], deviceTypeList.size()=%d", 
                    dev->GetName(), (int)deviceTypeList.size());
    
    // 打印设备类型信息
    for (size_t i = 0; i < deviceTypeList.size(); i++)
    {
        ChipLogProgress(DeviceLayer, "AddDeviceEndpoint: DeviceType[%d] = 0x%04lx, version=%d", 
                        (int)i, (unsigned long)deviceTypeList.data()[i].deviceId, deviceTypeList.data()[i].deviceVersion);
    }
    
    uint8_t index = 0;
    while (index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        if (NULL == gDevices[index])
        {
            gDevices[index] = dev;
            CHIP_ERROR err;
            while (true)
            {
                dev->SetEndpointId(gCurrentEndpointId);
                ChipLogProgress(DeviceLayer, "AddDeviceEndpoint: Attempting to set endpoint %d for device [%s]", 
                                gCurrentEndpointId, dev->GetName());
                
                err = emberAfSetDynamicEndpoint(index, gCurrentEndpointId, ep, dataVersionStorage, deviceTypeList, parentEndpointId);
                if (err == CHIP_NO_ERROR)
                {
                    ChipLogProgress(DeviceLayer, "Added device %s to dynamic endpoint %d (index=%d)", dev->GetName(),
                                    gCurrentEndpointId, index);
                    return index;
                }
                else if (err != CHIP_ERROR_ENDPOINT_EXISTS)
                {
                    ChipLogProgress(DeviceLayer, "AddDeviceEndpoint: Failed to add device %s, error: %" CHIP_ERROR_FORMAT, 
                                    dev->GetName(), err.Format());
                    return -1;
                }
                // Handle wrap condition
                if (++gCurrentEndpointId < gFirstDynamicEndpointId)
                {
                    gCurrentEndpointId = gFirstDynamicEndpointId;
                }
            }
        }
        index++;
    }
    ChipLogProgress(DeviceLayer, "Failed to add dynamic endpoint: No endpoints available!");
    return -1;
}

CHIP_ERROR RemoveDeviceEndpoint(Device * dev)
{
    for (uint8_t index = 0; index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT; index++)
    {
        if (gDevices[index] == dev)
        {
            // Silence complaints about unused ep when progress logging
            // disabled.
            [[maybe_unused]] EndpointId ep = emberAfClearDynamicEndpoint(index);
            gDevices[index]                = NULL;
            ChipLogProgress(DeviceLayer, "Removed device %s from dynamic endpoint %d (index=%d)", dev->GetName(), ep, index);
            return CHIP_NO_ERROR;
        }
    }
    return CHIP_ERROR_INTERNAL;
}

Protocols::InteractionModel::Status HandleReadBridgedDeviceBasicAttribute(Device * dev, chip::AttributeId attributeId,
                                                                          uint8_t * buffer, uint16_t maxReadLength)
{
    using namespace BridgedDeviceBasicInformation::Attributes;
    ChipLogProgress(DeviceLayer, "HandleReadBridgedDeviceBasicAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId,
                    maxReadLength);

    if ((attributeId == Reachable::Id) && (maxReadLength == 1))
    {
        *buffer = dev->IsReachable() ? 1 : 0;
    }
    else if ((attributeId == NodeLabel::Id) && (maxReadLength == 32))
    {
        MutableByteSpan zclNameSpan(buffer, maxReadLength);
        MakeZclCharString(zclNameSpan, dev->GetName());
    }
    else if ((attributeId == ClusterRevision::Id) && (maxReadLength == 2))
    {
        uint16_t rev = ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_REVISION;
        memcpy(buffer, &rev, sizeof(rev));
    }
    else
    {
        return Protocols::InteractionModel::Status::Failure;
    }

    return Protocols::InteractionModel::Status::Success;
}

Protocols::InteractionModel::Status HandleReadOnOffAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,
                                                             uint16_t maxReadLength)
{
    ChipLogProgress(DeviceLayer, "HandleReadOnOffAttribute: attrId=%" PRIu32 ", maxReadLength=%u, device state=%s", 
                    attributeId, maxReadLength, dev->IsOn() ? "ON" : "OFF");

    if ((attributeId == OnOff::Attributes::OnOff::Id) && (maxReadLength == 1))
    {
        uint8_t onOffValue = dev->IsOn() ? 1 : 0;
        *buffer = onOffValue;
        ChipLogProgress(DeviceLayer, "HandleReadOnOffAttribute: returning OnOff value=%d", onOffValue);
    }
    else if ((attributeId == OnOff::Attributes::ClusterRevision::Id) && (maxReadLength == 2))
    {
        uint16_t rev = ZCL_ON_OFF_CLUSTER_REVISION;
        memcpy(buffer, &rev, sizeof(rev));
        ChipLogProgress(DeviceLayer, "HandleReadOnOffAttribute: returning ClusterRevision=%d", rev);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "HandleReadOnOffAttribute: Unhandled attrId=%" PRIu32 " or invalid maxReadLength=%u", 
                        attributeId, maxReadLength);
        return Protocols::InteractionModel::Status::Failure;
    }

    return Protocols::InteractionModel::Status::Success;
}

// 新增: LevelControl 属性读取处理函数。
// 当Matter SDK收到读取亮度相关属性的请求时，会最终调用到这里。
Protocols::InteractionModel::Status HandleReadLevelControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,
                                                                  uint16_t maxReadLength)
{
    ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId,
                    maxReadLength);

    if ((attributeId == LevelControl::Attributes::CurrentLevel::Id) && (maxReadLength == 1))
    {
        // 修复：确保返回的CurrentLevel符合Matter规范
        // 当设备关闭时，CurrentLevel应该是null/undefined，但由于我们必须返回一个值，
        // 我们根据OnOff状态来决定返回什么
        uint8_t currentLevel = dev->GetLevel();
        uint8_t returnLevel = currentLevel;
        
        if (!dev->IsOn()) {
            // 当设备关闭时，根据Matter规范，CurrentLevel应该是null
            // 但由于我们必须返回一个uint8值，我们返回0（表示关闭）
            returnLevel = 0;
        } else if (currentLevel == 0) {
            // 如果设备开启但level为0，这是不一致的状态，修正为最小值1
            returnLevel = 1;
        }
        
        ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: device level=%d, device state=%s, returning CurrentLevel=%d", 
                        currentLevel, dev->IsOn() ? "ON" : "OFF", returnLevel);
        *buffer = returnLevel;
    }
    else if ((attributeId == LevelControl::Attributes::MinLevel::Id) && (maxReadLength == 1))
    {
        // 返回最小亮度值 (1) - 符合Matter规范
        *buffer = 1;
        ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: returning MinLevel=1");
    }
    else if ((attributeId == LevelControl::Attributes::MaxLevel::Id) && (maxReadLength == 1))
    {
        // 返回最大亮度值 (254)
        *buffer = 254;
        ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: returning MaxLevel=254");
    }
    else if ((attributeId == LevelControl::Attributes::ClusterRevision::Id) && (maxReadLength == 2))
    {
        // 返回 LevelControl Cluster 的版本号
        uint16_t rev = ZCL_LEVEL_CONTROL_CLUSTER_REVISION;
        memcpy(buffer, &rev, sizeof(rev));
        ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: returning ClusterRevision=%d", rev);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: Unhandled attrId=%" PRIu32 " or invalid maxReadLength=%u", 
                        attributeId, maxReadLength);
        return Protocols::InteractionModel::Status::Failure;
    }

    return Protocols::InteractionModel::Status::Success;
}

// 删除
Protocols::InteractionModel::Status HandleWriteOnOffAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    ChipLogProgress(DeviceLayer, "HandleWriteOnOffAttribute: attrId=%" PRIu32 ", buffer value=%d", attributeId, *buffer);

    VerifyOrReturnError((attributeId == OnOff::Attributes::OnOff::Id) && dev->IsReachable(),
                        Protocols::InteractionModel::Status::Failure);
    
    bool newOnOffState = (*buffer == 1);
    ChipLogProgress(DeviceLayer, "HandleWriteOnOffAttribute: Setting OnOff to %s (current state: %s)", 
                    newOnOffState ? "ON" : "OFF", dev->IsOn() ? "ON" : "OFF");
    
    dev->SetOnOff(newOnOffState);
    return Protocols::InteractionModel::Status::Success;
}

// 新增: LevelControl 属性写入处理函数。
// 参考官方lighting-app的简单处理方式
Protocols::InteractionModel::Status HandleWriteLevelControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    ChipLogProgress(DeviceLayer, "HandleWriteLevelControlAttribute: attrId=%" PRIu32 ", buffer value=%d", attributeId, *buffer);

    // 验证设备是否在线
    VerifyOrReturnError(dev->IsReachable(), Protocols::InteractionModel::Status::Failure);

    if (attributeId == LevelControl::Attributes::CurrentLevel::Id)
    {
        uint8_t level = *buffer;
        ChipLogProgress(DeviceLayer, "HandleWriteLevelControlAttribute: received level=%d (current level=%d, current state=%s)", 
                        level, dev->GetLevel(), dev->IsOn() ? "ON" : "OFF");
        
        // 🎯 关键简化：参考官方lighting-app，直接设置亮度
        // 类似于官方例程中的 AppLED.SetBrightness(*value)
        dev->SetLevel(level);
        
        ChipLogProgress(DeviceLayer, "HandleWriteLevelControlAttribute: after SetLevel - new level=%d, new state=%s", 
                        dev->GetLevel(), dev->IsOn() ? "ON" : "OFF");
        return Protocols::InteractionModel::Status::Success;
    }

    ChipLogProgress(DeviceLayer, "HandleWriteLevelControlAttribute: Unhandled attributeId=%" PRIu32, attributeId);
    return Protocols::InteractionModel::Status::Failure;
}

//新增
// Protocols::InteractionModel::Status HandleReadThermostatAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,
//     uint16_t maxReadLength)
// {
//     ESP_LOGI(TAG, "HandleReadThermostatAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId, maxReadLength);

//     switch (attributeId) {
//         case Thermostat::Attributes::LocalTemperature::Id:
//         if (maxReadLength >= 2) {
//             int16_t temp = 2500; // 25°C
//             memcpy(buffer, &temp, sizeof(temp));
//             return Protocols::InteractionModel::Status::Success;
//         }
//         break;

//         case  Thermostat::Attributes::OccupiedCoolingSetpoint::Id:
//         if (maxReadLength >= 2) {
//             int16_t setpoint = 2200; // 22°C
//             memcpy(buffer, &setpoint, sizeof(setpoint));
//             return Protocols::InteractionModel::Status::Success;
//         }
//         break;

//         case  Thermostat::Attributes::SystemMode::Id:
//         if (maxReadLength >= 1) {
//             *buffer = 1; // Cool mode
//             return Protocols::InteractionModel::Status::Success;
//         }
//         break;

//         case  Thermostat::Attributes::ThermostatRunningMode::Id:
//         if (maxReadLength >= 1) {
//             *buffer = 1; // Running
//             return Protocols::InteractionModel::Status::Success;
//         }
//         break;

//         case  Thermostat::Attributes::HVACSystemTypeConfiguration::Id:
//         if (maxReadLength >= 1) {
//             *buffer = 1; // Cooling system
//             return Protocols::InteractionModel::Status::Success;
//         }
//         break;

//         case Thermostat::Attributes::FeatureMap::Id:
//         if (maxReadLength >= 1) {  // BITMAP32 需要 4 字节
//             uint32_t featureMapValue = 0x03;  // 支持功能位（例如 bit 0 和 bit 1）
//             memcpy(buffer, &featureMapValue, sizeof(featureMapValue));
//             return Protocols::InteractionModel::Status::Success;
//         }
//         break;

//         case  Thermostat::Attributes::ClusterRevision::Id:
//         if (maxReadLength >= 2) {
//             uint16_t rev = 4; // ZCL Thermostat cluster revision
//             memcpy(buffer, &rev, sizeof(rev));
//             return Protocols::InteractionModel::Status::Success;
//         }
//         break;

//     }
//     ESP_LOGE(TAG, "Unhandled Thermostat attribute: 0x%lX", attributeId);
//     return Protocols::InteractionModel::Status::Failure;
// }

//读开关状态，状态上报
Protocols::InteractionModel::Status emberAfExternalAttributeReadCallback(EndpointId endpoint, ClusterId clusterId,
                                                                         const EmberAfAttributeMetadata * attributeMetadata,
                                                                         uint8_t * buffer, uint16_t maxReadLength)
{
    ChipLogProgress(DeviceLayer, "emberAfExternalAttributeReadCallback: endpoint=%d, clusterId=0x%lx, attrId=0x%lx, maxReadLength=%u", 
                    endpoint, (unsigned long)clusterId, (unsigned long)attributeMetadata->attributeId, maxReadLength);
    
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);
    ChipLogProgress(DeviceLayer, "emberAfExternalAttributeReadCallback: endpointIndex=%d, max_count=%d", 
                    endpointIndex, CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT);

    if ((endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT) && (gDevices[endpointIndex] != NULL))
    {
        Device * dev = gDevices[endpointIndex];
        ChipLogProgress(DeviceLayer, "emberAfExternalAttributeReadCallback: found device [%s], reachable=%d", 
                        dev->GetName(), dev->IsReachable());

        if (clusterId == BridgedDeviceBasicInformation::Id)
        {
            ChipLogProgress(DeviceLayer, "emberAfExternalAttributeReadCallback: handling BridgedDeviceBasicInformation");
            return HandleReadBridgedDeviceBasicAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
        else if (clusterId == OnOff::Id)
        {
            ChipLogProgress(DeviceLayer, "emberAfExternalAttributeReadCallback: handling OnOff cluster");
            return HandleReadOnOffAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
        // 新增: 在总的属性读取回调中，增加一个分支判断。
        else if (clusterId == LevelControl::Id)
        {
            ChipLogProgress(DeviceLayer, "emberAfExternalAttributeReadCallback: handling LevelControl cluster");
            // 如果请求的Cluster是LevelControl，则将请求分发给我们新写的处理函数。
            return HandleReadLevelControlAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
        else
        {
            ChipLogProgress(DeviceLayer, "emberAfExternalAttributeReadCallback: unhandled clusterId=0x%lx", (unsigned long)clusterId);
        }
    }
    else
    {
        ChipLogProgress(DeviceLayer, "emberAfExternalAttributeReadCallback: invalid endpointIndex=%d or null device", endpointIndex);
    }

    ChipLogProgress(DeviceLayer, "emberAfExternalAttributeReadCallback: returning Failure");
    return Protocols::InteractionModel::Status::Failure;
}

//控制开关状态
// 删除
Protocols::InteractionModel::Status emberAfExternalAttributeWriteCallback(EndpointId endpoint, ClusterId clusterId,
                                                                          const EmberAfAttributeMetadata * attributeMetadata,
                                                                          uint8_t * buffer)
{
    ChipLogProgress(DeviceLayer, "emberAfExternalAttributeWriteCallback: endpoint=%d, clusterId=0x%lx, attrId=0x%lx, buffer=0x%02x", 
                    endpoint, (unsigned long)clusterId, (unsigned long)attributeMetadata->attributeId, *buffer);
    
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);
    ChipLogProgress(DeviceLayer, "emberAfExternalAttributeWriteCallback: endpointIndex=%d", endpointIndex);

    if (endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        Device * dev = gDevices[endpointIndex];
        ChipLogProgress(DeviceLayer, "emberAfExternalAttributeWriteCallback: found device [%s], reachable=%d", 
                        dev->GetName(), dev->IsReachable());

        if ((dev->IsReachable()) && (clusterId == OnOff::Id))
        {
            ChipLogProgress(DeviceLayer, "emberAfExternalAttributeWriteCallback: handling OnOff cluster write");
            return HandleWriteOnOffAttribute(dev, attributeMetadata->attributeId, buffer);
        }
        // 恢复对LevelControl属性写入的处理
        else if ((dev->IsReachable()) && (clusterId == LevelControl::Id))
        {
            ChipLogProgress(DeviceLayer, "emberAfExternalAttributeWriteCallback: handling LevelControl cluster write");
            return HandleWriteLevelControlAttribute(dev, attributeMetadata->attributeId, buffer);
        }
        else
        {
            ChipLogProgress(DeviceLayer, "emberAfExternalAttributeWriteCallback: device not reachable or unhandled clusterId=0x%lx", 
                            (unsigned long)clusterId);
        }
    }
    else
    {
        ChipLogProgress(DeviceLayer, "emberAfExternalAttributeWriteCallback: invalid endpointIndex=%d", endpointIndex);
    }

    ChipLogProgress(DeviceLayer, "emberAfExternalAttributeWriteCallback: returning Failure");
    return Protocols::InteractionModel::Status::Failure;
}

namespace {
void CallReportingCallback(intptr_t closure)
{
    auto path = reinterpret_cast<app::ConcreteAttributePath *>(closure);
    MatterReportingAttributeChangeCallback(*path);
    Platform::Delete(path);
}

void ScheduleReportingCallback(Device * dev, ClusterId cluster, AttributeId attribute)
{
    auto * path = Platform::New<app::ConcreteAttributePath>(dev->GetEndpointId(), cluster, attribute);
    DeviceLayer::PlatformMgr().ScheduleWork(CallReportingCallback, reinterpret_cast<intptr_t>(path));
}
} // anonymous namespace

void HandleDeviceStatusChanged(Device * dev, Device::Changed_t itemChangedMask)
{
    if (itemChangedMask & Device::kChanged_Reachable)
    {
        ScheduleReportingCallback(dev, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::Reachable::Id);
    }

    if (itemChangedMask & Device::kChanged_State)
    {
        ScheduleReportingCallback(dev, OnOff::Id, OnOff::Attributes::OnOff::Id);
    }

    // 新增: 增加对亮度变化的判断。
    if (itemChangedMask & Device::kChanged_Level)
    {
        // 如果是亮度发生了变化，就调用 ScheduleReportingCallback。
        // 这个函数会通知Matter SDK，CurrentLevel 属性已经更新，
        // SDK会自动将新值"报告"给所有订阅了该属性的控制器。
        ScheduleReportingCallback(dev, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    }

    if (itemChangedMask & Device::kChanged_Name)
    {
        ScheduleReportingCallback(dev, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id);
    }
}

// sdk修改
bool emberAfActionsClusterInstantActionCallback(app::CommandHandler * commandObj, const app::ConcreteCommandPath & commandPath,
    const Actions::Commands::InstantAction::DecodableType & commandData)
{
// No actions are implemented, just return status NotFound.
commandObj->AddStatus(commandPath, Protocols::InteractionModel::Status::NotFound);
return true;
}

// 移除复杂的命令拦截机制，采用官方lighting-app的简单方式
// 让SDK正常处理所有命令，我们只在属性变化时响应
bool emberAfPreCommandReceivedCallback(const app::ConcreteCommandPath & commandPath, chip::TLV::TLVReader & aReader,
                                       app::CommandHandler * apCommandObj)
{
    // 让SDK正常处理所有命令，不进行任何拦截
    return false;
}

// const EmberAfDeviceType gRootDeviceTypes[]          = { { DEVICE_TYPE_ROOT_NODE, DEVICE_VERSION_DEFAULT } };
// const EmberAfDeviceType gAggregateNodeDeviceTypes[] = { { DEVICE_TYPE_BRIDGE, DEVICE_VERSION_DEFAULT } };

// const EmberAfDeviceType gBridgedThermostatDeviceTypes[] = { { DEVICE_TYPE_THERMOSTAT, DEVICE_VERSION_DEFAULT },
//                                                        { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gRootDeviceTypes[]          = { { DEVICE_TYPE_ROOT_NODE, DEVICE_VERSION_DEFAULT } };
const EmberAfDeviceType gAggregateNodeDeviceTypes[] = { { DEVICE_TYPE_BRIDGE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedOnOffDeviceTypes[] = { { DEVICE_TYPE_LO_ON_OFF_LIGHT, DEVICE_VERSION_DEFAULT },
                                                       { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

// 新增: 定义一个正式的设备类型列表，用于在Matter协议层面注册设备。
// 它告诉网络，这个端点既是一个"可调光灯"，也是一个"被桥接的节点"。
const EmberAfDeviceType gBridgedDimmableDeviceTypes[] = { { DEVICE_TYPE_LO_DIMMABLE_LIGHT, DEVICE_VERSION_DEFAULT },
                                                          { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

// 删除
static void InitServer(intptr_t context)
{
    PrintOnboardingCodes(chip::RendezvousInformationFlags(CONFIG_RENDEZVOUS_MODE));

    Esp32AppServer::Init(); // Init ZCL Data Model and CHIP App Server AND Initialize device attestation config

    // Set starting endpoint id where dynamic endpoints will be assigned, which
    // will be the next consecutive endpoint id after the last fixed endpoint.
    gFirstDynamicEndpointId = static_cast<chip::EndpointId>(
        static_cast<int>(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1))) + 1);
    gCurrentEndpointId = gFirstDynamicEndpointId;

    // Disable last fixed endpoint, which is used as a placeholder for all of the
    // supported clusters so that ZAP will generated the requisite code.
    emberAfEndpointEnableDisable(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1)), false);

    // A bridge has root node device type on EP0 and aggregate node device type (bridge) at EP1
    emberAfSetDeviceTypeList(0, Span<const EmberAfDeviceType>(gRootDeviceTypes));
    emberAfSetDeviceTypeList(1, Span<const EmberAfDeviceType>(gAggregateNodeDeviceTypes));

    // Add lights 1..3 --> will be mapped to ZCL endpoints 3, 4, 5
    AddDeviceEndpoint(&gLight1, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
                      Span<DataVersion>(gLight1DataVersions), 1);
    // AddDeviceEndpoint(&gLight2, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
    //                   Span<DataVersion>(gLight2DataVersions), 1);
    // AddDeviceEndpoint(&gLight3, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
    //                   Span<DataVersion>(gLight3DataVersions), 1);

    // Remove Light 2 -- Lights 1 & 3 will remain mapped to endpoints 3 & 5
    // RemoveDeviceEndpoint(&gLight2);

    // Add Light 4 -- > will be mapped to ZCL endpoint 6
    // AddDeviceEndpoint(&gLight4, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
    //                   Span<DataVersion>(gLight4DataVersions), 1);

    // Re-add Light 2 -- > will be mapped to ZCL endpoint 7
    // AddDeviceEndpoint(&gLight2, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
    //                   Span<DataVersion>(gLight2DataVersions), 1);

    // 新增: 调用 AddDeviceEndpoint，将我们的可调光灯添加到桥接器。
    // 注意这里使用了我们新定义的 bridgedDimmableLightEndpoint 模板和 gBridgedDimmableDeviceTypes 设备类型。
    AddDeviceEndpoint(&gDimmableLight, &bridgedDimmableLightEndpoint, Span<const EmberAfDeviceType>(gBridgedDimmableDeviceTypes),
                      Span<DataVersion>(gDimmableLightDataVersions), 1);
}

// 新增
// static void InitServer(intptr_t context)
// {
//     ESP_LOGE(TAG, " void InitServer(intptr_t context)");
//     PrintOnboardingCodes(chip::RendezvousInformationFlags(CONFIG_RENDEZVOUS_MODE));

//     Esp32AppServer::Init(); // Init ZCL Data Model and CHIP App Server AND Initialize device attestation config

//     // Set starting endpoint id where dynamic endpoints will be assigned, which
//     // will be the next consecutive endpoint id after the last fixed endpoint.
//     gFirstDynamicEndpointId = static_cast<chip::EndpointId>(
//         static_cast<int>(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1))) + 1);
//     gCurrentEndpointId = gFirstDynamicEndpointId;

//     // Disable last fixed endpoint, which is used as a placeholder for all of the
//     // supported clusters so that ZAP will generated the requisite code.
//     emberAfEndpointEnableDisable(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1)), false);

//     // A bridge has root node device type on EP0 and aggregate node device type (bridge) at EP1
//     emberAfSetDeviceTypeList(0, Span<const EmberAfDeviceType>(gRootDeviceTypes));
//     emberAfSetDeviceTypeList(1, Span<const EmberAfDeviceType>(gAggregateNodeDeviceTypes));

//     // Add lights 1..3 --> will be mapped to ZCL endpoints 3, 4, 5
//     AddDeviceEndpoint(&gThermostat, &bridgedThermostatEndpoint, Span<const EmberAfDeviceType>(gBridgedThermostatDeviceTypes),
//                       Span<DataVersion>(gThermostatDataVersions), 1);
// }

// sdk修改
// void emberAfActionsClusterInitCallback(EndpointId endpoint)
// {
//     VerifyOrReturn(endpoint == 1,
//                    ChipLogError(Zcl, "Actions cluster delegate is not implemented for endpoint with id %d.", endpoint));
//     VerifyOrReturn(emberAfContainsServer(endpoint, app::Clusters::Actions::Id) == true,
//                    ChipLogError(Zcl, "Endpoint %d does not support Actions cluster.", endpoint));
//     VerifyOrReturn(!sActionsDelegateImpl && !sActionsServer);

//     sActionsDelegateImpl = std::make_unique<app::Clusters::Actions::ActionsDelegateImpl>();
//     sActionsServer       = std::make_unique<app::Clusters::Actions::ActionsServer>(endpoint, *sActionsDelegateImpl.get());

//     sActionsServer->Init();
// }

extern "C" void app_main()
{
    // Initialize the ESP NVS layer.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_flash_init() failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default()  failed: %s", esp_err_to_name(err));
        return;
    }

    CHIP_ERROR chip_err = CHIP_NO_ERROR;

    // bridge will have own database named gDevices.
    // Clear database
    memset(gDevices, 0, sizeof(gDevices));

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    if (DeviceLayer::Internal::ESP32Utils::InitWiFiStack() != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to initialize the Wi-Fi stack");
        return;
    }
#endif
    // 删除
    gLight1.SetReachable(true);
    // gLight2.SetReachable(true);
    // gLight3.SetReachable(true);
    // gLight4.SetReachable(true);
    // 新增: 在启动时，将我们的可调光灯设置为"可达"状态。
    gDimmableLight.SetReachable(true);

    // 新增
    // gThermostat.SetReachable(true);

    // Whenever bridged device changes its state
    // 删除
    gLight1.SetChangeCallback(&HandleDeviceStatusChanged);
    // gLight2.SetChangeCallback(&HandleDeviceStatusChanged);
    // gLight3.SetChangeCallback(&HandleDeviceStatusChanged);
    // gLight4.SetChangeCallback(&HandleDeviceStatusChanged);
    // 新增: 为可调光灯注册状态变更回调函数。
    // 这样，当它的状态（开关、亮度等）改变时，HandleDeviceStatusChanged 就会被调用。
    gDimmableLight.SetChangeCallback(&HandleDeviceStatusChanged);

    DeviceLayer::SetDeviceInfoProvider(&gExampleDeviceInfoProvider);

    CHIPDeviceManager & deviceMgr = CHIPDeviceManager::GetInstance();

    chip_err = deviceMgr.Init(&AppCallback);
    if (chip_err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "device.Init() failed: %" CHIP_ERROR_FORMAT, chip_err.Format());
        return;
    }

#if CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER
    SetCommissionableDataProvider(&sFactoryDataProvider);
    SetDeviceAttestationCredentialsProvider(&sFactoryDataProvider);
#if CONFIG_ENABLE_ESP32_DEVICE_INSTANCE_INFO_PROVIDER
    SetDeviceInstanceInfoProvider(&sFactoryDataProvider);
#endif
#else
    SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif // CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER

    chip::DeviceLayer::PlatformMgr().ScheduleWork(InitServer, reinterpret_cast<intptr_t>(nullptr));
}
