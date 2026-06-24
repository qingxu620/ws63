# WS63 Screen Panel LVGL - Refactored Architecture

WS63 激光打标机本地工控 HMI，基于 LVGL v9.3.0，运行在 MSP3223 (ILI9341V 320x240 + FT6336U) 屏幕节点上。

参考 [esp32s3-lvgl-terminal](https://github.com/Lee-Stone/esp32s3-lvgl-terminal) 的模块化设计和 UI 工程化方法（SquareLine 页面结构、released/pressed 图标按钮、淡入动画、任务按需启动），但定位完全不同：本项目是工业激光设备的本地控制面板，不是娱乐终端。

## 产品定位

| 角色 | 说明 |
|------|------|
| **激光打标机 HMI** | 实时显示 RX 状态、任务进度、SLE 链路、错误信息 |
| **本地安全控制** | STOP / ABORT / FOCUS 一键操作，不依赖 Host |
| **技术力展示** | 精致 UI、丝滑动画、深色工业主题 |
| **提示音系统** | 任务完成 / 错误 / 触摸反馈音（不是音乐播放器） |
| **彩蛋游戏** | 等待打标时的隐藏小游戏（低优先级，不影响安全） |

## 设计理念

| 特性 | 说明 |
|------|------|
| **模块化** | 每个功能模块独立的 `.c/.h` 文件，职责单一 |
| **分层架构** | HAL → Service → UI 三层分离 |
| **任务管理** | 统一的 LiteOS 任务创建和生命周期管理 |
| **配置集中** | 全局配置结构体，统一管理状态 |
| **可扩展** | 新增功能只需添加模块，不影响现有代码 |
| **页面工程化** | 参考 SquareLine 的单文件单页面结构，支持懒加载 |
| **安全优先** | STOP/ABORT 不受动画、页面切换、游戏影响 |

## 目录结构

```
ws63_screen_panel_lvgl_refactor/
├── src/
│   ├── main.c                    # 入口：硬件初始化、任务启动
│   ├── config.h                  # 全局配置结构体和宏定义
│   │
│   ├── hal/                      # 硬件抽象层
│   │   ├── spi_bus.c/.h          # SPI 总线互斥管理
│   │   ├── lcd_driver.c/.h       # ILI9341 LCD 驱动
│   │   ├── touch_driver.c/.h     # FT6336 触摸驱动
│   │   └── sd_card.c/.h          # SD 卡 SPI 驱动
│   │
│   ├── service/                  # 服务层
│   │   ├── task_manager.c/.h     # LiteOS 任务管理
│   │   ├── sle_client.c/.h       # SLE 客户端（连接RX）
│   │   ├── status_sync.c/.h      # 状态同步服务
│   │   ├── cmd_dispatcher.c/.h   # 命令分发器
│   │   └── job_uploader.c/.h     # 离线任务上传
│   │
│   ├── storage/                  # 存储服务
│   │   ├── config_store.c/.h     # 配置持久化
│   │   ├── task_scanner.c/.h     # SD卡任务扫描
│   │   ├── manifest.c/.h         # manifest.json 解析
│   │   └── history_log.c/.h      # 任务历史记录
│   │
│   └── ui/                       # UI 层
│       ├── ui_manager.c/.h       # UI 主管理器、页面路由
│       ├── panel_theme.c/.h      # 深色工业主题
│       │
│       ├── pages/                # 页面（单文件单页面，懒加载）
│       │   ├── page_home.c/.h        # Laser Dashboard（默认页）
│       │   ├── page_job_monitor.c/.h # 任务执行详情
│       │   ├── page_control.c/.h     # 激光控制面板
│       │   ├── page_alert_sound.c/.h # 提示音配置
│       │   ├── page_settings.c/.h    # 系统设置
│       │   ├── page_diagnostics.c/.h # 诊断信息
│       │   └── page_easter_egg.c/.h  # 彩蛋游戏（隐藏）
│       │
│       ├── widgets/              # 自定义组件
│       │   ├── status_badge.c/.h     # 状态胶囊
│       │   ├── progress_arc.c/.h     # 进度弧
│       │   └── nav_bar.c/.h          # 导航栏
│       │
│       ├── images/               # 图片资源（LVGL 图片数组）
│       └── fonts/                # 字体资源
│
├── config/                       # 构建配置
│   └── screen_panel.cmake        # CMake 配置
│
└── docs/                         # 设计文档
    ├── ui_style_study.md         # 开源 UI 设计学习
    ├── ui_product_spec.md        # 产品定位与功能边界
    ├── ui_page_map.md            # 页面结构与导航
    ├── ui_motion_spec.md         # 动画规范
    ├── architecture.md           # 架构说明（待实现）
    ├── api_reference.md          # API 参考（待实现）
    └── integration_guide.md      # 集成指南（待实现）
```

## 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                        UI Layer                             │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐      │
│  │Dashboard │ │JobMonitor│ │ Control  │ │ Settings │      │
│  │(Home)    │ │          │ │ Panel    │ │          │      │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐                   │
│  │AlertSound│ │Diagnost- │ │EasterEgg │                   │
│  │          │ │  ics     │ │(hidden)  │                   │
│  └──────────┘ └──────────┘ └──────────┘                   │
│                       │                                     │
│                       ▼                                     │
│  ┌─────────────────────────────────────────────────────┐   │
│  │                   UI Manager                         │   │
│  │      (页面路由、懒加载、状态管理、事件分发)          │   │
│  └─────────────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────┤
│                      Service Layer                          │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐        │
│  │ Task Manager │ │ Status Sync  │ │Cmd Dispatcher│        │
│  │ (任务管理)   │ │ (状态同步)   │ │ (命令分发)   │        │
│  └──────────────┘ └──────────────┘ └──────────────┘        │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐        │
│  │ SLE Client   │ │Job Uploader  │ │ Config Store │        │
│  │ (SLE通信)    │ │ (任务上传)   │ │ (配置存储)   │        │
│  └──────────────┘ └──────────────┘ └──────────────┘        │
├─────────────────────────────────────────────────────────────┤
│                        HAL Layer                            │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐        │
│  │  SPI Bus     │ │  LCD Driver  │ │Touch Driver  │        │
│  │ (总线互斥)   │ │ (ILI9341)    │ │ (FT6336)     │        │
│  └──────────────┘ └──────────────┘ └──────────────┘        │
│  ┌──────────────┐                                          │
│  │  SD Card     │                                          │
│  │ (SD卡驱动)   │                                          │
│  └──────────────┘                                          │
├─────────────────────────────────────────────────────────────┤
│                       Hardware                              │
│  SPI0 (LCD + SD) │ I2C1 (Touch) │ GPIO (LED/Buzzer)       │
└─────────────────────────────────────────────────────────────┘
```

## 任务架构

| 任务名 | 优先级 | 栈大小 | 核心职责 | 生命周期 |
|--------|--------|--------|----------|----------|
| `panel_task` | 25 | 12KB | LVGL 主循环、UI 刷新 | 常驻 |
| `sle_rx_task` | 4 | 8KB | SLE 状态轮询、响应接收 | Dashboard 激活时 |
| `sync_task` | 6 | 4KB | 状态同步、数据更新 | Dashboard 激活时 |
| `storage_task` | 20 | 4KB | SD卡操作（低优先级） | 按需创建/删除 |
| `sound_task` | 22 | 2KB | 提示音播放 | 按需创建/删除 |
| `game_task` | 30 | 4KB | 彩蛋游戏（最低优先级） | 页面激活时 |

## 与现有代码的关系

| 现有文件 | 新位置 | 改动说明 |
|----------|--------|----------|
| `main.c` | `src/main.c` | 简化，只做初始化和任务启动 |
| `panel_ui.c` | `src/ui/ui_manager.c` | 拆分为多个页面 |
| `panel_theme.c` | `src/ui/panel_theme.c` | 保留，微调 |
| `lv_port_disp.c` | `src/hal/lcd_driver.c` | 合并显示驱动 |
| `lv_port_indev.c` | `src/hal/touch_driver.c` | 合并触摸驱动 |
| `screen_config.h` | `src/config.h` | 扩展为全局配置 |
| `screen_board.c` | `src/hal/spi_bus.c` | 重构为SPI总线管理 |

## 构建方式

```bash
# 使用现有构建脚本
./scripts/build_screen_firmware.sh --panel

# 或手动构建
python3 build.py -c ws63-liteos-app menuconfig
python3 build.py -c ws63-liteos-app -ninja -j24
```

## 开发状态

- [x] 目录结构创建
- [x] UI 设计文档（风格研究、产品规格、页面地图、动画规范）
- [ ] 第一阶段：UI 原型壳（Dashboard + Settings + 动画，无真实数据）
- [ ] 第二阶段：SLE 集成（真实 RX 状态同步）
- [ ] 第三阶段：SD 卡集成（离线任务、配置持久化）
- [ ] 第四阶段：提示音系统
- [ ] 第五阶段：彩蛋游戏
