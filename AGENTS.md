# fbb_ws63 OpenCode Project Rules

## Project Scope

This WSL environment is dedicated to the fbb_ws63 project only.

The project root is:

```text
/root/fbb_ws63
```

Do not treat Windows paths under /mnt/c as the active development workspace unless explicitly instructed.

## Active Development Directories

The current active development focus is:

- src/ws63_laser_sle_job/
- src/ws63_laser_sle_job_host/

Do not modify old demo directories, unrelated SDK examples, vendor code, or historical experimental folders unless explicitly instructed.

## Development Workflow

Before editing code:

1. Read the relevant files first.
2. Explain the current module responsibility or data flow.
3. Identify the minimum set of files that need modification.
4. Explain the risk of the change.
5. Make small, reviewable patches.

Do not rewrite entire files unless strictly necessary.

Do not make large architecture changes without explicit confirmation.

## Git Rules

Before any commit:

1. Run git status.
2. Run git diff --stat.
3. Review the actual diff.
4. Summarize modified files by purpose.
5. Propose a commit message.

Commit only when explicitly requested.

Use commit messages such as:

- feat: add new functionality
- fix: fix a bug
- refactor: restructure code without behavior change
- docs: update documentation
- chore: tooling or project maintenance
- checkpoint: save current working state

Never commit build outputs, temporary files, logs, or generated binaries.

## Laser Safety Rules

For laser-related code:

1. Any disconnect, abort, fatal parser error, or job cancellation must force laser off.
2. M5 must physically disable PWM/GPIO output.
3. Do not rely only on software modal state to indicate laser state.
4. RX should remain the source of truth for motion execution, job state, and laser state.

## WS63 / LiteOS Task Priority Rules

WS63 runs Huawei LiteOS-M. **Priority numbering is inverted compared to FreeRTOS:**

```text
Lower number = Higher priority
0 = HIGHEST (LOS_TASK_PRIORITY_HIGHEST)
31 = LOWEST (LOS_TASK_PRIORITY_LOWEST)
```

OSAL priority constants (from `osal_task.h`):

```text
OSAL_TASK_PRIORITY_ABOVE_HIGH   2
OSAL_TASK_PRIORITY_HIGH         3
OSAL_TASK_PRIORITY_BELOW_HIGH   4
OSAL_TASK_PRIORITY_ABOVE_MIDDLE 5
OSAL_TASK_PRIORITY_MIDDLE       6
...
```

Current task priorities (from `config.h`):

| Task           | Macro                    | Value | Priority   |
| -------------- | ------------------------ | ----- | ---------- |
| SLE stack      | `TASK_PRIO_SLE`          | 3     | HIGH       |
| UART RX        | `TASK_PRIO_JOB_UART`     | 3     | HIGH       |
| Job executor   | `TASK_PRIO_JOB_EXECUTOR` | 4     | BELOW_HIGH |
| Motion         | `TASK_PRIO_MOTION`       | 4     | BELOW_HIGH |

**Consequences:**

- SLE (3) has **higher** priority than job executor (4). SLE can always preempt exec when data arrives.
- `osal_yield()` on a lower-priority task (e.g. exec at 4) will NOT starve higher-priority tasks (e.g. SLE at 3). When SLE becomes Ready, it preempts exec immediately.
- If you need a task to wait for an event from a higher-priority task, use `ulTaskNotifyTake` / `xTaskNotifyGive`, not busy-wait with `osal_yield(); continue;`. The higher-priority task will unblock the lower one via notification.
- Never increase a task's priority number thinking it makes it "higher priority" — it does the opposite.

## SLE / Transport Rules

When modifying SLE transport logic:

1. Check both transmitter and receiver paths.
2. Preserve packet framing compatibility unless explicitly changing the protocol.
3. Consider duplicate packets, lost ACKs, credit/buffer overflow, timeout, and reconnect behavior.
4. Do not create two independent Grbl controllers.

## Host Tool Rules

For src/ws63_laser_sle_job_host/:

1. Keep Python scripts simple and inspectable.
2. Prefer clear logs over hidden automation.
3. Do not add large dependencies unless necessary.
4. Keep serial protocol behavior explicit.

## Development Environment

1. Firmware code editing and compilation happen in **WSL2**.
2. Project root: `/root/fbb_ws63`.
3. Firmware build commands:

   ```bash
   python3 build.py -c ws63-liteos-app menuconfig
   python3 build.py -c ws63-liteos-app -ninja -j24
   ```

4. Do NOT move the firmware project to `/mnt/c/...` or Windows Desktop for compilation.
5. Host tool source (`src/ws63_laser_sle_job_host/`) can be edited in WSL2, but **running and serial debugging happen on Win11**.
6. The user manually copies/downloads the host tool to Win11 Desktop to run.
7. **Win11 COM 口不固定**，可能因 USB 口、拓展坞、线缆、驱动、插入顺序变化。不要假设 COM8/COM24/COM26 或 COM27/COM6/COM24 仍然有效，这些只能作为历史示例。
8. 每次新的运行/调试会话开始时，必须询问用户当前串口角色映射：
   - **TX 命令串口**：UART1，Host 发送 @BEGIN/@DATA/@EXEC_START/@STATUS
   - **TX 日志串口**：UART0 debug/log，TX 固件 osal_printk
   - **RX 日志串口**：UART0 debug/log，RX 固件 osal_printk
9. 当前安全波特率基线：
   - TX 命令串口：**115200**，除非用户明确说 UART1 已经重新编译并烧录为其他波特率；
   - TX 日志串口：**115200**；
   - RX 日志串口：**115200**。
10. Do NOT assume WSL2 can directly access COM ports.
11. All test instructions must distinguish:
    - **WSL2**: edit source, compile firmware, run scripts, check Git;
    - **Win11**: run Host, select COM ports, capture serial logs, upload G-code, verify execution.
12. 在给 Win11 运行/调试步骤前，先问：
    - 当前 TX 命令串口是哪个 COM？
    - 当前 TX 日志串口是哪个 COM？
    - 当前 RX 日志串口是哪个 COM？
    - 当前 TX 命令串口波特率是否仍为 115200？

## Automation Scripts / Common Workflow

### WSL2 / Win11 分工

| 环境      | 职责                                             |
| --------- | ----------------------------------------------- |
| **WSL2**  | 源码修改、固件编译、Host 上位机同步             |
| **Win11** | Host 运行、串口调试、BurnTool 烧录 |

- 不要把固件工程移动到 `/mnt/c/...` 下编译。
- 不要自动调用 BurnTool。
- 不要自动 commit。

### 1. Host 上位机同步脚本

**路径：** `/root/fbb_ws63/scripts/sync_host_to_win.sh`

**用途：** 将 WSL2 中的 Host 上位机源码同步到 Win11 桌面运行目录。

**WSL2 源码目录：** `/root/fbb_ws63/src/ws63_laser_sle_job_host/`

**Win11 运行目录：** `/mnt/c/Users/ZKX/OneDrive/Desktop/ws63_laser_sle_job_host/`

**使用命令：**

```bash
cd /root/fbb_ws63
./scripts/sync_host_to_win.sh
```

**Win11 启动命令：**

```cmd
cd /d C:\Users\ZKX\OneDrive\Desktop\ws63_laser_sle_job_host
python main.py
```

**说明：**

- 同步前会做 `main.py` 语法检查；
- `logs/` 目录不同步、不覆盖、不删除；
- Host 源码在 WSL2 中修改，实际运行在 Win11。

### 2. TX/RX 固件一键编译与归档脚本

**路径：** `/root/fbb_ws63/scripts/build_sle_job_firmwares.sh`

**用途：** 自动切换 TX/RX 配置，分别编译，归档到 `fwstage`，避免同名产物互相覆盖。

**使用命令：**

```bash
cd /root/fbb_ws63

# 编译 TX + RX（默认）
./scripts/build_sle_job_firmwares.sh --both

# 只编译 TX
./scripts/build_sle_job_firmwares.sh --tx-only

# 只编译 RX
./scripts/build_sle_job_firmwares.sh --rx-only
```

**归档目录：**

```text
/root/fbb_ws63/src/output/ws63/fwstage/latest/
```

**TX/RX 固件路径：**

```text
/root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_tx_all.fwpkg
/root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_rx_all.fwpkg
```

### 3. Win11 BurnTool 烧录

- BurnTool 在 Win11 手动使用，不自动调用。
- `ws63-liteos-app_tx_all.fwpkg` 烧录到 TX 板。
- `ws63-liteos-app_rx_all.fwpkg` 烧录到 RX 板。

### 4. 常用开发流程

```bash
# 1. WSL2 修改 Host 上位机
vim /root/fbb_ws63/src/ws63_laser_sle_job_host/main.py

# 2. WSL2 同步 Host 到 Win11 桌面
./scripts/sync_host_to_win.sh

# 3. WSL2 编译 TX/RX 固件
./scripts/build_sle_job_firmwares.sh --both

# 4. Win11 烧录固件（手动 BurnTool）
#    TX: \\wsl.localhost\Ubuntu\root\fbb_ws63\src\output\ws63\fwstage\latest\ws63-liteos-app_tx_all.fwpkg
#    RX: \\wsl.localhost\Ubuntu\root\fbb_ws63\src\output\ws63\fwstage\latest\ws63-liteos-app_rx_all.fwpkg

# 5. Win11 启动 Host 并测试
cd /d C:\Users\ZKX\OneDrive\Desktop\ws63_laser_sle_job_host
python main.py
```

### 5. 手动调焦功能

**功能：** Host 手动控制激光调焦光开关。

**协议：**
- `@FOCUS_ON S{0-100}` → TX 转发 `PKT_FOCUS_CTRL(0x14)` → RX 开激光
- `@FOCUS_OFF` → TX 转发 `PKT_FOCUS_CTRL(0x14)` → RX 关激光
- TX 不直接控制激光，只转发命令
- RX 只在 IDLE 状态允许 FOCUS_ON
- FOCUS_OFF 无条件允许

**RX 安全互锁：**
- EXEC_START / EXEC_STOP / ABORT / disconnect / safe_stop / JOB_EXEC done 均强制 focus_off
- focus_force_off() 幂等

**功率换算：**
- Host S: 0-100
- RX internal: S × 10 = 0-1000
- 占空比: S%

**关键文件：**
- `common/protocol.h`: PKT_FOCUS_CTRL=0x14, focus_ctrl_payload_t
- `transmitter/main.c`: @FOCUS_ON/@FOCUS_OFF 解析
- `receiver/job_manager.c`: handle_focus_ctrl(), focus_force_off(), 安全互锁
- `main.py`: toggle_focus(), _focus_on(), _focus_off(), _focus_off_before_job()

## Response Style

When working in this repository, prefer this structure:

1. Current problem
2. Root cause
3. Minimal fix
4. Affected files
5. Verification method
