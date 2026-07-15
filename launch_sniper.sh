#!/bin/bash
set -euo pipefail

#============================================================
# Pacific Doorlock Sniper — 一键启动脚本
# 自动查找摄像头 + 云台板串口，设置环境后启动上位机
#============================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/standalone/build-real"
BINARY="$BUILD_DIR/doorlock_sniper"
CONFIG="$SCRIPT_DIR/standalone/config/sniper.yaml"
GALAXY_LIB="$SCRIPT_DIR/Galaxy_camera/lib/x86_64"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
die()       { log_error "$*"; exit 1; }

# ---- 解析参数 ----
WITH_DISPLAY=false
FAKE_MODE=false
EXTRA_SETS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --display)   WITH_DISPLAY=true; shift ;;
    --fake)      FAKE_MODE=true; shift ;;
    --set)       EXTRA_SETS+=("--set" "$2"); shift 2 ;;
    --help|-h)
      echo "用法: $0 [--display] [--fake] [--set key=value ...]"
      echo ""
      echo "  --display    启用本机预览窗口（需要显示器）"
      echo "  --fake       无硬件测试模式（假相机 + 内存回环）"
      echo "  --set k=v    覆盖配置项，可多次使用"
      echo ""
      echo "示例:"
      echo "  $0                                    # 实机运行，无预览"
      echo "  $0 --display                          # 实机运行 + 预览窗口"
      echo "  $0 --fake --display                   # 无硬件测试"
      echo "  $0 --set encoder.output_size=320     # 自定义分辨率"
      exit 0
      ;;
    *) die "未知参数: $1 (用 --help 查看用法)" ;;
  esac
done

# ---- 环境检查 ----
log_info "Pacific Doorlock Sniper 启动脚本"

# 检查 Galaxy SDK 库
if [ ! -d "$GALAXY_LIB" ]; then
  die "Galaxy SDK 目录不存在: $GALAXY_LIB"
fi
export LD_LIBRARY_PATH="$GALAXY_LIB:$LD_LIBRARY_PATH"

# 检查配置文件
if [ ! -f "$CONFIG" ]; then
  die "配置文件不存在: $CONFIG"
fi

# ---- 检查二进制 ----
if [ ! -f "$BINARY" ]; then
  log_warn "未找到 build-real 二进制，尝试检查 build/ ..."
  BUILD_DIR="$SCRIPT_DIR/standalone/build"
  BINARY="$BUILD_DIR/doorlock_sniper"
  if [ ! -f "$BINARY" ]; then
    die "未编译！请先编译:\n  cd standalone && mkdir build-real && cd build-real && cmake .. && make -j\$(nproc)"
  fi
  if ! ldd "$BINARY" 2>/dev/null | grep -q gxiapi; then
    log_warn "build/ 可能是 FAKE_ONLY 版本，无法使用真相机"
  fi
fi

# ---- 摄像头检查 ----
if [ "$FAKE_MODE" = false ]; then
  CAM_COUNT=$(lsusb 2>/dev/null | grep -c "2ba2" || true)
  if [ "$CAM_COUNT" -eq 0 ]; then
    log_warn "未检测到大恒相机 (VID 2ba2)，自动切到 FAKE 模式"
    FAKE_MODE=true
  else
    log_info "检测到 $CAM_COUNT 个大恒相机"
  fi
fi

# ---- 串口检查 ----
SERIAL_PORT=""
if [ "$FAKE_MODE" = false ]; then
  SERIAL_PORT=$(ls /dev/serial/by-id/usb-YueLuEmbedded* 2>/dev/null | head -1 || true)
  if [ -z "$SERIAL_PORT" ]; then
    # 回退：尝试 ttyACM
    SERIAL_PORT=$(ls /dev/ttyACM* 2>/dev/null | head -1 || true)
  fi
  if [ -z "$SERIAL_PORT" ]; then
    log_warn "未找到云台板串口，自动切到 FAKE 模式"
    FAKE_MODE=true
  else
    log_info "串口: $SERIAL_PORT"
  fi
fi

# ---- 构建启动命令 ----
CMD=(
  "$BINARY"
  "--config" "$CONFIG"
)

if [ "$FAKE_MODE" = true ]; then
  log_info "运行模式: FAKE（假相机 + 内存回环）"
  CMD+=(
    "--set" "camera.fake=true"
    "--set" "serial.fake=true"
  )
else
  log_info "运行模式: 实机"
  CMD+=(
    "--set" "serial.port=$SERIAL_PORT"
    "--set" "serial.transport_mode=raw_h264"
    "--set" "camera.fake=false"
    "--set" "serial.fake=false"
  )
fi

CMD+=("--set" "display.enable=$WITH_DISPLAY")

# 追加用户 --set
CMD+=("${EXTRA_SETS[@]}")

# ---- 启动 ----
echo ""
log_info "启动命令: ${CMD[*]}"
echo ""

cd "$BUILD_DIR"
exec "${CMD[@]}"
