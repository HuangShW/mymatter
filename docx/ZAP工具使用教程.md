# ZAP (ZCL Advanced Platform) 工具使用教程

## 目录
1. [ZAP工具简介](#zap工具简介)
2. [安装与配置](#安装与配置)
3. [ZAP文件结构](#zap文件结构)
4. [基本使用方法](#基本使用方法)
5. [集群配置](#集群配置)
6. [属性和命令配置](#属性和命令配置)
7. [代码生成](#代码生成)
8. [高级功能](#高级功能)
9. [常见问题解决](#常见问题解决)
10. [最佳实践](#最佳实践)

---

## ZAP工具简介

ZCL Advanced Platform (ZAP) 是一个基于 Node.js 的模板引擎，专门用于配置和生成 Matter 设备的数据模型代码。

### 主要功能
- **可视化配置**：通过GUI界面配置Matter端点、集群、属性和设备功能
- **代码生成**：自动生成数据模型定义、回调函数和其他Matter源代码
- **模板系统**：支持自定义模板来生成特定格式的代码
- **集群管理**：管理Matter集群的启用/禁用、属性配置等

### 核心概念
- **端点 (Endpoints)**：Matter设备的逻辑分组，每个端点代表一个功能单元
- **集群 (Clusters)**：定义设备功能的标准化接口
- **属性 (Attributes)**：集群中的数据字段
- **命令 (Commands)**：集群支持的操作

---

## 安装与配置

### 方法一：从GitHub下载预编译版本

1. **下载ZAP工具**
   ```bash
   # 访问ZAP发布页面
   https://github.com/project-chip/zap/releases
   
   # 下载适合你操作系统的版本
   # 例如：zap-linux-x64.deb (Ubuntu/Debian)
   ```

2. **安装ZAP (Ubuntu/Debian)**
   ```bash
   sudo dpkg -i zap-linux-x64.deb
   
   # 验证安装
   which zap
   zap --version
   ```

### 方法二：从源码构建

1. **克隆ZAP仓库**
   ```bash
   git clone https://github.com/project-chip/zap.git
   cd zap
   ```

2. **安装依赖**
   ```bash
   npm install
   ```

3. **构建ZAP**
   ```bash
   npm run build
   ```

4. **设置环境变量**
   ```bash
   export ZAP_DEVELOPMENT_PATH=/path/to/zap
   ```

### 方法三：使用ConnectedHomeIP内置脚本

在Matter项目中，可以直接使用内置的ZAP脚本：

```bash
# 在connectedhomeip项目根目录下
./scripts/tools/zap/run_zaptool.sh [ZAP文件路径]
```

---

## ZAP文件结构

ZAP文件是JSON格式的配置文件，包含以下主要部分：

### 基本结构
```json
{
  "fileFormat": 2,
  "featureLevel": 106,
  "creator": "zap",
  "keyValuePairs": [
    {
      "key": "commandDiscovery",
      "value": "1"
    }
  ],
  "package": [...],
  "endpointTypes": [...],
  "endpoints": [...]
}
```

### 关键字段说明
- **fileFormat**: ZAP文件格式版本
- **keyValuePairs**: 全局配置选项
- **package**: 引用的模板和数据包
- **endpointTypes**: 端点类型定义
- **endpoints**: 具体的端点实例

---

## 基本使用方法

### 启动ZAP工具

1. **使用GUI模式**
   ```bash
   # 方法1：直接启动
   zap
   
   # 方法2：打开现有ZAP文件
   zap path/to/your-app.zap
   
   # 方法3：使用Matter项目脚本
   ./scripts/tools/zap/run_zaptool.sh examples/lighting-app/lighting-common/lighting-app.zap
   ```

2. **使用命令行模式**
   ```bash
   # 生成代码（无GUI）
   zap-cli generate -z your-app.zap -g templates.json -o output/
   ```

### 界面概览

ZAP GUI主要包含以下区域：
- **端点列表**：显示所有配置的端点
- **集群配置**：显示当前端点的集群配置
- **属性/命令面板**：配置选中集群的详细信息
- **生成按钮**：生成源代码文件

---

## 集群配置

### 查看和管理集群

1. **选择端点**
   - 在左侧端点列表中选择要配置的端点
   - 例如：Endpoint - 1 (Dimmable Light)

2. **启用/禁用集群**
   ```
   集群状态选项：
   - 未启用 (Not Enabled)
   - 客户端 (Client)
   - 服务器 (Server)  
   - 客户端和服务器 (Client & Server)
   ```

3. **常用集群类型**
   - **Basic Information**: 设备基本信息
   - **On/Off**: 开关控制
   - **Level Control**: 亮度/级别控制
   - **Color Control**: 颜色控制
   - **Identify**: 设备识别
   - **Groups**: 群组功能
   - **Scenes**: 场景功能

### 集群配置示例

以Level Control集群为例：

1. **启用集群**
   - 找到"Level Control"集群
   - 将"Enable"设置为"Server"

2. **配置集群特性**
   - 在集群行点击配置图标 ⚙️
   - 设置Feature Map（如：OnOff, Lighting）

---

## 属性和命令配置

### 属性配置

1. **访问属性配置**
   - 选择集群后点击配置图标
   - 切换到"Attributes"标签

2. **属性配置选项**
   ```
   - Include: 是否包含此属性
   - Storage: 存储类型 (RAM/NVM/External)
   - Default: 默认值
   - Reportable: 是否可报告
   - Min/Max Interval: 报告间隔
   ```

3. **重要属性示例**
   ```
   Level Control集群：
   - CurrentLevel: 当前亮度级别 (0-254)
   - MinLevel: 最小亮度级别 (默认1)
   - MaxLevel: 最大亮度级别 (默认254)
   - RemainingTime: 剩余过渡时间
   ```

### 命令配置

1. **访问命令配置**
   - 在属性配置界面切换到"Commands"标签

2. **命令类型**
   ```
   - Incoming: 设备接收的命令
   - Outgoing: 设备发送的命令
   ```

3. **Level Control命令示例**
   ```
   - MoveToLevel: 移动到指定级别
   - MoveToLevelWithOnOff: 移动到级别并控制开关
   - Move: 连续移动
   - Step: 步进移动
   - Stop: 停止移动
   ```

---

## 代码生成

### 生成配置

1. **选择输出目录**
   - 点击"Generate"按钮
   - 选择代码输出目录
   - 推荐：`zzz_generated/your-app/zap-generated/`

2. **生成的文件类型**
   ```
   - callback-stub.cpp: 集群回调函数
   - attribute-storage.cpp: 属性存储定义
   - endpoint_config.h: 端点配置
   - gen_config.h: 生成的配置宏
   - cluster-objects.h: 集群对象定义
   ```

### 集成生成的代码

1. **包含头文件**
   ```cpp
   #include <app-common/zap-generated/ids/Attributes.h>
   #include <app-common/zap-generated/ids/Clusters.h>
   #include <app-common/zap-generated/attributes/Accessors.h>
   ```

2. **使用生成的代码**
   ```cpp
   using namespace chip::app::Clusters;
   
   // 设置属性
   auto status = LevelControl::Attributes::CurrentLevel::Set(endpoint, level);
   
   // 读取属性
   uint8_t currentLevel;
   LevelControl::Attributes::CurrentLevel::Get(endpoint, &currentLevel);
   ```

---

## 高级功能

### 自定义模板

1. **创建自定义模板**
   ```handlebars
   {{!-- custom-template.zapt --}}
   // Generated code for {{name}}
   {{#zcl_clusters}}
   #define {{as_delimited_macro define}}_CLUSTER_ID {{code}}
   {{/zcl_clusters}}
   ```

2. **配置模板**
   ```json
   {
     "path": "templates/custom-template.zapt",
     "name": "Custom Template",
     "output": "custom-output.h"
   }
   ```

### 插件配置

ZAP支持通过插件扩展功能：

1. **Level Control插件配置**
   ```
   MATTER_DM_PLUGIN_LEVEL_CONTROL_MINIMUM_LEVEL: 最小级别
   MATTER_DM_PLUGIN_LEVEL_CONTROL_MAXIMUM_LEVEL: 最大级别
   MATTER_DM_PLUGIN_LEVEL_CONTROL_RATE: 变化速率
   ```

### 批量操作

1. **批量生成代码**
   ```bash
   # 使用脚本批量处理多个ZAP文件
   ./scripts/tools/zap/zap_regen_all.py
   ```

2. **格式化ZAP文件**
   ```bash
   # 格式化ZAP文件
   ./scripts/tools/zap/zapfile_formatter.py format input.zap
   ```

---

## 常见问题解决

### 问题1：ZAP工具无法启动

**症状**：运行`zap`命令提示"command not found"

**解决方案**：
```bash
# 检查安装
which zap

# 如果未安装，重新安装或设置PATH
export PATH=$PATH:/usr/bin

# 或使用完整路径
/usr/bin/zap your-app.zap
```

### 问题2：生成的代码编译错误

**症状**：包含ZAP生成的头文件时出现编译错误

**解决方案**：
1. 检查ZAP文件配置是否正确
2. 确保使用了正确的模板版本
3. 重新生成代码文件

### 问题3：属性默认值不正确

**症状**：生成的代码中属性默认值与预期不符

**解决方案**：
1. 在ZAP中检查属性的Default值设置
2. 确认数据类型匹配
3. 重新生成代码

### 问题4：集群功能不工作

**症状**：设备不响应特定集群的命令

**解决方案**：
1. 确认集群已启用为"Server"
2. 检查必需的属性是否已包含
3. 验证命令是否标记为"Incoming"

---

## 最佳实践

### 1. 项目组织

```
project/
├── zap/
│   ├── app.zap              # 主ZAP配置文件
│   └── templates/           # 自定义模板
├── generated/
│   └── zap-generated/       # 生成的代码
└── src/
    └── main.cpp            # 主程序
```

### 2. 版本控制

- **包含ZAP文件**：将.zap文件加入版本控制
- **排除生成文件**：将generated/目录加入.gitignore
- **文档化配置**：记录重要的配置决策

### 3. 配置管理

- **使用描述性名称**：为端点和集群使用清晰的名称
- **合理的默认值**：设置符合设备特性的默认值
- **最小化集群**：只启用必需的集群以减少代码大小

### 4. 代码集成

```cpp
// 推荐的代码结构
class MyDevice {
private:
    chip::EndpointId mEndpoint;
    
public:
    // 使用ZAP生成的访问器
    chip::Protocols::InteractionModel::Status SetLevel(uint8_t level) {
        return LevelControl::Attributes::CurrentLevel::Set(mEndpoint, level);
    }
    
    uint8_t GetLevel() {
        uint8_t level;
        LevelControl::Attributes::CurrentLevel::Get(mEndpoint, &level);
        return level;
    }
};
```

### 5. 调试技巧

1. **启用详细日志**
   ```cpp
   #define CHIP_CONFIG_LOG_LEVEL CHIP_LOG_LEVEL_DEBUG
   ```

2. **验证生成的代码**
   ```bash
   # 检查生成的宏定义
   grep -r "LEVEL_CONTROL" generated/
   ```

3. **使用chip-tool测试**
   ```bash
   # 测试Level Control集群
   ./chip-tool levelcontrol move-to-level 128 10 1 1
   ./chip-tool levelcontrol read current-level 1 1
   ```

---

## 总结

ZAP工具是Matter开发中的重要工具，通过正确使用ZAP可以：

1. **简化开发**：自动生成大量样板代码
2. **确保一致性**：使用标准化的Matter集群定义
3. **提高效率**：通过可视化界面快速配置设备功能
4. **减少错误**：避免手动编写复杂的数据模型代码

掌握ZAP工具的使用对于Matter设备开发至关重要，建议开发者深入学习并在实际项目中应用这些最佳实践。

---

## 附录A：实际案例 - 修改Level Control最小值

### 问题背景
在我们的Matter桥接器项目中，遇到了一个问题：SDK默认的Level Control最小值设置为0，但Matter规范要求最小值应该是1。这导致调光功能出现异常。

### 问题分析
1. **ZAP配置正确**：bridge-app.zap中MinLevel属性默认值设置为"0x01"
2. **模板问题**：gen_config.zapt模板中硬编码了`MATTER_DM_PLUGIN_LEVEL_CONTROL_MINIMUM_LEVEL = 0`
3. **配置冲突**：SDK使用模板生成的值覆盖了ZAP配置

### 解决方案

#### 方法1：通过编译选项覆盖（推荐）
在CMakeLists.txt中添加编译器定义：

```cmake
# 在esp32/CMakeLists.txt中
idf_build_set_property(CXX_COMPILE_OPTIONS
    "-DMATTER_DM_PLUGIN_LEVEL_CONTROL_MINIMUM_LEVEL=1" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS
    "-DMATTER_DM_PLUGIN_LEVEL_CONTROL_MINIMUM_LEVEL=1" APPEND)
```

#### 方法2：修改ZAP配置并重新生成
1. 使用ZAP工具打开bridge-app.zap
2. 找到Level Control集群配置
3. 确认MinLevel属性默认值为1
4. 重新生成代码

#### 方法3：创建自定义模板
创建自定义的gen_config.zapt模板：

```handlebars
{{#if (is_str_equal name "Level Control")}}
{{#if (is_server side)}}
// User options for {{side}} plugin {{name}}
#define MATTER_DM_PLUGIN_LEVEL_CONTROL_MAXIMUM_LEVEL 254
#define MATTER_DM_PLUGIN_LEVEL_CONTROL_MINIMUM_LEVEL 1
#define MATTER_DM_PLUGIN_LEVEL_CONTROL_RATE 0
{{/if}}
{{/if}}
```

### 验证修复
```cpp
// 在代码中验证配置
ChipLogProgress(DeviceLayer, "Level Control Min: %d, Max: %d",
                MATTER_DM_PLUGIN_LEVEL_CONTROL_MINIMUM_LEVEL,
                MATTER_DM_PLUGIN_LEVEL_CONTROL_MAXIMUM_LEVEL);
```

---

## 附录B：ZAP文件格式详解

### 端点类型配置
```json
{
  "id": 2,
  "name": "MA-dimmablelight",
  "deviceTypeRef": {
    "code": 257,
    "profileId": 259,
    "label": "MA-dimmablelight",
    "name": "MA-dimmablelight"
  },
  "clusters": [...]
}
```

### 集群配置详解
```json
{
  "name": "Level Control",
  "code": 8,
  "define": "LEVEL_CONTROL_CLUSTER",
  "side": "server",
  "enabled": 1,
  "attributes": [
    {
      "name": "CurrentLevel",
      "code": 0,
      "type": "int8u",
      "included": 1,
      "storageOption": "NVM",
      "defaultValue": "0xFE",
      "reportable": 1,
      "minInterval": 0,
      "maxInterval": 65344
    }
  ]
}
```

### 属性存储选项
- **RAM**: 存储在RAM中，重启后丢失
- **NVM**: 存储在非易失性存储器中，重启后保持
- **External**: 由应用程序外部管理

---

## 附录C：常用ZAP命令参考

### GUI模式命令
```bash
# 启动ZAP GUI
zap

# 打开特定ZAP文件
zap /path/to/app.zap

# 使用特定模板
zap --gen /path/to/templates.json /path/to/app.zap
```

### 命令行模式
```bash
# 生成代码（无GUI）
zap-cli generate \
  --zap /path/to/app.zap \
  --gen /path/to/templates.json \
  --out /path/to/output/

# 验证ZAP文件
zap-cli validate /path/to/app.zap

# 转换ZAP文件格式
zap-cli convert --input old.zap --output new.zap
```

### 批量操作脚本
```bash
# ConnectedHomeIP项目中的批量脚本
./scripts/tools/zap/zap_regen_all.py

# 格式化ZAP文件
./scripts/tools/zap/zapfile_formatter.py format input.zap

# 检查ZAP文件同步
./scripts/tools/zap/check_zcl_file_sync.py
```

---

## 附录D：故障排除指南

### 编译错误排除

1. **未找到生成的头文件**
   ```
   错误：fatal error: 'zap-generated/attribute-id.h' file not found

   解决：
   - 确认已运行ZAP生成代码
   - 检查include路径配置
   - 验证生成的文件位置
   ```

2. **宏定义冲突**
   ```
   错误：'MATTER_DM_PLUGIN_LEVEL_CONTROL_MINIMUM_LEVEL' redefined

   解决：
   - 检查是否有多个定义源
   - 使用编译器选项覆盖
   - 修改模板文件
   ```

### 运行时错误排除

1. **集群不响应命令**
   ```
   症状：设备不响应特定集群命令

   排查步骤：
   - 确认集群已启用为Server
   - 检查命令是否标记为Incoming
   - 验证端点配置正确
   - 查看设备日志
   ```

2. **属性读写失败**
   ```
   症状：属性读写返回错误状态

   排查步骤：
   - 确认属性已包含在ZAP配置中
   - 检查属性访问权限
   - 验证数据类型匹配
   - 查看存储选项配置
   ```

### 调试工具

1. **使用chip-tool测试**
   ```bash
   # 读取集群属性
   ./chip-tool levelcontrol read current-level 1 1

   # 发送集群命令
   ./chip-tool levelcontrol move-to-level 128 10 1 1

   # 订阅属性变化
   ./chip-tool levelcontrol subscribe current-level 5 10 1 1
   ```

2. **启用详细日志**
   ```cpp
   // 在代码中启用调试日志
   #define CHIP_CONFIG_LOG_LEVEL CHIP_LOG_LEVEL_DEBUG

   // 查看特定模块日志
   ChipLogProgress(ZCL, "Level Control: %d", level);
   ```

---

## 参考资源

### 官方文档
- [ZAP GitHub仓库](https://github.com/project-chip/zap)
- [ZAP模板教程](https://github.com/project-chip/zap/blob/master/docs/template-tutorial.md)
- [Matter规范文档](https://csa-iot.org/developer-resource/specifications-download-request/)

### 示例项目
- [ConnectedHomeIP示例](https://github.com/project-chip/connectedhomeip/tree/master/examples)
- [lighting-app示例](https://github.com/project-chip/connectedhomeip/tree/master/examples/lighting-app)
- [bridge-app示例](https://github.com/project-chip/connectedhomeip/tree/master/examples/bridge-app)

### 社区资源
- [Matter开发者论坛](https://github.com/project-chip/connectedhomeip/discussions)
- [CSA开发者资源](https://csa-iot.org/developer-resource/)
- [Google Home开发者文档](https://developers.home.google.com/matter/tools/zap)
