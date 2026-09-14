# EasyConfigSerial

可复制到 PlatformIO `lib/` 的 ESP32 / ESP8266 Arduino 库，支持 ArduinoJson 6.21.4 至 7.x。串口协议与文件读写、项目字段规则通过回调分离。ArduinoJson 6 使用显式内存池，应按项目配置大小设置 `maxBytes`（例如 RemoteBoxV4 和 PCIE-Switch 使用 4096）。

## 移植

复制整个 `EasyConfigSerial` 目录到目标项目的 `lib/EasyConfigSerial`，参照 [Basic 示例](examples/Basic/Basic.ino)。已有 ConfigStore 的项目参照 EasyCommand 仓库内 `integrations/ClockV1.4/SerialConfigService.cpp`。

```cpp
#include <EasyConfigFile.h>
#include <LittleFS.h>

EasyConfigSerial serialConfig(Serial);
EasyConfigFile configFile(LittleFS);

void setup() {
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);
  configFile.setMounted(LittleFS.begin(false));
  serialConfig.begin("MyDevice", configFile.backend([] { ESP.restart(); }));
  // 此后加载业务配置；配置无效时保留串口维护功能。
}

void loop() {
  serialConfig.poll();
  // 业务逻辑
  delay(1);
}
```

`EasyConfigFile` 默认校验 JSON 对象，使用 `setValidator()` 增加必填字段、类型和范围检查。配置路径默认为 `/config.json`，大小上限默认为 16 KiB。指定其他上限时，文件后端和协议对象应使用相同值；当前桌面工具按 `/config.json` 显示文件名。

### 简化接入

`EasyConfigMaintenance` 合并协议与文件后端，适用于直接维护 LittleFS 配置的项目：

```cpp
#include <EasyConfigMaintenance.h>
#include <LittleFS.h>

EasyConfigMaintenance maintenance(Serial, LittleFS, 8192);

// Serial 初始化后调用；validateProjectConfig 只检查候选内容，不应用到运行状态。
void beginMaintenance() {
  maintenance.begin("MyProject", LittleFS.begin(false),
                    validateProjectConfig, [] { ESP.restart(); });
}
```

在主循环、Wi-Fi 等待和配网门户循环调用 `maintenance.poll()`。保存后 `restartRequired()` 返回 true，可据此暂停运行状态自动写回，避免覆盖新文件；该标志不触发重启。需要卸载文件系统时先调用 `setMounted(false)`。ESP8266 应使用 `LittleFSConfig::setAutoFormat(false)` 后调用无参数 `LittleFS.begin()`，参考 Basic 示例。

休眠设备应在配置模式中持续轮询；配置解析失败时保留串口修复入口。库不会主动阻止休眠或修改项目的省电策略。

## 自定义后端

`begin()` 的名称参数是项目名兜底。每次 `hello` 握手通过 `read` 回调读取已保存配置，优先使用顶层非空字符串 `id` 作为设备名。文件缺失、读取失败、超过大小上限、JSON 无效或 `id` 缺失、类型错误、仅含空白时使用项目名。修改并保存 `id` 后，重新连接即可更新桌面显示；编辑器中尚未保存的内容不影响设备名。

| 回调 | 契约 |
| --- | --- |
| `read(content, exists, error)` | 返回实际文件原文，包括损坏 JSON。文件不存在时返回 true、exists=false、空 content；I/O 失败返回 false |
| `validate(content, error)` | 只校验，不写文件，不更新运行状态；失败返回 false 和原因 |
| `write(content, error)` | 再次校验后可靠保存；失败返回 false 和原因；不得在回调内重启 |
| `restart()` | 可选，设备收到重启请求、回复后延迟调用 |

库在执行自定义校验前检查 JSON 对象、传输大小和 CRC。保存前重新读取文件，比较读取时的版本。项目自己的其他写入也应走同一任务/互斥机制。

## 调用约束

- `poll()` 占用该串口 RX，其他模块不要再调用同一串口的 `read()`。
- 使用 ESP32 `HardwareSerial` 时，`setRxBufferSize(2048)` 必须在 `begin()` 前调用。其他 Stream 应能容纳至少一个编码后的分块帧（约 650 字节）。
- 普通日志可共用 TX，协议应答以一次 `write()` 发出；不要在其他任务绕过 HardwareSerial，直接向同一 UART 写入数据。
- 在 Arduino 主循环及 WiFi 等待循环中调用 `poll()`。所有配置操作在同一任务执行；库不创建后台 FreeRTOS 任务。
- 不要在校验或保存回调里递归调用 `poll()`。单次阻塞操作应短于电脑端默认 5 秒响应超时。
- 上传和读取快照在 RAM 中暂存，默认各次传输上限 16 KiB；30 秒不活动后释放。JSON 校验和提交还需要解析文档及读取旧内容的额外内存。
- 挂载 LittleFS 时禁用自动格式化。WiFi 不通和配置无效时仍需运行主循环；文件系统无法挂载时应报告错误。

`EasyConfigFile` 保存前备份原文到 `/config.json.ec-bak`；先逐字节验证临时文件，再调用 LittleFS 原子 rename 替换正式配置。断电后正式路径保留旧文件或新文件。其他文件系统的原子替换语义需要由使用方确认。

## 参考

- [PlatformIO 本地库与依赖](https://docs.platformio.org/en/stable/librarymanager/dependencies.html)
- [ArduinoJson 7 deserializeJson](https://arduinojson.org/v7/api/json/deserializejson/)
- [LittleFS 原子性说明](https://github.com/littlefs-project/littlefs#design)
