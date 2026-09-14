# 串口配置维护

使用 EasyCommand 连接 `Serial` 对应的串口，波特率 115200。支持读取 `/config.json`、设备校验、备份后原子保存和重启。配置上限 4 KiB。

通过原生 USB 串口连接，在 USB 供电模式下维护。电池触发与休眠流程保持原样。

设备名优先使用文件顶层非空字符串 `id`，否则显示 `RemoteBox`。保存不主动应用到运行状态，保存后请在工具中点击“重启设备”。不要同时通过网页或按键保存配置。

复用 `lib/EasyConfigSerial` 的 `EasyConfigMaintenance` 封装；校验不修改运行状态，挂载失败不会自动格式化文件系统。旧文件备份为 `/config.json.ec-bak`。

验证环境：`pio run -e remote_box_ota`。只编译固件，不烧录，也不上传文件系统。
