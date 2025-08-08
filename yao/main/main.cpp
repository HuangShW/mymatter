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
 * @file main.cpp
 * @brief Matter 桥接应用入口：动态端点管理、ZCL 属性读写回调与状态上报。
 * @details 本文件负责：
 *  - 定义桥接设备、动态端点及其集群/属性
 *  - 实现外部属性读写回调（Read/Write Callback），与设备对象互相映射
 *  - 设备状态变化时调度 Attribute Reporting
 *  - 应用初始化（Server、动态端点、DAC 等）与入口 app_main
 */
#include "Device.h"
#include "DeviceCallbacks.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/reporting/reporting.h>
#include <app/util/attribute-storage.h>
#include <app/util/endpoint-config-api.h>
#include <common/Esp32AppServer.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/core/CHIPError.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CHIPMemString.h>
#include <lib/support/ZclString.h>
#include <platform/ESP32/ESP32Utils.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <app/InteractionModelEngine.h>
#include <app/server/Server.h>

#include "driver/uart.h"
#include "../kis_light/kis_lightContrl.h"
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
} // namespace

extern const char TAG[] = "bridge-app";

using namespace ::chip;
using namespace ::chip::DeviceManager;
using namespace ::chip::Platform;
using namespace ::chip::Credentials;
using namespace ::chip::app::Clusters;

/** 应用层设备回调处理器（处理通用设备事件） */
static AppDeviceCallbacks AppCallback;

/** NodeLabel ZCL 字段最大长度（字节） */
static const int kNodeLabelSize = 32;
// Current ZCL implementation of Struct uses a max-size array of 254 bytes
/** ZCL 结构体数组的最大长度（Descriptor 集群属性数组大小基线） */
static const int kDescriptorAttributeArraySize = 254;

/** 当前分配到的动态端点 ID（向后推进），与 gFirstDynamicEndpointId 配合使用 */
static EndpointId gCurrentEndpointId;
/** 第一个可用于动态分配的端点 ID（固定端点之后） */
static EndpointId gFirstDynamicEndpointId;
/** 动态端点对应的设备指针表（与 emberAfSetDynamicEndpoint 索引一致） */
static Device * gDevices[CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT]; // number of dynamic endpoints count

// 4 Bridged devices
/**
 * @brief 示例：一盏可调光/色温的灯（CT Light）。
 * @note 名称与位置会映射到 Bridged Device Basic Information 的 NodeLabel 等属性。
 */
static Device gCTLight("Light 1", "Office");

// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGED_NODE 0x0013
// (taken from lo-devices.xml)
#define DEVICE_TYPE_CT_LIG 0X010C

// (taken from chip-devices.xml)
#define DEVICE_TYPE_ROOT_NODE 0x0016
// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGE 0x000e

// Device Version for dynamic endpoints:
#define DEVICE_VERSION_DEFAULT 1

/**
 * @brief BRIDGED DEVICE ENDPOINT: 包含集群清单
 * - On/Off
 * - Level Control
 * - Color Control
 * - Descriptor
 * - Bridged Device Basic Information
 */
// Declare On/Off cluster attributes

/* 声明开关集群属性（On/Off） */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(CTLightOnOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnOff::Id, BOOLEAN, 1,MATTER_ATTRIBUTE_FLAG_WRITABLE),
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::StartUpOnOff::Id, INT8U, 1,MATTER_ATTRIBUTE_FLAG_WRITABLE),
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::FeatureMap::Id, INT8U, 1,0), 
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::ClusterRevision::Id, INT16U,2,0), 
 DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

/* 声明亮度集群属性（Level Control） */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(CTLightLevelControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::FeatureMap::Id,INT8U,1,0), 
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::CurrentLevel::Id,INT8U,1,0), 
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::ClusterRevision::Id,INT16U,2,0), 
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MinLevel::Id,INT8U,1,0), 
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MaxLevel::Id,INT8U,1,0), 
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::StartUpCurrentLevel::Id,INT8U,1,0), 
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::OnLevel::Id,INT8U,1,0), 
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::Options::Id,INT8U,1,0), 
 DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

/* 声明色温控制集群属性（Color Control，使用 Mireds 单位） */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(CTLightColorControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::FeatureMap::Id,INT8U, 1, 0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTemperatureMireds::Id,INT16U, 2,0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorMode::Id,INT8U, 1, 0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::Options::Id,INT8U, 1, 0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::NumberOfPrimaries::Id,INT8U, 1, 0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::EnhancedColorMode::Id,INT8U, 1, 0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorCapabilities::Id,INT8U, 1, 0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMinMireds::Id,INT16U, 2, 0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id,INT16U,2, 0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::StartUpColorTemperatureMireds::Id,INT16U,2, 0),
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ClusterRevision::Id,INT16U, 2, 0),
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

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
constexpr CommandId onOffIncomingCommands[] = {
    app::Clusters::OnOff::Commands::Off::Id,
    app::Clusters::OnOff::Commands::On::Id,
    app::Clusters::OnOff::Commands::Toggle::Id,
    app::Clusters::OnOff::Commands::OffWithEffect::Id,
    app::Clusters::OnOff::Commands::OnWithRecallGlobalScene::Id,
    app::Clusters::OnOff::Commands::OnWithTimedOff::Id,
    kInvalidCommandId,
};

constexpr CommandId levelControlIncomingCommands[] = {
    app::Clusters::LevelControl::Commands::Move::Id,
    app::Clusters::LevelControl::Commands::MoveToLevel::Id,
    app::Clusters::LevelControl::Commands::Step::Id,
    app::Clusters::LevelControl::Commands::Stop::Id,
    app::Clusters::LevelControl::Commands::MoveToLevelWithOnOff::Id,
    app::Clusters::LevelControl::Commands::MoveWithOnOff::Id,
    app::Clusters::LevelControl::Commands::StepWithOnOff::Id,
    app::Clusters::LevelControl::Commands::StopWithOnOff::Id,
    app::Clusters::LevelControl::Commands::MoveToClosestFrequency::Id,
    kInvalidCommandId,
};

constexpr CommandId colorControlIncomingCommands[] = {
    app::Clusters::ColorControl::Commands::MoveToColorTemperature::Id,
    app::Clusters::ColorControl::Commands::StopMoveStep::Id,
    app::Clusters::ColorControl::Commands::MoveColorTemperature::Id,
    app::Clusters::ColorControl::Commands::StepColorTemperature::Id,
    kInvalidCommandId,
};



/**
 * @brief 动态集群列表（桥接灯端点）。
 * @note 将集群与其属性列表、命令列表进行绑定。
 */
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedCTLightClusters)
    DECLARE_DYNAMIC_CLUSTER(OnOff::Id, CTLightOnOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, CTLightLevelControlAttrs, ZAP_CLUSTER_MASK(SERVER),levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, CTLightColorControlAttrs, ZAP_CLUSTER_MASK(SERVER),colorControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr) DECLARE_DYNAMIC_CLUSTER_LIST_END;
                            
// Declare Bridged Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedCTLightEndpoint, bridgedCTLightClusters);

/** 每个集群的数据版本存储（用于报告机制） */
DataVersion gLight1DataVersions[MATTER_ARRAY_SIZE(bridgedCTLightClusters)];
/** 根节点设备类型列表（EP0） */
const EmberAfDeviceType gRootDeviceTypes[]          = { { DEVICE_TYPE_ROOT_NODE, DEVICE_VERSION_DEFAULT } };
/** 汇聚节点（Bridge）设备类型列表（EP1） */
const EmberAfDeviceType gAggregateNodeDeviceTypes[] = { { DEVICE_TYPE_BRIDGE, DEVICE_VERSION_DEFAULT } };

/** 桥接灯端点的设备类型（包含自定义 CT 灯 + Bridged Node） */
const EmberAfDeviceType gBridgedOnOffDeviceTypes[] = { { DEVICE_TYPE_CT_LIG, DEVICE_VERSION_DEFAULT },
                                                       { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };
/* REVISION definitions:
 */

#define ZCL_DESCRIPTOR_CLUSTER_REVISION (1u)
#define ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_REVISION (2u)
#define ZCL_FIXED_LABEL_CLUSTER_REVISION (1u)
#define ZCL_ON_OFF_CLUSTER_REVISION (4u)

/**
 * @brief 动态添加设备到一个空闲的动态端点。
 * @param dev 设备对象
 * @param ep 端点类型描述
 * @param deviceTypeList 设备类型列表（用于 Descriptor::DeviceTypeList）
 * @param dataVersionStorage 集群数据版本存储
 * @param parentEndpointId 父端点（聚合节点）
 * @return 成功时返回动态端点索引（index），失败返回 -1。
 * @note 成功后会调用 emberAfSetDynamicEndpoint 完成端点注册，并通过 Device::AddDevice 建立 endpoint->device 映射。
 */
int AddDeviceEndpoint(Device * dev, EmberAfEndpointType * ep, const Span<const EmberAfDeviceType> & deviceTypeList,
                      const Span<DataVersion> & dataVersionStorage, chip::EndpointId parentEndpointId)
{
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
                err =
                    emberAfSetDynamicEndpoint(index, gCurrentEndpointId, ep, dataVersionStorage, deviceTypeList, parentEndpointId);
                if (err == CHIP_NO_ERROR)
                {
                    ChipLogProgress(DeviceLayer, "Added device %s to dynamic endpoint %d (index=%d)", dev->GetName(),gCurrentEndpointId, index);
                    Device::AddDevice(gCurrentEndpointId, &gCTLight);
                    return index;
                }
                else if (err != CHIP_ERROR_ENDPOINT_EXISTS)
                {
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

/**
 * @brief 移除设备所占用的动态端点。
 * @param dev 设备对象
 * @return 成功返回 CHIP_NO_ERROR，否则返回错误码。
 */
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

/**
 * @brief 读取 Bridged Device Basic Information 集群属性的适配函数。
 * @param dev 设备对象
 * @param attributeId 属性 ID
 * @param buffer 输出缓冲
 * @param maxReadLength 缓冲区最大长度
 * @return IM 状态码
 */
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

/**
 * @brief 读取 On/Off 集群属性的适配函数。
 */
Protocols::InteractionModel::Status HandleReadOnOffAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,uint16_t maxReadLength)
{
    ESP_LOGI(TAG, "HandleReadOnOffAttribute=%" PRIu32 ", maxReadLength=%u", attributeId,maxReadLength);
    switch (attributeId) {
        case OnOff::Attributes::OnOff::Id:
            if (maxReadLength >= 1) {
                *buffer = dev->IsOn() ? 1 : 0;
                return Protocols::InteractionModel::Status::Success;
            }
            break;
            
        case  OnOff::Attributes::StartUpOnOff::Id:
            if (maxReadLength >= 1) {
            *buffer = 0x07;
            return Protocols::InteractionModel::Status::Success;
            }
            break;
            
        case  OnOff::Attributes::FeatureMap::Id:
            if (maxReadLength >= 1) {
                *buffer = 1; 
                return Protocols::InteractionModel::Status::Success;
            }
            break;
            
        case  OnOff::Attributes::ClusterRevision::Id:
            if (maxReadLength >= 2) {
                uint16_t temp = ZCL_ON_OFF_CLUSTER_REVISION; 
                memcpy(buffer, &temp, sizeof(temp));
                return Protocols::InteractionModel::Status::Success;
            }
            break;
        }
        ESP_LOGI(TAG, "Unhandled: attrId=%" PRIu32 ", maxReadLength=%u", attributeId,maxReadLength);
        return Protocols::InteractionModel::Status::Failure;
}

/**
 * @brief 读取 Level Control 集群属性的适配函数。
 */
Protocols::InteractionModel::Status HandleReadLevelControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,uint16_t maxReadLength)
{
    ESP_LOGI(TAG, "HandleReadLevelControlAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId,maxReadLength);
    uint16_t temp = 0;
    switch (attributeId) {
    case LevelControl::Attributes::FeatureMap::Id:
        if (maxReadLength >= 1) {
            *buffer = 0x03;
            return Protocols::InteractionModel::Status::Success;
        }
        break;
        
    case  LevelControl::Attributes::CurrentLevel::Id:
        if (maxReadLength >= 1) {
          *buffer = dev->GetLevel(); 
          return Protocols::InteractionModel::Status::Success;
        }
        break;
@
    case LevelControl::Attributes::MinLevel::Id:
        if (maxReadLength >= 1) {
            *buffer =0x01; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
    case LevelControl::Attributes::MaxLevel::Id:
        if (maxReadLength >= 1) {
            *buffer =254; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
     case LevelControl::Attributes::StartUpCurrentLevel::Id:
        if (maxReadLength >= 1) {
            *buffer =NULL; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
     case LevelControl::Attributes::OnLevel::Id:
        if (maxReadLength >= 1) {
            *buffer =254; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
     case LevelControl::Attributes::Options::Id:
        if (maxReadLength >= 1) {
            *buffer =0x03; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
    case LevelControl::Attributes::ClusterRevision::Id:
        if (maxReadLength >= 2) {
            temp = ZCL_ON_OFF_CLUSTER_REVISION; 
            memcpy(buffer, &temp, sizeof(temp));
            return Protocols::InteractionModel::Status::Success;
        }
        break;
    }
    ESP_LOGI(TAG, "Unhandled: attrId=%" PRIu32 ", maxReadLength=%u", attributeId,maxReadLength);
    return Protocols::InteractionModel::Status::Failure;
}

/**
 * @brief 读取 Color Control 集群属性的适配函数。
 */
Protocols::InteractionModel::Status HandleReadColorControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,uint16_t maxReadLength)
{
    ESP_LOGI(TAG, "HandleReadColorControlAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId,maxReadLength);
   uint16_t temp = 0;
    switch (attributeId) {
    case ColorControl::Attributes::FeatureMap::Id:
        if (maxReadLength >= 1) {
            *buffer = 0x10;
            return Protocols::InteractionModel::Status::Success;
        }
        break;
        
    case  ColorControl::Attributes::ColorTemperatureMireds::Id:
        if (maxReadLength >= 2) {
           temp =dev->GetColorTemperature();
            memcpy(buffer, &temp, sizeof(temp));
          return Protocols::InteractionModel::Status::Success;
        }
        break;
        
    case ColorControl::Attributes::ColorMode::Id:
        if (maxReadLength >= 1) {
            *buffer =0x04; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
    case ColorControl::Attributes::Options::Id:
        if (maxReadLength >= 1) {
            *buffer =0x01; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
     case ColorControl::Attributes::NumberOfPrimaries::Id:
        if (maxReadLength >= 1) {
            *buffer =2; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
     case ColorControl::Attributes::EnhancedColorMode::Id:
        if (maxReadLength >= 1) {
            *buffer =0x04; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
     case ColorControl::Attributes::ColorCapabilities::Id:
        if (maxReadLength >= 1) {
            *buffer =0x10; 
            return Protocols::InteractionModel::Status::Success;
        }
        break;
    case ColorControl::Attributes::ColorTempPhysicalMinMireds::Id:
        if (maxReadLength >= 2) {
            temp = 153; 
            memcpy(buffer, &temp, sizeof(temp));
            return Protocols::InteractionModel::Status::Success;
        }
        break;
    case ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id:
        if (maxReadLength >= 2) {
           temp = 500; 
           memcpy(buffer, &temp, sizeof(temp));
           return Protocols::InteractionModel::Status::Success;
        }
        break;
    case ColorControl::Attributes::StartUpColorTemperatureMireds::Id:
        if (maxReadLength >=2) {
            temp = 153; 
            memcpy(buffer, &temp, sizeof(temp));
            return Protocols::InteractionModel::Status::Success;
        }
        break;

    case ColorControl::Attributes::ClusterRevision::Id:
        if (maxReadLength >= 2) {
            temp = ZCL_ON_OFF_CLUSTER_REVISION; 
            memcpy(buffer, &temp, sizeof(temp));
            return Protocols::InteractionModel::Status::Success;
        }
        break;
        
    }
    ESP_LOGI(TAG, "Unhandled: attrId=%" PRIu32 ", maxReadLength=%u", attributeId,maxReadLength);
    return Protocols::InteractionModel::Status::Failure;
}

/**
 * @brief 写入 On/Off 集群属性的适配函数。
 */
Protocols::InteractionModel::Status HandleWriteOnOffAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    VerifyOrReturnError((attributeId == OnOff::Attributes::OnOff::Id) && dev->IsReachable(), Protocols::InteractionModel::Status::Failure);
    dev->SetOnOff(*buffer);
    return Protocols::InteractionModel::Status::Success;
}
/**
 * @brief 写入 Level Control 集群属性的适配函数。
 */
Protocols::InteractionModel::Status HandleWriteLevelControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    VerifyOrReturnError((attributeId ==LevelControl::Attributes::CurrentLevel::Id) && dev->IsReachable(), Protocols::InteractionModel::Status::Failure);
    uint16_t temp = buffer[0]<<8;
    temp |= buffer[1];
    dev->SetLevel(temp);
    return Protocols::InteractionModel::Status::Success;
}
/**
 * @brief 写入 Color Control 集群属性的适配函数。
 */
Protocols::InteractionModel::Status HandleWriteColorControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    VerifyOrReturnError((attributeId == ColorControl::Attributes::ColorTemperatureMireds::Id) && dev->IsReachable(), Protocols::InteractionModel::Status::Failure);
    uint16_t temp = buffer[0]<<8;
    temp |= buffer[1];
    dev->SetColorTemperature(temp);
    return Protocols::InteractionModel::Status::Success;
}

/**
 * @brief 外部属性读取回调，按端点路由到对应设备并分派到集群处理函数。
 */
Protocols::InteractionModel::Status emberAfExternalAttributeReadCallback(EndpointId endpoint, ClusterId clusterId,
                                                                         const EmberAfAttributeMetadata * attributeMetadata,
                                                                         uint8_t * buffer, uint16_t maxReadLength)
{
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);

    if ((endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT) && (gDevices[endpointIndex] != NULL))
    {
        Device * dev = gDevices[endpointIndex];

        if (clusterId == BridgedDeviceBasicInformation::Id)
        {
            return HandleReadBridgedDeviceBasicAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
        else if (clusterId == OnOff::Id)
        {
            return HandleReadOnOffAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
        else if(clusterId == LevelControl::Id)
        {
            return HandleReadLevelControlAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
        else if(clusterId == ColorControl::Id)
        {
            return HandleReadColorControlAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
    }
    return Protocols::InteractionModel::Status::Failure;
}

/**
 * @brief 外部属性写入回调，按端点路由到对应设备并分派到集群处理函数。
 */
Protocols::InteractionModel::Status emberAfExternalAttributeWriteCallback(EndpointId endpoint, ClusterId clusterId,
                                                                          const EmberAfAttributeMetadata * attributeMetadata,
                                                                          uint8_t * buffer)
{
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);
    ESP_LOGE(TAG, "endpointIndex =%d\n\r",endpointIndex);
    if (endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        Device * dev = gDevices[endpointIndex];

        if ((dev->IsReachable()) && (clusterId == OnOff::Id))
        {
            return HandleWriteOnOffAttribute(dev, attributeMetadata->attributeId, buffer);
        }
        else if((dev->IsReachable()) && (clusterId == LevelControl::Id))
        {
            ESP_LOGE(TAG, "HandleWriteLevelControlAttribute\n\r");
            return HandleWriteLevelControlAttribute(dev, attributeMetadata->attributeId, buffer);
        }
        else if((dev->IsReachable()) && (clusterId == ColorControl::Id))
        {
            ESP_LOGE(TAG, "HandleWriteColorControlAttribute\n\r");
            return HandleWriteColorControlAttribute(dev, attributeMetadata->attributeId, buffer);
        }
    }

    return Protocols::InteractionModel::Status::Failure;
}

namespace {
/**
 * @brief 实际执行报告回调的工作函数（在 PlatformMgr 任务上下文中运行）。
 */
void CallReportingCallback(intptr_t closure)
{
    auto path = reinterpret_cast<app::ConcreteAttributePath *>(closure);
    MatterReportingAttributeChangeCallback(*path);
    Platform::Delete(path);
}

/**
 * @brief 调度属性报告回调（延迟在平台任务中执行）。
 */
void ScheduleReportingCallback(Device * dev, ClusterId cluster, AttributeId attribute)
{
    auto * path = Platform::New<app::ConcreteAttributePath>(dev->GetEndpointId(), cluster, attribute);
    DeviceLayer::PlatformMgr().ScheduleWork(CallReportingCallback, reinterpret_cast<intptr_t>(path));
}
} // anonymous namespace

/**
 * @brief 设备状态变化回调：按变更标志触发对应属性的上报。
 * @param dev 设备对象
 * @param itemChangedMask 变更掩码（Device::Changed_t，可按位或）
 */
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
    
    if (itemChangedMask & Device::kChanged_Name)
    {
        ScheduleReportingCallback(dev, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id);
    }
    if(itemChangedMask & Device::kChanged_ColorTemp)
    {
        ScheduleReportingCallback(dev,ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id); 
    }
    if(itemChangedMask & Device:: kChanged_Level)
    {
        ScheduleReportingCallback(dev,LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id); 
    }
}

/**
 * @brief 初始化 Matter Server、设置设备类型列表并添加动态端点。
 */
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
    AddDeviceEndpoint(&gCTLight, &bridgedCTLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
                      Span<DataVersion>(gLight1DataVersions), 1);
}

/**
 * @brief 应用入口：初始化 NVS/事件循环/网络、注册回调并启动 Server。
 */
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
    if (err != ESP_OK)
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
    uart1Initr();
    gCTLight.SetReachable(true);

    // Whenever bridged device changes its state
   gCTLight.SetChangeCallback(&HandleDeviceStatusChanged);

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
