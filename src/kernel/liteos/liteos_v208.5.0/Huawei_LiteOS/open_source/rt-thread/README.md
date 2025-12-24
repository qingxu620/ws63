# README - RT-Thread 适配说明

## 1. 简介 (Introduction)

本目录包含了从 **RT-Thread RTOS** 中提取的核心组件及接口代码。为了在 **Huawei LiteOS** 上实现 RT-Thread 兼容接口，我们保留了部分不依赖内核底层的通用工具代码。

## 2. 来源与版本 (Source & Version)

- **开源项目**：RT-Thread
- **官方仓库**：https://github.com/RT-Thread/rt-thread
- **版本/分支**：v5.2.1

## 3. 裁剪说明 (Pruning & Tailoring Notes)

为了使 RT-Thread 源码能够在 LiteOS 环境下轻量化运行，我们对原始代码进行了如下裁剪与调整：

- **宏裁剪**：通过 `LOSCFG_COMPAT_RT_THREAD` 等宏对 `.c` 文件进行了大幅度精简，仅保留核心逻辑。
- **文件精简**：仅保留了 `include/` 、 `src/`和`components/` 中必要的头文件及源文件。涉及的文件和目录如下：
  - .c源文件
  
    1. `rt-thread/components/drivers/ipc/pipe.c`
    2. `rt-thread/components/drivers/ipc/ringbuffer.c`
    3. `rt-thread/components/drivers/ipc/ringblk_buf.c`
    4. `rt-thread/components/drivers/ipc/condvar.c`
    5. `rt-thread/components/drivers/core/device.c`
    6. `rt-thread/src/klibc/kerrno.c`
    7. `rt-thread/src/kservice.c`
  - 头文件目录
      1. `rt-thread/include`
      2. `rt-thread/include/klibc`
      3. `rt-thread/components/drivers/include/ipc`
      4. `rt-thread/components/libc/posix/libdl`
      5. `rt-thread/components/drivers/include`
      6. `rt-thread/components/utilities/ulog`

## 4. 目录结构 (Directory Structure)

```
opensource/
├── rtthread/
│   ├── include/
│   └── src/
│   └── components/
└── README.md        # 本文件
```

## 5. 许可协议 (License)

本目录下的 RT-Thread 源码遵循 **Apache License 2.0** 协议。

- **协议类型**：本目录下的代码源自 [RT-Thread 项目](https://github.com/RT-Thread/rt-thread)，遵循 **Apache License 2.0** 协议。
- **修改说明**：本目录下的部分文件经过了裁剪与修改以适配 LiteOS。根据协议第 4.b 条规定，所有被修改的文件均已在文件头部记录了修改日志（Change Logs）。
- **详细协议**：详细协议内容请参阅 [Apache License 2.0](http://www.apache.org/licenses/LICENSE-2.0)。