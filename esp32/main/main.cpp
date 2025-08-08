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
#include <bridged-actions-stub.h>
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
#include <app/clusters/level-control/level-control.h>
#include <app/clusters/on-off-server/on-off-server.h>

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

std::unique_ptr<chip::app::Clusters::Actions::ActionsDelegateImpl> sActionsDelegateImpl;
std::unique_ptr<chip::app::Clusters::Actions::ActionsServer> sActionsServer;
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
static Device gLight1("Light 1", "Office", Device::kDeviceType_OnOffLight);
static Device gLight2("Light 2", "Office", Device::kDeviceType_DimmableLight);
// static Device gLight3("Light 3", "Kitchen", Device::kDeviceType_OnOffLight);
// static Device gLight4("Light 4", "Den", Device::kDeviceType_OnOffLight);

// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGED_NODE 0x0013
// (taken from lo-devices.xml)
#define DEVICE_TYPE_LO_ON_OFF_LIGHT 0x0100

// (taken from chip-devices.xml)
#define DEVICE_TYPE_DIMMABLE_LIGHT 0x0101

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
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(onOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::OnOff::Id, BOOLEAN, 1, 0), /* on/off */
DECLARE_DYNAMIC_ATTRIBUTE(OnOff::Attributes::ClusterRevision::Id, INT16U, 2, 0), // 集群修订版本（只读）
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

// Declare Level Control cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(levelControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::CurrentLevel::Id, INT8U, 1, MATTER_ATTRIBUTE_FLAG_WRITABLE),         /* current level */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::RemainingTime::Id, INT16U, 2, 0),       /* remaining time */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MinLevel::Id, INT8U, 1, 0),             /* min level */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::MaxLevel::Id, INT8U, 1, 0),             /* max level */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::Options::Id, BITMAP8, 1, MATTER_ATTRIBUTE_FLAG_WRITABLE),            /* options */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::OnOffTransitionTime::Id, INT16U, 2, MATTER_ATTRIBUTE_FLAG_WRITABLE), /* on/off transition time */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::OnLevel::Id, INT8U, 1, MATTER_ATTRIBUTE_FLAG_WRITABLE),              /* on level */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::StartUpCurrentLevel::Id, INT8U, 1, MATTER_ATTRIBUTE_FLAG_WRITABLE),  /* 启动亮度（可为空），可写 */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),        /* feature map */
    DECLARE_DYNAMIC_ATTRIBUTE(LevelControl::Attributes::ClusterRevision::Id, INT16U, 2, 0),     /* 集群修订版本（只读） */
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
    app::Clusters::LevelControl::Commands::MoveToLevel::Id,
    app::Clusters::LevelControl::Commands::Move::Id,
    app::Clusters::LevelControl::Commands::Step::Id,
    app::Clusters::LevelControl::Commands::Stop::Id,
    app::Clusters::LevelControl::Commands::MoveToLevelWithOnOff::Id,
    app::Clusters::LevelControl::Commands::MoveWithOnOff::Id,
    app::Clusters::LevelControl::Commands::StepWithOnOff::Id,
    app::Clusters::LevelControl::Commands::StopWithOnOff::Id,
    kInvalidCommandId,
};

// Declare Cluster List for Bridged On/Off Light endpoint (Light 1)
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedOnOffLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr) DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Cluster List for Bridged Dimmable Light endpoint (Light 2)
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedDimmableLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelControlAttrs, ZAP_CLUSTER_MASK(SERVER), levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr) DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged On/Off Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedOnOffLightEndpoint, bridgedOnOffLightClusters);

// Declare Bridged Dimmable Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedDimmableLightEndpoint, bridgedDimmableLightClusters);

DataVersion gLight1DataVersions[MATTER_ARRAY_SIZE(bridgedOnOffLightClusters)];
DataVersion gLight2DataVersions[MATTER_ARRAY_SIZE(bridgedDimmableLightClusters)];
// DataVersion gLight3DataVersions[MATTER_ARRAY_SIZE(bridgedLightClusters)];
// DataVersion gLight4DataVersions[MATTER_ARRAY_SIZE(bridgedLightClusters)];

/* REVISION definitions:
 */

#define ZCL_DESCRIPTOR_CLUSTER_REVISION (1u)
#define ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_REVISION (2u)
#define ZCL_FIXED_LABEL_CLUSTER_REVISION (1u)
#define ZCL_ON_OFF_CLUSTER_REVISION (4u)
#define ZCL_LEVEL_CONTROL_CLUSTER_REVISION (6u) // Level Control 集群修订版本（与规范保持一致）

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
                    ChipLogProgress(DeviceLayer, "Added device %s to dynamic endpoint %d (index=%d)", dev->GetName(),
                                    gCurrentEndpointId, index);

                    // 取消在此处直接初始化 Level Control，改为统一集中初始化
                    // if (dev->SupportsLevelControl())
                    // {
                    //     emberAfPluginLevelControlClusterServerPostInitCallback(gCurrentEndpointId);
                    // }

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
    ChipLogProgress(DeviceLayer, "HandleReadOnOffAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId, maxReadLength);

    if ((attributeId == OnOff::Attributes::OnOff::Id) && (maxReadLength >= 1))
    {
        *buffer = dev->IsOn() ? 1 : 0;
    }
    else if ((attributeId == OnOff::Attributes::ClusterRevision::Id) && (maxReadLength >= 2))
    {
        uint16_t rev = ZCL_ON_OFF_CLUSTER_REVISION;
        memcpy(buffer, &rev, sizeof(rev));
    }
    else
    {
        return Protocols::InteractionModel::Status::Failure;
    }

    return Protocols::InteractionModel::Status::Success;
}

Protocols::InteractionModel::Status HandleWriteOnOffAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    ChipLogProgress(DeviceLayer, "HandleWriteOnOffAttribute: attrId=%" PRIu32, attributeId);

    VerifyOrReturnError((attributeId == OnOff::Attributes::OnOff::Id) && dev->IsReachable(),
                        Protocols::InteractionModel::Status::Failure);

    bool turnOn = (*buffer == 1);
    // 先同步业务设备状态，确保外部读取一致
    dev->SetOnOff(turnOn);
    // 同步到 OnOff Server（触发集群语义联动/上报）
    ::OnOffServer::Instance().setOnOffValue(
        dev->GetEndpointId(),
        turnOn ? chip::app::Clusters::OnOff::Commands::On::Id : chip::app::Clusters::OnOff::Commands::Off::Id,
        /* initiatedByLevelChange */ false);

    return Protocols::InteractionModel::Status::Success;
}

// 读取 Level Control 属性：使用 switch-case 简化分支，严格遵循 Matter Level Control 语义
Protocols::InteractionModel::Status HandleReadLevelControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,
                                                                   uint16_t maxReadLength)
{
    // 每个 case 覆盖一个标准属性；若缓冲区长度不足则返回 Failure；未知属性返回 UnsupportedAttribute
    using namespace LevelControl::Attributes;
    ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId, maxReadLength);

    switch (attributeId)
    {
    case FeatureMap::Id: {
        if (maxReadLength < sizeof(uint32_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint32_t featureMap = static_cast<uint32_t>(LevelControl::Feature::kOnOff) |
                               static_cast<uint32_t>(LevelControl::Feature::kLighting);
        memcpy(buffer, &featureMap, sizeof(featureMap));
        return Protocols::InteractionModel::Status::Success;
    }

    case CurrentLevel::Id: {
        if (maxReadLength < 1)
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint8_t currentLevel = dev->GetCurrentLevel();
        *buffer              = currentLevel;
        ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: CurrentLevel=%d", currentLevel);
        return Protocols::InteractionModel::Status::Success;
    }

    case RemainingTime::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint16_t remainingTime = 0; // No transitions supported
        memcpy(buffer, &remainingTime, sizeof(remainingTime));
        return Protocols::InteractionModel::Status::Success;
    }

    case MinLevel::Id: {
        if (maxReadLength < 1)
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        *buffer = dev->GetMinLevel();
        return Protocols::InteractionModel::Status::Success;
    }

    case MaxLevel::Id: {
        if (maxReadLength < 1)
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        *buffer = dev->GetMaxLevel();
        return Protocols::InteractionModel::Status::Success;
    }

    case Options::Id: {
        if (maxReadLength < 1)
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        *buffer = 0; // No special options supported
        return Protocols::InteractionModel::Status::Success;
    }

    case OnOffTransitionTime::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint16_t transitionTime = 0; // No transition supported
        memcpy(buffer, &transitionTime, sizeof(transitionTime));
        ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: OnOffTransitionTime=0");
        return Protocols::InteractionModel::Status::Success;
    }

    case OnLevel::Id: {
        if (maxReadLength < 1)
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        *buffer = 0xFF; // null => use previous level
        return Protocols::InteractionModel::Status::Success;
    }

    case StartUpCurrentLevel::Id: {
        if (maxReadLength < 1)
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        *buffer = 0xFF; // null => use previous level on startup
        return Protocols::InteractionModel::Status::Success;
    }

    case ClusterRevision::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint16_t rev = ZCL_LEVEL_CONTROL_CLUSTER_REVISION;
        memcpy(buffer, &rev, sizeof(rev));
        return Protocols::InteractionModel::Status::Success;
    }
    default:
        ChipLogProgress(DeviceLayer, "HandleReadLevelControlAttribute: Unsupported attribute %" PRIu32, attributeId);
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
}

// 写入 Level Control 属性：支持 CurrentLevel/Options/OnOffTransitionTime/OnLevel/StartUpCurrentLevel 的写入
Protocols::InteractionModel::Status HandleWriteLevelControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    using namespace LevelControl::Attributes;
    // 设备需可达；CurrentLevel 会在 [1,254] 内由设备层最终钳制；其余写入目前接受但未持久化
    ChipLogProgress(DeviceLayer, "HandleWriteLevelControlAttribute: attrId=%" PRIu32, attributeId);

    VerifyOrReturnError(dev->IsReachable(), Protocols::InteractionModel::Status::Failure);
    
    if (attributeId == CurrentLevel::Id)
    {
        uint8_t level = *buffer;
        ChipLogProgress(DeviceLayer, "HandleWriteLevelControlAttribute: Received level %d", level);

        if (level > 254)
        {
            ChipLogProgress(DeviceLayer, "HandleWriteLevelControlAttribute: Invalid level %d", level);
            return Protocols::InteractionModel::Status::ConstraintError;
        }
        ChipLogProgress(DeviceLayer, "HandleWriteLevelControlAttribute: Setting level to %d", level);
        dev->SetLevel(level);
        
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == Options::Id)
    {
        // Options attribute is writable but we don't support any options
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == OnOffTransitionTime::Id)
    {
        // OnOffTransitionTime is writable but we don't support transitions
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == OnLevel::Id)
    {
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == StartUpCurrentLevel::Id)
    {
        return Protocols::InteractionModel::Status::Success;
    }

    return Protocols::InteractionModel::Status::Failure;
}

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
        else if (clusterId == LevelControl::Id)
        {
            return HandleReadLevelControlAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
    }
    return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status emberAfExternalAttributeWriteCallback(EndpointId endpoint, ClusterId clusterId,
                                                                          const EmberAfAttributeMetadata * attributeMetadata,
                                                                          uint8_t * buffer)
{
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);

    if (endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        Device * dev = gDevices[endpointIndex];

        if ((dev->IsReachable()) && (clusterId == OnOff::Id))
        {
            return HandleWriteOnOffAttribute(dev, attributeMetadata->attributeId, buffer);
        }
        else if ((dev->IsReachable()) && (clusterId == LevelControl::Id))
        {
            return HandleWriteLevelControlAttribute(dev, attributeMetadata->attributeId, buffer);
        }
    }

    return Protocols::InteractionModel::Status::Failure;
}

namespace {
void CallReportingCallback(intptr_t closure)
{
    auto path = reinterpret_cast<app::ConcreteAttributePath *>(closure);
    MatterReportingAttributeChangeCallback(*path);
    Platform::Delete(path);
}

// 统一集中初始化：对包含 Level Control 集群的端点执行插件后置初始化并设置必要的属性
static void PostInitClustersForEndpoint(EndpointId endpoint)
{
    using namespace chip::app::Clusters;

    // Level Control
    if (emberAfContainsServer(endpoint, LevelControl::Id))
    {
        ChipLogProgress(DeviceLayer, "PostInit: Level Control on ep=%d", endpoint);
        emberAfPluginLevelControlClusterServerPostInitCallback(endpoint);
        // 其余属性默认值由 ZAP/插件负责，无需在此写入
    }
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

    if (itemChangedMask & Device::kChanged_Level)
    {
        ScheduleReportingCallback(dev, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
        // uint8_t currentLevel = dev->GetCurrentLevel();
    }

    if (itemChangedMask & Device::kChanged_Name)
    {
        ScheduleReportingCallback(dev, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id);
    }
}

const EmberAfDeviceType gRootDeviceTypes[]          = { { DEVICE_TYPE_ROOT_NODE, DEVICE_VERSION_DEFAULT } };
const EmberAfDeviceType gAggregateNodeDeviceTypes[] = { { DEVICE_TYPE_BRIDGE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedOnOffDeviceTypes[] = { { DEVICE_TYPE_LO_ON_OFF_LIGHT, DEVICE_VERSION_DEFAULT },
                                                       { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedDimmableLightDeviceTypes[] = { { DEVICE_TYPE_DIMMABLE_LIGHT, DEVICE_VERSION_DEFAULT }, // DEVICE_TYPE_DIMMABLE_LIGHT
                                                              { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

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

    // Add Light 1 as On/Off Light --> will be mapped to ZCL endpoint 3
    AddDeviceEndpoint(&gLight1, &bridgedOnOffLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
                      Span<DataVersion>(gLight1DataVersions), 1);
    // Add Light 2 as Dimmable Light --> will be mapped to ZCL endpoint 4
    AddDeviceEndpoint(&gLight2, &bridgedDimmableLightEndpoint, Span<const EmberAfDeviceType>(gBridgedDimmableLightDeviceTypes),
                      Span<DataVersion>(gLight2DataVersions), 1);
    // AddDeviceEndpoint(&gLight3, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
    //                   Span<DataVersion>(gLight3DataVersions), 1);
    // // Remove Light 2 -- Lights 1 & 3 will remain mapped to endpoints 3 & 5
    // RemoveDeviceEndpoint(&gLight2);

    // // Add Light 4 -- > will be mapped to ZCL endpoint 6
    // AddDeviceEndpoint(&gLight4, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
    //                   Span<DataVersion>(gLight4DataVersions), 1);

    // // Re-add Light 2 -- > will be mapped to ZCL endpoint 7
    // AddDeviceEndpoint(&gLight2, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
    //                   Span<DataVersion>(gLight2DataVersions), 1);
                      // 集中 PostInit：对已添加端点执行必要的集群初始化
    PostInitClustersForEndpoint(gLight1.GetEndpointId());
    PostInitClustersForEndpoint(gLight2.GetEndpointId());
}

void emberAfActionsClusterInitCallback(EndpointId endpoint)
{
    VerifyOrReturn(endpoint == 1,
                   ChipLogError(Zcl, "Actions cluster delegate is not implemented for endpoint with id %d.", endpoint));
    VerifyOrReturn(emberAfContainsServer(endpoint, app::Clusters::Actions::Id) == true,
                   ChipLogError(Zcl, "Endpoint %d does not support Actions cluster.", endpoint));
    VerifyOrReturn(!sActionsDelegateImpl && !sActionsServer);

    sActionsDelegateImpl = std::make_unique<app::Clusters::Actions::ActionsDelegateImpl>();
    sActionsServer       = std::make_unique<app::Clusters::Actions::ActionsServer>(endpoint, *sActionsDelegateImpl.get());

    sActionsServer->Init();
}

// Switch cluster plugin server init callback
void MatterSwitchPluginServerInitCallback()
{
    ChipLogProgress(DeviceLayer, "Switch cluster plugin server init callback");
    // Switch cluster doesn't require special initialization for bridge applications
    // The cluster attributes are handled through the standard attribute access mechanisms
}

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

    gLight1.SetReachable(true);
    gLight2.SetReachable(true);
    // gLight3.SetReachable(true);
    // gLight4.SetReachable(true);

    // Set initial Level values for dimmable lights (254 = full brightness)
    if (gLight1.SupportsLevelControl()) gLight1.SetLevel(254);
    if (gLight2.SupportsLevelControl()) gLight2.SetLevel(254);
    // if (gLight3.SupportsLevelControl()) gLight3.SetLevel(254);
    // if (gLight4.SupportsLevelControl()) gLight4.SetLevel(254);

    // Whenever bridged device changes its state
    gLight1.SetChangeCallback(&HandleDeviceStatusChanged);
    gLight2.SetChangeCallback(&HandleDeviceStatusChanged);
    // gLight3.SetChangeCallback(&HandleDeviceStatusChanged);
    // gLight4.SetChangeCallback(&HandleDeviceStatusChanged);

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
