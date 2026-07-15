# Pacific Doorlock Sniper

单进程 3 线程视频采集编码传输系统，C++17 + CMake，无 ROS2 依赖。

```
大恒 USB3 相机 ──USB3──▶ 上位机 (doorlock_sniper) ──USB CDC──▶ 下位机 (云台板) ──▶ 操作手电脑
```

数据流：相机 Bayer → BGR 转换 → ROI 裁剪 + 缩放 → 运动检测 + 背景灰度化 + 中心保彩 → GStreamer x264 编码 → raw H.264 Annex-B → 带宽限速 → USB CDC 串口 → 云台板

---

## 硬件要求

| 组件 | 型号 |
|------|------|
| 上位机 | Jetson / x86 工控机，Ubuntu 22.04+ |
| 相机 | 大恒 Galaxy USB3 Vision 相机 |
| 下位机 | 越路嵌入式 (YueLuEmbedded) 云台板，USB CDC |

---

## 快速开始

### 1. 获取大恒 Galaxy SDK

从大恒官网下载 **Galaxy Linux SDK**（C 版本），解压后把 `inc/` 和 `lib/` 放到项目目录：

```
Pacific_doorlock_sniper/
├── Galaxy_camera/
│   ├── inc/          # 头文件 (DxImageProc.h, GxIAPI.h, ...)
│   │   ├── DxImageProc.h
│   │   ├── GxIAPI.h
│   │   └── ...
│   ├── lib/
│   │   └── x86_64/
│   │       ├── libgxiapi.so
│   │       └── ...
│   └── config/
│       └── 99-galaxy-u3v.rules
├── standalone/
│   ├── src/
│   ├── config/
│   └── CMakeLists.txt
└── launch_sniper.sh
```

> 如果只想测试代码（不需要真实相机），跳过此步，编译时使用 `-DFAKE_ONLY=ON`。

### 2. 安装相机 udev 规则

```bash
sudo cp Galaxy_camera/config/99-galaxy-u3v.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

### 3. 增大 USB 帧缓冲

大恒相机需要较大 USB 缓冲，Linux 默认 16MB 不够：

```bash
sudo bash Galaxy_camera/SetUSBStack.sh
# 或手动：echo '1000' | sudo tee /sys/module/usbcore/parameters/usbfs_memory_mb
```

### 4. 安装系统依赖

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  libopencv-dev libyaml-cpp-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly gstreamer1.0-libav
```

### 5. 编译

```bash
cd standalone
mkdir build && cd build
cmake ..
make -j$(nproc)
```

编译产物：`standalone/build/doorlock_sniper`

> **无相机环境**：`cmake .. -DFAKE_ONLY=ON` 编译不含 SDK 的版本，只能使用合成测试图。

### 6. 运行

**实机（真相机 + 云台板）：**

```bash
./launch_sniper.sh
```

脚本会自动检测相机（USB VID `2ba2`）和云台板串口（`/dev/serial/by-id/usb-YueLuEmbedded*`）。检测不到硬件时自动切到 Fake 模式。

**常用选项：**

```bash
./launch_sniper.sh --display          # 带本机预览窗口
./launch_sniper.sh --fake --display   # 无硬件测试
./launch_sniper.sh --set encoder.output_size=320  # 覆盖配置
```

**手动启动（不依赖脚本）：**

```bash
cd standalone/build
LD_LIBRARY_PATH=$(pwd)/../../Galaxy_camera/lib/x86_64:$LD_LIBRARY_PATH \
./doorlock_sniper \
  --config ../config/sniper.yaml \
  --set serial.port=/dev/serial/by-id/usb-YueLuEmbedded_Vision_Comm_port_XXXXXXXX-if00 \
  --set serial.transport_mode=raw_h264 \
  --set display.enable=false
```

### 7. 停止

按 `Ctrl+C` 优雅退出。

---

## 配置参数速查

详见 [`standalone/config/sniper.yaml`](standalone/config/sniper.yaml)，核心参数：

### 相机

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `camera.exposure_time` | 15000 | 曝光时间 (μs) |
| `camera.gain` | 10.0 | 增益 |
| `camera.gamma` | 1.25 | Gamma |
| `camera.saturation` | 36 | 饱和度 |
| `camera.fake` | false | 合成测试图 |

### 编码

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `encoder.output_size` | 300 | 输出分辨率 |
| `encoder.output_fps` | 30 | 输出帧率 |
| `encoder.target_bitrate` | 110 | x264 目标码率 (kbps) |
| `encoder.x264_preset` | veryslow | x264 预设 |
| `encoder.static_simplify` | true | 背景灰度化+模糊 |
| `encoder.center_clear_size` | 123 | 中心保彩区域 |
| `encoder.force_monochrome` | false | 全局黑白 |

### 串口

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `serial.port` | /dev/ttyACM0 | 串口路径 |
| `serial.baud_rate` | 921600 | 波特率 |
| `serial.transport_mode` | raw_h264 | 传输协议 |

### 带宽

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `bandwidth.limit_kbytes_per_s` | 15.0 | 发送限速 (KB/s) |
| `bandwidth.window_s` | 2.0 | 滑动窗口 |

---

## 解码端（操作手电脑）

```bash
pip install av opencv-python pyserial paho-mqtt

# 串口直连解码（仅 legacy_chunk_v1 模式）
python3 standalone/decoder/serial_decoder.py --port /dev/ttyACM0 --baud 921600

# MQTT 解码
python3 standalone/decoder/mqtt_decoder.py --broker 192.168.12.1 --mqtt-port 3333
```

---

## 常见问题

| 现象 | 解决 |
|------|------|
| `libgxiapi.so: cannot open` | `LD_LIBRARY_PATH` 未设：加上 `Galaxy_camera/lib/x86_64` |
| `No camera found` | 检查 USB 线、`lsusb \| grep 2ba2`、udev 规则 |
| 串口设备不存在 | 云台板未连接/未上电，改用 `/dev/serial/by-id/` 路径 |
| 启动后立即退出 | 查看日志；检查 `sniper.yaml` 配置项 |
| 相机画面异常 | USB 带宽不足：增大 `usbfs_memory_mb` 或降低分辨率 |

---

## License

MIT
