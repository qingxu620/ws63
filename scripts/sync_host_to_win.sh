#!/usr/bin/env bash
set -euo pipefail

SRC="/root/fbb_ws63/src/ws63_laser_host_ui/"
DST="/mnt/c/Users/ZKX/OneDrive/Desktop/ws63_laser_host_ui/"
WIN_DST="C:\Users\ZKX\OneDrive\Desktop\ws63_laser_host_ui"

echo "=== Host 上位机同步 ==="

# 1. 语法检查
echo "[1/3] 语法检查..."
python3 -m compileall -q "${SRC}main.py" "${SRC}app" "${SRC}transports" "${SRC}ui" "${SRC}workers"
echo "  Python 语法正确"

# 2. 创建目标目录
echo "[2/3] 同步到 Win11 桌面..."
mkdir -p "${DST}"

# 3. rsync 同步（排除 __pycache__、*.pyc、.venv、venv、logs）
rsync -av --delete \
    --exclude='__pycache__/' \
    --exclude='*.pyc' \
    --exclude='.venv/' \
    --exclude='venv/' \
    --exclude='logs/' \
    --exclude='generated_images/' \
    --exclude='config/host_ui_config.json' \
    "${SRC}" "${DST}"

echo ""
echo "[3/3] 同步完成"
echo ""
echo "Win11 运行路径:"
echo "  ${WIN_DST}"
echo ""
echo "启动命令 (Win11 CMD):"
echo "  cd /d ${WIN_DST}"
echo "  python main.py"
echo ""
echo "启动命令 (PowerShell):"
echo "  Set-Location '${WIN_DST}'"
echo "  python main.py"
