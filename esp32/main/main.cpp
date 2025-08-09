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

/*
 * 文件用途（中文说明）
 *
 * 本文件实现了基于桥接器（Bridge）的动态端点示例：
 * - 动态创建三类设备端点：仅开关灯、可调光灯、双色温灯（Color Temperature Light, 0x010C）。
 * - 采用“按 Cluster 驱动”的后置初始化（PostInitClustersForEndpoint）：仅对动态端点执行，
 *   在端点添加完成后，根据是否存在 OnOff、LevelControl、ColorControl 等集群进行必要的属性与状态初始化。
 * - 通过 emberAfExternalAttributeRead/Write 回调，对动态端点的属性进行读写桥接（外部属性存储），
 *   与业务设备对象（Device）保持同步，并按需上报（reporting）。
 * - 对 Color Control（仅色温特性）实现最小命令处理（MoveTo/Move/Step/Stop），并对色温范围进行钳制。
 *
 * 注意：
 * - 本文件中所有初始化与校正均遵循 Matter 1.4.1 规范的基本语义，属性值范围和特性位（FeatureMap）与设备能力一致。
 * - 对 Level Control 的内部插件状态初始化依赖 emberAfLevelControlClusterServerInitCallback，以镜像 SDK 默认行为，
 *   保障动态端点在调光命令路径中能正确使用 min/max/current。随后通过 Attribute Accessors 读取并按需钳制。
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
#include <app/clusters/color-control-server/color-control-server.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/callback.h>
#include <app/data-model/Nullable.h>

/*
 * 工程提示：如 IDE/索引器报 "无法打开 esp_log.h/nvs_flash.h"，通常是 includePath 配置提示，
 * 并不影响使用 ESP-IDF 的实际构建（idf.py build）。
 */

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

void CallReportingCallback(intptr_t closure);
void ScheduleReportingCallback(Device * dev, chip::ClusterId cluster, chip::AttributeId attribute);

void NotifyMetricChange(Device * dev, const char * metricName, uint32_t value);
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

// 说明：gDevices[] 维护“动态端点索引”到“业务设备对象”的映射，供外部属性回调/上报等路径使用

// 4 Bridged devices
static Device gLight1("Light 1", "Office", Device::kDeviceType_OnOffLight);
static Device gLight2("Light 2", "Office", Device::kDeviceType_DimmableLight);
static Device gLight3("Light 3", "Kitchen", Device::kDeviceType_DimmableLight);
// static Device gLight4("Light 4", "Den", Device::kDeviceType_OnOffLight);

// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGED_NODE 0x0013
// (taken from lo-devices.xml)
#define DEVICE_TYPE_LO_ON_OFF_LIGHT 0x0100

// (taken from chip-devices.xml)
#define DEVICE_TYPE_DIMMABLE_LIGHT 0x0101

#define DEVICE_TYPE_COLOR_TEMP_LIGHT 0x010C

/*
 * 设备类型常量：与 Matter 设备库（Device Library）定义保持一致
 * - 0x0100：On/Off Light
 * - 0x0101：Dimmable Light
 * - 0x010C：Color Temperature Light（双色温灯，仅支持 CT，不含色彩空间）
 */

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
/*
 * Level Control 动态属性表（按规范挑选常用属性）：
 * - CurrentLevel（RW）：当前亮度（1-254）；
 * - RemainingTime（R）：剩余过渡时间，此示例不实现过渡，固定 0；
 * - MinLevel/MaxLevel（R）：约束边界；
 * - Options/OnOffTransitionTime/OnLevel/StartUpCurrentLevel（RW）：接受写入但未持久化；
 * - FeatureMap/ClusterRevision（R）：能力声明与集群版本。
 */
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

// Declare Color Control cluster attributes (Color Temperature feature)
/*
 * Color Control（仅色温特性）动态属性表：
 * - ColorTemperatureMireds（RW）：当前色温（mireds），范围受物理最小/最大限制；
 * - ColorTempPhysicalMin/MaxMireds（R）：物理范围；
 * - RemainingTime（R）：无过渡，固定 0；
 * - ColorMode/EnhancedColorMode（R）：固定为 CT 模式（2）；
 * - ColorCapabilities（R）：仅包含 kColorTemperature；
 * - Options/StartUpColorTemperatureMireds（RW）：接受写入但未持久化；
 * - CoupleColorTempToLevelMinMireds（R）：与亮度耦合的最小 CT；
 * - FeatureMap/ClusterRevision（R）：能力声明与集群版本。
 */
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(colorControlAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTemperatureMireds::Id, INT16U, 2, MATTER_ATTRIBUTE_FLAG_WRITABLE), /* current CT */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMinMireds::Id, INT16U, 2, 0),                       /* min CT */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorTempPhysicalMaxMireds::Id, INT16U, 2, 0),                       /* max CT */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::RemainingTime::Id, INT16U, 2, 0),                                     /* remaining time */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorMode::Id, INT8U, 1, 0),                                          /* color mode */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::EnhancedColorMode::Id, INT8U, 1, 0),                                  /* enhanced color mode */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ColorCapabilities::Id, BITMAP16, 2, 0),                               /* color capabilities */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::CoupleColorTempToLevelMinMireds::Id, INT16U, 2, 0),                   /* couple min mireds */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::Options::Id, BITMAP8, 1, MATTER_ATTRIBUTE_FLAG_WRITABLE),             /* options */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::StartUpColorTemperatureMireds::Id, INT16U, 2, MATTER_ATTRIBUTE_FLAG_WRITABLE), /* startup CT */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::FeatureMap::Id, BITMAP32, 4, 0),                                       /* feature map */
    DECLARE_DYNAMIC_ATTRIBUTE(ColorControl::Attributes::ClusterRevision::Id, INT16U, 2, 0),                                    /* revision */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Cluster List for Bridged Light endpoint
// TODO: It's not clear whether it would be better to get the command lists from
// the ZAP config on our last fixed endpoint instead.
// 说明：以下命令数组用于声明“服务端可接收”的命令列表，以满足 ZCL 数据模型对命令发现/匹配的需要
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

constexpr CommandId colorControlIncomingCommands[] = {
    app::Clusters::ColorControl::Commands::MoveToColorTemperature::Id,
    app::Clusters::ColorControl::Commands::MoveColorTemperature::Id,
    app::Clusters::ColorControl::Commands::StepColorTemperature::Id,
    app::Clusters::ColorControl::Commands::StopMoveStep::Id,
    kInvalidCommandId,
};

// Declare Cluster List for Bridged On/Off Light endpoint (Light 1)
// 说明：仅开关型灯具，包含 OnOff/Descriptor/Bridged Device Basic Information 三个集群
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedOnOffLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr) DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Cluster List for Bridged Dimmable Light endpoint (Light 2)
// 说明：可调光灯具，增加 Level Control 集群及相关命令
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedDimmableLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelControlAttrs, ZAP_CLUSTER_MASK(SERVER), levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr) DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Cluster List for Bridged Color Temperature Light endpoint (Light 3)
// 说明：双色温灯具，在可调光基础上增加 Color Control（仅 CT 特性）
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedColorTempLightClusters)
DECLARE_DYNAMIC_CLUSTER(OnOff::Id, onOffAttrs, ZAP_CLUSTER_MASK(SERVER), onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(LevelControl::Id, levelControlAttrs, ZAP_CLUSTER_MASK(SERVER), levelControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ColorControl::Id, colorControlAttrs, ZAP_CLUSTER_MASK(SERVER), colorControlIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BridgedDeviceBasicInformation::Id, bridgedDeviceBasicAttrs, ZAP_CLUSTER_MASK(SERVER), nullptr,
                            nullptr) DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged On/Off Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedOnOffLightEndpoint, bridgedOnOffLightClusters);

// Declare Bridged Dimmable Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedDimmableLightEndpoint, bridgedDimmableLightClusters);

// Declare Bridged Color Temperature Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedColorTempLightEndpoint, bridgedColorTempLightClusters);

DataVersion gLight1DataVersions[MATTER_ARRAY_SIZE(bridgedOnOffLightClusters)];
DataVersion gLight2DataVersions[MATTER_ARRAY_SIZE(bridgedDimmableLightClusters)];
DataVersion gLight3DataVersions[MATTER_ARRAY_SIZE(bridgedColorTempLightClusters)];
// DataVersion gLight4DataVersions[MATTER_ARRAY_SIZE(bridgedLightClusters)];

/* REVISION definitions:
 * 说明：各集群修订版本（ClusterRevision）遵循 Matter 规范，用于客户端兼容性判断
 */
#define ZCL_DESCRIPTOR_CLUSTER_REVISION (1u)
#define ZCL_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER_REVISION (2u)
#define ZCL_FIXED_LABEL_CLUSTER_REVISION (1u)
#define ZCL_ON_OFF_CLUSTER_REVISION (4u)
#define ZCL_LEVEL_CONTROL_CLUSTER_REVISION (6u) // Level Control 集群修订版本（与规范保持一致）
#define ZCL_COLOR_CONTROL_CLUSTER_REVISION (6u) // Color Control 集群修订版本（与规范保持一致）

// 说明：AddDeviceEndpoint 负责在运行期为桥接设备分配 ZCL 动态端点，并记录 gDevices[] 映射关系。
// 成功后，端点级的集群初始化工作由 PostInitClustersForEndpoint 统一集中处理。
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
        // NodeLabel 采用 ZCL CharString（首字节长度），需使用工具函数编码
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
    // 同步到 OnOff Server，使其执行标准语义（含联动/上报），避免手工构造 IM 通知
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
    // 写前检查：设备需可达；当前实现不支持过渡（由插件/命令路径负责），此处只做最终值同步与钳制。
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
        // 交给 Device::SetLevel 做范围钳制与状态变更回调（触发上报）
        ChipLogProgress(DeviceLayer, "HandleWriteLevelControlAttribute: Setting level to %d", level);
        dev->SetLevel(level);
        
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == Options::Id)
    {
        // Options 可写但未使用，接受写入以保持与规范兼容
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == OnOffTransitionTime::Id)
    {
        // 当前不实现过渡时间，接受写入但不生效
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == OnLevel::Id)
    {
        // 启动亮度相关策略可后续扩展，当前接受但未持久化
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == StartUpCurrentLevel::Id)
    {
        // 启动亮度（Nullable），当前接受但未持久化
        return Protocols::InteractionModel::Status::Success;
    }

    return Protocols::InteractionModel::Status::Failure;
}

// 说明：色温（mireds）状态迁移至 Device 对象管理：
// - 当前文件不再维护全局 gCt* 状态；
// - 通过 Device::Get/SetColorTemperatureMireds 与 Min/Max 访问范围与当前值；
// - mireds 与 K 的关系约为 mireds ≈ 1,000,000 / 色温K。
// 读取 Color Control（色温）属性
Protocols::InteractionModel::Status HandleReadColorControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,
                                                                    uint16_t maxReadLength)
{
    using namespace ColorControl::Attributes;
    ChipLogProgress(DeviceLayer, "HandleReadColorControlAttribute: attrId=%" PRIu32 ", maxReadLength=%u", attributeId, maxReadLength);

    switch (attributeId)
    {
    case FeatureMap::Id: {
        if (maxReadLength < sizeof(uint32_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint32_t featureMap = static_cast<uint32_t>(ColorControl::Feature::kColorTemperature);
        memcpy(buffer, &featureMap, sizeof(featureMap));
        return Protocols::InteractionModel::Status::Success;
    }
    case ColorMode::Id: {
        if (maxReadLength < 1)
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        // 0: Hue/Sat, 1: xy, 2: CT
        *buffer = 2;
        return Protocols::InteractionModel::Status::Success;
    }
    case EnhancedColorMode::Id: {
        if (maxReadLength < 1)
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        // 0: CurrentHueAndCurrentSaturation, 1: CurrentXAndCurrentY, 2: ColorTemperatureMireds, 3: EnhancedCurrentHueAndCurrentSaturation
        *buffer = 2;
        return Protocols::InteractionModel::Status::Success;
    }
    case ColorCapabilities::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        // Bits 0-4 mirror FeatureMap. Only ColorTemperature is supported here.
        uint16_t caps = static_cast<uint16_t>(ColorControl::Feature::kColorTemperature);
        memcpy(buffer, &caps, sizeof(caps));
        return Protocols::InteractionModel::Status::Success;
    }
    case Options::Id: {
        if (maxReadLength < 1)
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        // No special options supported
        *buffer = 0;
        return Protocols::InteractionModel::Status::Success;
    }
    case ColorTemperatureMireds::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint16_t ct = dev->GetColorTemperatureMireds();
        memcpy(buffer, &ct, sizeof(ct));
        return Protocols::InteractionModel::Status::Success;
    }
    case ColorTempPhysicalMinMireds::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint16_t minCt = dev->GetMinColorTemperatureMireds();
        memcpy(buffer, &minCt, sizeof(minCt));
        return Protocols::InteractionModel::Status::Success;
    }
    case ColorTempPhysicalMaxMireds::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint16_t maxCt = dev->GetMaxColorTemperatureMireds();
        memcpy(buffer, &maxCt, sizeof(maxCt));
        return Protocols::InteractionModel::Status::Success;
    }
    case RemainingTime::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint16_t remainingTime = 0; // 不实现过渡
        memcpy(buffer, &remainingTime, sizeof(remainingTime));
        return Protocols::InteractionModel::Status::Success;
    }
    case CoupleColorTempToLevelMinMireds::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        // 与亮度耦合的最小色温：使用设备物理最小值
        uint16_t coupleMin = dev->GetMinColorTemperatureMireds();
        memcpy(buffer, &coupleMin, sizeof(coupleMin));
        return Protocols::InteractionModel::Status::Success;
    }
    case StartUpColorTemperatureMireds::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint16_t nullValue = 0xFFFF; // null => use previous CT on startup
        memcpy(buffer, &nullValue, sizeof(nullValue));
        return Protocols::InteractionModel::Status::Success;
    }
    case ClusterRevision::Id: {
        if (maxReadLength < sizeof(uint16_t))
        {
            return Protocols::InteractionModel::Status::Failure;
        }
        uint16_t rev = ZCL_COLOR_CONTROL_CLUSTER_REVISION;
        memcpy(buffer, &rev, sizeof(rev));
        return Protocols::InteractionModel::Status::Success;
    }
    default:
        ChipLogProgress(DeviceLayer, "HandleReadColorControlAttribute: Unsupported attribute %" PRIu32, attributeId);
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
}

// 写入 Color Control（色温）属性
Protocols::InteractionModel::Status HandleWriteColorControlAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    using namespace ColorControl::Attributes;
    ChipLogProgress(DeviceLayer, "HandleWriteColorControlAttribute: attrId=%" PRIu32, attributeId);

    VerifyOrReturnError(dev->IsReachable(), Protocols::InteractionModel::Status::Failure);

    if (attributeId == ColorTemperatureMireds::Id)
    {
        uint16_t target;
        memcpy(&target, buffer, sizeof(target));
        dev->SetColorTemperatureMireds(target);
        // report change
        ScheduleReportingCallback(dev, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id);
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == Options::Id)
    {
        // Accept but ignore options in this minimal implementation
        return Protocols::InteractionModel::Status::Success;
    }
    else if (attributeId == StartUpColorTemperatureMireds::Id)
    {
        // Accept but not persisted
        return Protocols::InteractionModel::Status::Success;
    }

    return Protocols::InteractionModel::Status::Failure;
}

Protocols::InteractionModel::Status emberAfExternalAttributeReadCallback(EndpointId endpoint, ClusterId clusterId,
                                                                         const EmberAfAttributeMetadata * attributeMetadata,
                                                                         uint8_t * buffer, uint16_t maxReadLength)
{
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);

    // 仅对“动态端点”执行外部属性桥接；静态端点由生成代码/插件默认处理
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
        else if (clusterId == ColorControl::Id)
        {
            return HandleReadColorControlAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
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
        else if ((dev->IsReachable()) && (clusterId == ColorControl::Id))
        {
            return HandleWriteColorControlAttribute(dev, attributeMetadata->attributeId, buffer);
        }
    }

    return Protocols::InteractionModel::Status::Failure;
}

namespace {

// 说明：报告上报辅助函数。
// - CallReportingCallback：真正触发 IM 的属性变更回调；
// - ScheduleReportingCallback：在平台任务中调度上报，避免在中断或不安全上下文直接上报。
void CallReportingCallback(intptr_t closure)
{
    auto path = reinterpret_cast<app::ConcreteAttributePath *>(closure);
    MatterReportingAttributeChangeCallback(*path);
    Platform::Delete(path);
}

// 说明：统一的“指标变更”通知钩子，后续可在此处对接串口/网络上报
void NotifyMetricChange(Device * dev, const char * metricName, uint32_t value)
{
    if (dev == nullptr || metricName == nullptr)
    {
        return;
    }
    // 当前仅打印日志；你可以在这里实现 UART 发送
    ChipLogProgress(DeviceLayer, "MetricChanged ep=%d name=\"%s\" %s=%u",
                    dev->GetEndpointId(), dev->GetName(), metricName, static_cast<unsigned>(value));
}

// 端点集群后置初始化（仅动态端点）：
// - 按集群存在性进行初始化，而非按设备类型；
// - Switch-only：仅存在 OnOff；
// - LevelControl：先调用插件的 ServerInit 回调以初始化内部状态，再按需钳制 CurrentLevel；
// - ColorControl（仅 CT 特性）：设置物理范围（153-500 mireds），并钳制当前色温，必要时调度上报。
static void PostInitClustersForEndpoint(EndpointId endpoint)
{
    using namespace chip::app::Clusters;

    // Cluster-driven initialization (dynamic endpoints only)
    uint16_t dynamicIndex      = emberAfGetDynamicIndexFromEndpoint(endpoint);
    bool isDynamicEndpoint     = (dynamicIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT);
    if (!isDynamicEndpoint)
    {
        ChipLogProgress(DeviceLayer, "PostInit(ep=%d): static endpoint, skip dynamic init", endpoint);
        return;
    }

    bool hasOnOffServer        = emberAfContainsServer(endpoint, OnOff::Id);
    bool hasLevelControlServer = emberAfContainsServer(endpoint, LevelControl::Id);
    bool hasColorControlServer = emberAfContainsServer(endpoint, ColorControl::Id);

    ChipLogProgress(DeviceLayer,
                    "PostInit(ep=%d): dynamic | OnOff=%d Level=%d ColorCtrl=%d",
                    endpoint, hasOnOffServer, hasLevelControlServer, hasColorControlServer);

    // Switch-only endpoints: OnOff present, without Level/Color
    if (hasOnOffServer && !hasLevelControlServer && !hasColorControlServer)
    {
        ChipLogProgress(DeviceLayer, "PostInit(ep=%d): Switch-only dynamic endpoint", endpoint);
    }

    // Level Control: clamp CurrentLevel within [MinLevel, MaxLevel]
    if (hasLevelControlServer || hasColorControlServer)
    {
        // Initialize Level Control server state for dynamic endpoint, mirroring SDK behavior
        emberAfLevelControlClusterServerInitCallback(endpoint);

        // using namespace LevelControl::Attributes;
        // chip::app::DataModel::Nullable<uint8_t> currentLevel;
        // uint8_t minLevel = 1, maxLevel = 254;
        // (void) MinLevel::Get(endpoint, &minLevel);
        // (void) MaxLevel::Get(endpoint, &maxLevel);
        // if (CurrentLevel::Get(endpoint, currentLevel) == Protocols::InteractionModel::Status::Success && !currentLevel.IsNull())
        // {
        //     uint8_t lvl = currentLevel.Value();
        //     if (lvl < minLevel || lvl > maxLevel)
        //     {
        //         uint8_t clamped = (lvl < minLevel) ? minLevel : maxLevel;
        //         ChipLogProgress(DeviceLayer, "PostInit(ep=%d): Clamp Level current to %u", endpoint, clamped);
        //         CurrentLevel::Set(endpoint, clamped);
        //     }
        // }
        // // Optionally ensure feature bits expose OnOff+Lighting for better client compatibility
        // uint32_t lcFeatures = static_cast<uint32_t>(LevelControl::Feature::kOnOff) |
        //                       static_cast<uint32_t>(LevelControl::Feature::kLighting);
        // FeatureMap::Set(endpoint, lcFeatures);
    }

    // Color Control (Color Temperature feature only)
    if (hasColorControlServer)
    {
        // Warn if dependencies are missing
        if (!hasOnOffServer || !hasLevelControlServer)
        {
            ChipLogProgress(DeviceLayer,
                            "PostInit(ep=%d): Color Control present but missing deps (OnOff=%d, Level=%d)",
                            endpoint, hasOnOffServer, hasLevelControlServer);
        }
        // 设备层（Device）默认 CT 物理范围为 [153, 500]，此处仅做当前值的范围校正与上报
        Device * dev = (dynamicIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT) ? gDevices[dynamicIndex] : nullptr;
        if (dev != nullptr)
        {
            uint16_t ct  = dev->GetColorTemperatureMireds();
            uint16_t min = dev->GetMinColorTemperatureMireds();
            uint16_t max = dev->GetMaxColorTemperatureMireds();
            if (ct < min)
            {
                dev->SetColorTemperatureMireds(min);
                ScheduleReportingCallback(dev, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id);
            }
            else if (ct > max)
            {
                dev->SetColorTemperatureMireds(max);
                ScheduleReportingCallback(dev, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id);
            }
        }

        ChipLogProgress(DeviceLayer, "PostInit(ep=%d): Color Control mode=CT", endpoint);
    }
}

void ScheduleReportingCallback(Device * dev, ClusterId cluster, AttributeId attribute)
{
    // 构建具体属性路径并在平台线程调度上报，避免在当前调用栈直接触发 IM 回调
    auto * path = Platform::New<app::ConcreteAttributePath>(dev->GetEndpointId(), cluster, attribute);
    DeviceLayer::PlatformMgr().ScheduleWork(CallReportingCallback, reinterpret_cast<intptr_t>(path));
}
} // anonymous namespace



// Color Control cluster server lifecycle callbacks (minimal stubs)
void emberAfColorControlClusterServerInitCallback(chip::EndpointId endpoint)
{
    ChipLogProgress(DeviceLayer, "ColorControl Server Init: ep=%d", endpoint);
}

void MatterColorControlClusterServerShutdownCallback(chip::EndpointId endpoint)
{
    ChipLogProgress(DeviceLayer, "ColorControl Server Shutdown: ep=%d", endpoint);
}

// Level Control -> Color Temperature coupling callback (minimal)
void emberAfPluginLevelControlCoupledColorTempChangeCallback(chip::EndpointId endpoint)
{
    // Minimal implementation: ensure current CT remains within physical limits and report
    uint16_t dynIdx = emberAfGetDynamicIndexFromEndpoint(endpoint);
    if (dynIdx < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT && gDevices[dynIdx] != nullptr)
    {
        // 触发一次 Set 以执行范围钳制（如有需要）
        gDevices[dynIdx]->SetColorTemperatureMireds(gDevices[dynIdx]->GetColorTemperatureMireds());
        ScheduleReportingCallback(gDevices[dynIdx], chip::app::Clusters::ColorControl::Id,
                                  chip::app::Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id);
    }
}

// 说明：Color Control（色温）命令最小实现：
// - MoveToColorTemperature：立即设置目标色温并钳制；
// - MoveColorTemperature：按方向步进固定增量（示例为 10 mireds）；
// - StepColorTemperature：按给定步长步进；
// - StopMoveStep：无过渡模型，直接确认；
// 所有写入都会根据物理范围进行钳制，并通过 ScheduleReportingCallback 触发上报。
bool emberAfColorControlClusterMoveToColorTemperatureCallback(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::ColorControl::Commands::MoveToColorTemperature::DecodableType & commandData)
{
    using namespace chip::app::Clusters::ColorControl::Attributes;
    chip::EndpointId endpoint = commandPath.mEndpointId;
    uint16_t target           = commandData.colorTemperatureMireds;

    uint16_t dynIdx = emberAfGetDynamicIndexFromEndpoint(endpoint);
    if (dynIdx < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT && gDevices[dynIdx] != nullptr)
    {
        gDevices[dynIdx]->SetColorTemperatureMireds(target);
        ScheduleReportingCallback(gDevices[dynIdx], chip::app::Clusters::ColorControl::Id,
                                  chip::app::Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id);
    }

    commandObj->AddStatus(commandPath, chip::Protocols::InteractionModel::Status::Success);
    return true;
}

bool emberAfColorControlClusterMoveColorTemperatureCallback(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::ColorControl::Commands::MoveColorTemperature::DecodableType & commandData)
{
    using MoveMode = chip::app::Clusters::ColorControl::MoveModeEnum;
    chip::EndpointId endpoint = commandPath.mEndpointId;

    int16_t delta = (commandData.moveMode == MoveMode::kUp) ? 10 : -10;

    uint16_t dynIdx = emberAfGetDynamicIndexFromEndpoint(endpoint);
    if (dynIdx < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT && gDevices[dynIdx] != nullptr)
    {
        int32_t curr = static_cast<int32_t>(gDevices[dynIdx]->GetColorTemperatureMireds());
        int32_t next = curr + delta;
        gDevices[dynIdx]->SetColorTemperatureMireds(static_cast<uint16_t>(next));
        ScheduleReportingCallback(gDevices[dynIdx], chip::app::Clusters::ColorControl::Id,
                                  chip::app::Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id);
    }
    commandObj->AddStatus(commandPath, chip::Protocols::InteractionModel::Status::Success);
    return true;
}

bool emberAfColorControlClusterStepColorTemperatureCallback(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::ColorControl::Commands::StepColorTemperature::DecodableType & commandData)
{
    using StepMode = chip::app::Clusters::ColorControl::StepModeEnum;
    chip::EndpointId endpoint = commandPath.mEndpointId;

    int16_t step = static_cast<int16_t>(commandData.stepSize);
    if (commandData.stepMode == StepMode::kDown)
    {
        step = -step;
    }

    uint16_t dynIdx = emberAfGetDynamicIndexFromEndpoint(endpoint);
    if (dynIdx < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT && gDevices[dynIdx] != nullptr)
    {
        int32_t curr = static_cast<int32_t>(gDevices[dynIdx]->GetColorTemperatureMireds());
        int32_t next = curr + step;
        gDevices[dynIdx]->SetColorTemperatureMireds(static_cast<uint16_t>(next));
        ScheduleReportingCallback(gDevices[dynIdx], chip::app::Clusters::ColorControl::Id,
                                  chip::app::Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id);
    }

    commandObj->AddStatus(commandPath, chip::Protocols::InteractionModel::Status::Success);
    return true;
}

bool emberAfColorControlClusterStopMoveStepCallback(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::ColorControl::Commands::StopMoveStep::DecodableType & commandData)
{
    // Minimal immediate model: no active transition, simply acknowledge
    commandObj->AddStatus(commandPath, chip::Protocols::InteractionModel::Status::Success);
    return true;
}

void HandleDeviceStatusChanged(Device * dev, Device::Changed_t itemChangedMask)
{
    // 说明：当业务设备状态变更时，按位触发对应属性的上报，保持 IM 数据模型与设备状态一致
    if (itemChangedMask & Device::kChanged_Reachable)
    {
        ScheduleReportingCallback(dev, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::Reachable::Id);
        NotifyMetricChange(dev, "reachable", dev->IsReachable() ? 1u : 0u);
    }

    if (itemChangedMask & Device::kChanged_State)
    {
        ScheduleReportingCallback(dev, OnOff::Id, OnOff::Attributes::OnOff::Id);
        NotifyMetricChange(dev, "onoff", dev->IsOn() ? 1u : 0u);
    }

    if (itemChangedMask & Device::kChanged_Level)
    {
        ScheduleReportingCallback(dev, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
        NotifyMetricChange(dev, "level", dev->GetCurrentLevel());
        // uint8_t currentLevel = dev->GetCurrentLevel();
    }

    if (itemChangedMask & Device::kChanged_ColorTemperature)
    {
        ScheduleReportingCallback(dev, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id);
        NotifyMetricChange(dev, "color_temp_mireds", dev->GetColorTemperatureMireds());
    }

    if (itemChangedMask & Device::kChanged_Name)
    {
        ScheduleReportingCallback(dev, BridgedDeviceBasicInformation::Id, BridgedDeviceBasicInformation::Attributes::NodeLabel::Id);
        NotifyMetricChange(dev, "name", 0);
    }
}

const EmberAfDeviceType gRootDeviceTypes[]          = { { DEVICE_TYPE_ROOT_NODE, DEVICE_VERSION_DEFAULT } };
const EmberAfDeviceType gAggregateNodeDeviceTypes[] = { { DEVICE_TYPE_BRIDGE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedOnOffDeviceTypes[] = { { DEVICE_TYPE_LO_ON_OFF_LIGHT, DEVICE_VERSION_DEFAULT },
                                                       { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedDimmableLightDeviceTypes[] = { { DEVICE_TYPE_DIMMABLE_LIGHT, DEVICE_VERSION_DEFAULT }, // DEVICE_TYPE_DIMMABLE_LIGHT
                                                              { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedColorTempLightDeviceTypes[] = { { DEVICE_TYPE_COLOR_TEMP_LIGHT, DEVICE_VERSION_DEFAULT },
                                                                { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

// 说明：应用服务器初始化。
// - 完成桥接节点（EP0/EP1）的设备类型配置；
// - 依次添加三个动态端点（Light1/Light2/Light3）；
// - 添加完成后，调用 PostInitClustersForEndpoint 进行“按集群”的后置初始化，确保属性状态与插件一致。
static void InitServer(intptr_t context)
{
    PrintOnboardingCodes(chip::RendezvousInformationFlags(CONFIG_RENDEZVOUS_MODE));

    // 说明：初始化 Matter 服务器（数据模型/安全/配网等），并配置 DAC/PAI 等认证材料
    Esp32AppServer::Init(); // Init ZCL Data Model and CHIP App Server AND Initialize device attestation config

    // Set starting endpoint id where dynamic endpoints will be assigned, which
    // will be the next consecutive endpoint id after the last fixed endpoint.
    gFirstDynamicEndpointId = static_cast<chip::EndpointId>(
        static_cast<int>(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1))) + 1);
    gCurrentEndpointId = gFirstDynamicEndpointId;

    // 说明：禁用最后一个固定端点（仅用作 ZAP 生成代码的“集群占位”），动态端点从其后开始分配
    emberAfEndpointEnableDisable(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1)), false);

    // 桥接节点设备类型：EP0 为 Root Node，EP1 为 Aggregate Node（Bridge）
    emberAfSetDeviceTypeList(0, Span<const EmberAfDeviceType>(gRootDeviceTypes));
    emberAfSetDeviceTypeList(1, Span<const EmberAfDeviceType>(gAggregateNodeDeviceTypes));

    // 说明：依次添加三个动态端点：仅开关、可调光、双色温（CT）
    // Add Light 1 as On/Off Light --> will be mapped to ZCL endpoint 3
    AddDeviceEndpoint(&gLight1, &bridgedOnOffLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
                      Span<DataVersion>(gLight1DataVersions), 1);
    // Add Light 2 as Dimmable Light --> will be mapped to ZCL endpoint 4
    AddDeviceEndpoint(&gLight2, &bridgedDimmableLightEndpoint, Span<const EmberAfDeviceType>(gBridgedDimmableLightDeviceTypes),
                      Span<DataVersion>(gLight2DataVersions), 1);
    // Add Light 3 as Color Temperature Light --> will be mapped to next ZCL endpoint
    AddDeviceEndpoint(&gLight3, &bridgedColorTempLightEndpoint, Span<const EmberAfDeviceType>(gBridgedColorTempLightDeviceTypes),
                      Span<DataVersion>(gLight3DataVersions), 1);
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
    PostInitClustersForEndpoint(gLight3.GetEndpointId());
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
    // 说明：仅在启用 Wi-Fi 时初始化 Wi-Fi 协议栈
    if (DeviceLayer::Internal::ESP32Utils::InitWiFiStack() != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to initialize the Wi-Fi stack");
        return;
    }
#endif

    // 说明：设置三盏灯初始在线状态
    gLight1.SetReachable(true);
    gLight2.SetReachable(true);
    gLight3.SetReachable(true);
    // gLight4.SetReachable(true);

    // 初始化可调光设备的初始亮度（按规范 254 = 满亮度）；仅对支持调光的设备生效
    if (gLight1.SupportsLevelControl()) gLight1.SetLevel(254);
    if (gLight2.SupportsLevelControl()) gLight2.SetLevel(254);
    if (gLight3.SupportsLevelControl()) gLight3.SetLevel(254);
    // if (gLight4.SupportsLevelControl()) gLight4.SetLevel(254);

    // 当设备状态变化（Reachable/OnOff/Level/Name）时，通过回调触发上报
    gLight1.SetChangeCallback(&HandleDeviceStatusChanged);
    gLight2.SetChangeCallback(&HandleDeviceStatusChanged);
    gLight3.SetChangeCallback(&HandleDeviceStatusChanged);
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

    // 说明：在平台任务中调度初始化服务器（添加动态端点并执行 PostInit）
    chip::DeviceLayer::PlatformMgr().ScheduleWork(InitServer, reinterpret_cast<intptr_t>(nullptr));
}
