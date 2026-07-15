# Pacific Doorlock Sniper

单进程 3 线程视频采集编码传输系统，C++17 + CMake，无 ROS2 依赖。

---

## 致谢 & 原项目声明

本项目基于 **五大湖联合大学** 的开源项目 [wele0612/Pacific_doorlock_sniper](https://github.com/wele0612/Pacific_doorlock_sniper) 修改，特此感谢！

原项目 README 副本见 [NOTICE](./NOTICE.md)。

单进程 3 线程视频采集编码传输系统，C++17 + CMake，无 ROS2 依赖。

```
大恒 USB3 相机 ──USB3──▶ 上位机 (doorlock_sniper) ──USB CDC──▶ 下位机 (云台板) ──▶ 操作手电脑
```

数据流：相机 Bayer → BGR → ROI 裁剪 + 运动检测 + 背景灰度化 + 中心保彩 → GStreamer x264 编码 → raw H.264 Annex-B → 带宽限速 → USB CDC 串口

---

## 相对原项目的改动

原项目为 ROS2 多进程架构（海康相机 → `hik_camera_node` → `video_encoder_node` → DDS 发布 `VideoPacket` 消息）。本版本做了以下改动：

### 架构

| 原项目 | 本版本 |
|--------|--------|
| ROS2 Humble，4 个 Package（camera / encoder / decoder / bringup） | **零 ROS2 依赖**，单进程 3 线程 |
| 海康机器人相机 (MvCameraControl SDK) | 大恒 Galaxy USB3 相机 |
| ROS2 DDS 发布 150B `VideoPacket` | USB CDC 串口直出 raw H.264 Annex-B |
| colcon build | `cmake && make` |
| 摄像头、编码器为独立进程 | 线程间 SPSC 帧槽传递，零拷贝零唤醒开销 |
| `sensor_msgs::msg::Image` + cv_bridge 中间拷贝 | 直接 `cv::Mat` 传递 |
| `vector<uint8_t>` + `memmove` 流缓冲 | 64KB 环形缓冲区 (`stream_buffer.hpp`) |



### 串口 & 传输

| 改动 | 说明 |
|------|------|
| **双传输模式** | `raw_h264`（对接云台板自定义图像桥）和 `legacy_chunk_v1`（兼容旧 300B 分包 CRC16 协议） |
| **带宽限速** | 滑动窗口限速，可配置 KB/s 上限和最大发送延迟 |
| **云台板故障恢复** | 串口断连自动重连，积压丢弃对齐 Annex-B 起始码 |

### 工程化

| 改动 | 说明 |
|------|------|
| **配置系统** | YAML 配置文件 + CLI `--set key=value` 覆盖 |
| **Fake 模式** | 合成移动目标画面 + 内置 H.264 解码回环验证，无需硬件即可全链路测试 |
| **一键启动脚本** | `launch_sniper.sh`：自动检测相机/云台板，自动回退 Fake 模式 |
| **调试 dump** | 可选帧 dump（原始/ROI/静态/最终），按帧号保存 PNG |

---

## 硬件要求

| 组件 | 说明 |
|------|------|
| 上位机 | Jetson / x86 工控机，Ubuntu 22.04+ |
| 相机 | 大恒 Galaxy USB3 Vision 相机 |
| 下位机 | 越路嵌入式云台板，USB CDC |

大恒 Galaxy SDK（头文件 + .so）已随仓库提供在 `Galaxy_camera/` 下，无需额外下载。

---

## 编译 & 运行

### 安装依赖

```bash
sudo apt install -y build-essential cmake pkg-config \
  libopencv-dev libyaml-cpp-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly gstreamer1.0-libav
```

### 编译

```bash
cd standalone && mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行

```bash
./launch_sniper.sh                 # 实机运行
./launch_sniper.sh --display       # 带预览窗口
./launch_sniper.sh --fake --display  # 无硬件测试
```

脚本自动检测相机和云台板串口。检测不到硬件时自动切到 Fake 模式。

首次使用需安装相机 udev 规则并增大 USB 缓冲：

```bash
sudo cp Galaxy_camera/config/99-galaxy-u3v.rules /etc/udev/rules.d/ && sudo udevadm control --reload
sudo bash Galaxy_camera/SetUSBStack.sh
```

---

## 配置

编辑 `standalone/config/sniper.yaml`，或用 `--set` 命令行覆盖：

```bash
./launch_sniper.sh --set encoder.output_size=320 --set camera.exposure_time=20000
```

核心参数：`encoder.output_size`（分辨率）、`encoder.target_bitrate`（码率）、`serial.transport_mode`（`raw_h264` / `legacy_chunk_v1`）。

---

## 常见问题

| 现象 | 解决 |
|------|------|
| `libgxiapi.so: cannot open` | 设置 `LD_LIBRARY_PATH` 包含 `Galaxy_camera/lib/x86_64` |
| 检测不到相机 | `lsusb \| grep 2ba2` 确认连接；检查 udev 规则 |
| 串口设备不存在 | 云台板未连接；改用 `/dev/serial/by-id/` 路径 |
| 相机画面异常 | USB 带宽不足：`echo 1000 \| sudo tee /sys/module/usbcore/parameters/usbfs_memory_mb` |

---

## License

MIT
