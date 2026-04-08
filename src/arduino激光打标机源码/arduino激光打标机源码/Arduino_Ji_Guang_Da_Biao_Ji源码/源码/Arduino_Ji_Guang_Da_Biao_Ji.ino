#include "GCodeParser.h"
#include "DAC8562.h"
#include <math.h>

#define REF_POWER 3.3383  // 参考电压值，用于DAC输出计算
//Arduino UNO (SCLK,DIN,CS，PWM) = 13,11,10,9
#define SS 10  // PIN SYNC on DAC8562 Board
DAC8562 dac = DAC8562(SS, REF_POWER);

#define BEILV 100     //放大倍数
#define STEP_NUM 0.1  //插补距离
// 创建解析器实例
GCodeParser parser;

// Grbl 协议状态
String inputLine = "";                      // 存储从串口读取的一行数据
bool lineReady = false;                     // 标识是否有一整行数据准备就绪
unsigned long lastStatusTime = 0;           // 上次发送状态报告的时间戳
const unsigned long STATUS_INTERVAL = 200;  // 状态报告发送间隔(200ms)

// 机器状态
double currentX = 0.0;                       // 当前X轴坐标位置
double currentY = 0.0;                       // 当前Y轴坐标位置
double currentFeedRate = 0.0;                // 当前进给速度
bool laserEnabled = false;                   // 激光是否开启标志
volatile unsigned long lineCounter = 0;      // 已处理的G代码行数（用于进度跟踪）
unsigned long lastActivityTime = 0;          // 上次设备活动时间戳
const unsigned long ACTIVITY_TIMEOUT = 200;  // 设备活动超时时间(200ms)，超过此时间认为设备空闲

// 位置模式
bool isAbsoluteMode = true;  // 坐标模式：true为绝对坐标(G90)，false为相对坐标(G91)

// 速度控制
const double DEFAULT_FEED_RATE = 600000.0;  // 默认进给速率 10000(mm/s)
const double G0_FEED_RATE = 10000.0;        // G0 快速定位速率5000 (mm/s)

// 激光 PWM（UNO 使用 Timer1 的 OC1A => D9 引脚，1kHz）
const uint8_t LASER_PWM_PIN = 9;    // 激光PWM输出引脚
const double LASER_S_MAX = 1000.0;  // S指令最大值，对应100%占空比
double currentSPower = 0.0;         // 当前S值（0..LASER_S_MAX），控制激光功率

// 串口环形缓冲（提高稳定性与可靠性）
// RX: 将串口数据尽快搬运到软件缓冲；TX: 非阻塞按可写空间吐出
const uint16_t RX_BUF_SIZE = 256;     // 接收缓冲区大小
const uint16_t TX_BUF_SIZE = 512;     // 发送缓冲区大小
volatile uint8_t rxBuf[RX_BUF_SIZE];  // 接收缓冲区数组
volatile uint16_t rxHead = 0;         // 接收缓冲区头指针
volatile uint16_t rxTail = 0;         // 接收缓冲区尾指针
volatile uint8_t txBuf[TX_BUF_SIZE];  // 发送缓冲区数组
volatile uint16_t txHead = 0;         // 发送缓冲区头指针
volatile uint16_t txTail = 0;         // 发送缓冲区尾指针

// 计算环形缓冲区下一个索引位置
inline uint16_t rbNext(uint16_t idx, uint16_t size) {
  return (uint16_t)((idx + 1) % size);
}

// 将硬件串口接收的数据搬运到软件接收缓冲区
void pollSerialToRxBuffer() {
  while (Serial.available()) {
    uint8_t c = (uint8_t)Serial.read();
    uint16_t nextHead = rbNext(rxHead, RX_BUF_SIZE);
    if (nextHead == rxTail) {
      // 溢出：丢弃最旧字节，前移尾指针
      rxTail = rbNext(rxTail, RX_BUF_SIZE);
    }
    rxBuf[rxHead] = c;
    rxHead = nextHead;
  }
}

// 从接收缓冲区尝试取出一整行数据（以 \n 或 \r 结束）
bool tryDequeueLine(String &outLine) {
  if (rxHead == rxTail) return false;
  // 探测是否存在行结束符
  uint16_t i = rxTail;
  bool hasEol = false;
  while (i != rxHead) {
    char c = (char)rxBuf[i];
    if (c == '\n' || c == '\r') {
      hasEol = true;
      break;
    }
    i = rbNext(i, RX_BUF_SIZE);
  }
  if (!hasEol) return false;

  // 组装一行（丢弃行结束符），并前移tail
  outLine = "";
  while (rxTail != rxHead) {
    char c = (char)rxBuf[rxTail];
    rxTail = rbNext(rxTail, RX_BUF_SIZE);
    if (c == '\n' || c == '\r') {
      // 折叠连续的CR/LF
      while (rxTail != rxHead) {
        char c2 = (char)rxBuf[rxTail];
        if (c2 == '\n' || c2 == '\r') {
          rxTail = rbNext(rxTail, RX_BUF_SIZE);
        } else {
          break;
        }
      }
      break;
    } else {
      outLine += c;
      if (outLine.length() > 100) {
        // 防止异常超长行
        break;
      }
    }
  }
  return true;
}

// 判断发送缓冲区是否为空
inline bool txIsEmpty() {
  return txHead == txTail;
}

// 判断发送缓冲区是否已满
inline bool txIsFull() {
  return rbNext(txHead, TX_BUF_SIZE) == txTail;
}

// 向发送缓冲区添加一个字符
void txEnqueueChar(char c) {
  uint16_t nextHead = rbNext(txHead, TX_BUF_SIZE);
  if (nextHead == txTail) {
    // 满则丢弃最旧，释放1字节空间
    txTail = rbNext(txTail, TX_BUF_SIZE);
  }
  txBuf[txHead] = (uint8_t)c;
  txHead = nextHead;
}

// 向发送缓冲区添加字符串
void txEnqueueStr(const char *s) {
  while (*s) { txEnqueueChar(*s++); }
}

// 向发送缓冲区添加换行符
void txEnqueueLn() {
  txEnqueueChar('\n');
}

// 非阻塞地刷新发送缓冲区数据到串口
void txFlushNonBlocking() {
  while (!txIsEmpty()) {
    int canWrite = Serial.availableForWrite();
    if (canWrite <= 0) break;
    // 一次写一个字节，保持简单与兼容性
    Serial.write(txBuf[txTail]);
    txTail = rbNext(txTail, TX_BUF_SIZE);
  }
}

void setup() {
  dac.begin();           // 初始化DAC芯片
  Serial.begin(115200);  // 初始化串口通信，波特率115200
  parser.Initialize();   // 初始化G代码解析器

  // 延迟以确保串口稳定
  delay(500);
  
  dac.writeA(0);//使振镜回中
  dac.writeB(0);

  // 初始化激光 PWM 到 1kHz，初始占空比 0
  setupLaserPwm();

  // 发送 Grbl 欢迎信息
  Serial.println("黑色蒲公英LASER_V1.0");
}

void loop() {
  // 处理串口输入（RX搬运->按行处理）
  processSerialInput();

  // 定期发送状态报告（可选，LaserGRBL 可能需要）
  sendPeriodicStatus();

  // 非阻塞发送TX缓冲
  txFlushNonBlocking();
}

// 处理串口输入数据
void processSerialInput() {
  // 将硬串口缓冲尽快搬到软件环形缓冲
  pollSerialToRxBuffer();
  // 按行出队处理
  String line;
  while (tryDequeueLine(line)) {
    processCommand(line);
  }
}

// 处理单条命令
void processCommand(String line) {
  line.trim();  // 去除行首行尾空白字符

  // 跳过空行
  if (line.length() == 0) {
    return;
  }

  // 处理 Grbl 特殊命令
  if (line.startsWith("$")) {
    processGrblCommand(line);
    return;
  }

  // 处理状态查询
  if (line == "?") {
    sendStatusReport();
    sendOk();
    return;
  }

  // 处理暂停/恢复命令
  if (line == "!") {
    // 暂停/恢复功能（可根据需要实现）
    sendOk();
    return;
  }

  // 处理 G 代码命令
  processGCode(line);
}

// 处理Grbl系统命令
void processGrblCommand(String cmd) {
  if (cmd == "$") {
    // 显示帮助信息
    Serial.println("$G - View gcode parser state");
    Serial.println("$I - View build info");
    Serial.println("$N - View startup blocks");
    Serial.println("$X - Kill alarm lock");
    Serial.println("$H - Run homing cycle");
    Serial.println("$C - Check gcode mode");
    Serial.println("$RST=* - Erase all");
    Serial.println("$RST=$ - Erase settings");
    Serial.println("$RST=# - Erase parameters");
    sendOk();
  } else if (cmd == "$G") {
    // 显示解析器状态
    Serial.print("G0");     // 快速定位模式
    Serial.print(" G54");   // 坐标系
    Serial.print(" G17");   // XY平面
    Serial.print(" G21");   // 毫米单位
    Serial.print(" G90");   // 绝对坐标
    Serial.print(" G94");   // 进给率单位：mm/s
    Serial.println(" M0");  // 程序停止
    sendOk();
  } else if (cmd == "$I") {
    // 显示构建信息
    Serial.print("BD_LASER ");
    Serial.println("V1.0");
    sendOk();
  } else if (cmd == "$X") {
    // 解锁报警
    sendOk();
  } else if (cmd == "$H") {
    // 回零（可根据需要实现）
    dac.writeA(0);
    dac.writeB(0);

    sendOk();
  } else if (cmd == "$C") {
    // 检查 G 代码模式
    Serial.println("G0 G54 G17 G21 G90 G94 M0");
    sendOk();
  } else {
    // 未知命令
    sendError(1);
  }
}

// 处理G代码命令
void processGCode(String line) {
  // 清空解析器
  parser.Initialize();

  // 将字符串转换为大写并添加到解析器
  for (size_t i = 0; i < line.length(); i++) {
    char c = line.charAt(i);
    // Convert lowercase to uppercase for G-Code compatibility
    if (c >= 'a' && c <= 'z') {
      c = c - 'a' + 'A';
    }
    parser.AddCharToLine(c);
  }

  // 解析行
  parser.ParseLine();

  bool commandExecuted = false;
  // 在全局变量区域添加新常量定义
  const double INTERPOLATION_STEP = STEP_NUM;  // 插补步长 0.1mm

  // 检查是否有 G 命令
  if (parser.HasWord('G')) {
    double gValue = parser.GetWordValue('G');
    // 替换整个 G0/G1 处理部分为以下代码
    // if (gValue == 0 || gValue == 1) {
    if (gValue == 1) {
      // G1 - 直线插补命令
      commandExecuted = true;

      // 保存起点坐标
      double startX = currentX;
      double startY = currentY;

      // 计算目标坐标
      double targetX = currentX;
      double targetY = currentY;

      if (parser.HasWord('X')) {
        double xVal = parser.GetWordValue('X');  // 获取X坐标值
        targetX = isAbsoluteMode ? xVal : (currentX + xVal);
      }

      if (parser.HasWord('Y')) {
        double yVal = parser.GetWordValue('Y');  // 获取Y坐标值
        targetY = isAbsoluteMode ? yVal : (currentY + yVal);
      }

      // 获取 F 速度（记录）
      if (parser.HasWord('F')) {
        currentFeedRate = parser.GetWordValue('F');
      }

      // 获取 S 值（激光功率，映射到 1kHz PWM 占空比）
      if (parser.HasWord('S')) {
        double s = parser.GetWordValue('S');
        if (s < 0) s = 0;
        if (s > LASER_S_MAX) s = LASER_S_MAX;
        currentSPower = s;
        setLaserDutyByS(currentSPower);
        laserEnabled = (currentSPower > 0);
      }

      // 计算总移动距离
      double dx = targetX - startX;
      double dy = targetY - startY;
      double distance = sqrt(dx * dx + dy * dy);

      // 如果需要移动，则进行插补
      if (distance >= STEP_NUM) {  // 当移动距离大于0.1mm时才执行插补
        // 确定使用的进给速率
        double feedRate = 0;
        // G1切削进给，使用F值或默认值
        feedRate = (currentFeedRate > 0) ? currentFeedRate : DEFAULT_FEED_RATE;

        // 计算每个插补步骤的时间
        double stepTimeMs = (INTERPOLATION_STEP / feedRate * 60000);  // 转换为毫秒
        if (stepTimeMs < 0.001) stepTimeMs = 0.001;                   // 限制最小步长时间
        if (stepTimeMs > 1000) stepTimeMs = 1000;                     // 限制最大步长时间

        // 计算插补步数
        int steps = (int)(distance / INTERPOLATION_STEP);
        if (steps == 0) steps = 1;  // 至少执行一步

        // 计算每步的增量
        double stepDx = dx / steps;
        double stepDy = dy / steps;

        // 执行插补步骤
        for (int i = 1; i <= steps; i++) {
          currentX = startX + stepDx * i;
          currentY = startY + stepDy * i;

          // 更新DAC输出
          dac.outPutValue(0x18, (unsigned int)(currentX * BEILV));
          dac.outPutValue(0x19, (unsigned int)(currentY * BEILV));

          // 根据F值延迟，控制速度
          delay((unsigned long)stepTimeMs);  //
        }

        // 确保最终位置准确
        currentX = targetX;
        currentY = targetY;
        dac.outPutValue(0x18, (unsigned int)(currentX * BEILV));
        dac.outPutValue(0x19, (unsigned int)(currentY * BEILV));
      }
    } else if (gValue == 0) {
      // G0 - 快速定位
      commandExecuted = true;

      // 保存起点坐标
      double startX = currentX;
      double startY = currentY;

      // 计算目标坐标
      double targetX = currentX;
      double targetY = currentY;

      if (parser.HasWord('X')) {
        double xVal = parser.GetWordValue('X');  // 获取X坐标值
        targetX = isAbsoluteMode ? xVal : (currentX + xVal);
      }

      if (parser.HasWord('Y')) {
        double yVal = parser.GetWordValue('Y');  // 获取Y坐标值
        targetY = isAbsoluteMode ? yVal : (currentY + yVal);
      }

      // 获取 F 速度（记录）
      if (parser.HasWord('F')) {
        currentFeedRate = parser.GetWordValue('F');
      }

      // 计算总移动距离
      double dx = targetX - startX;
      double dy = targetY - startY;
      double distance = sqrt(dx * dx + dy * dy);

      // 如果需要移动，则进行插补
      if (distance > STEP_NUM) {  // 当移动距离大于0.1mm时才执行插补
        // 确定使用的进给速率
        double feedRate = 0;
        // G0 快速定位，使用快速速率或当前F值
        feedRate = (currentFeedRate > 0) ? max(currentFeedRate, G0_FEED_RATE) : G0_FEED_RATE;

        // 计算每个插补步骤的时间
        double stepTimeMs = (INTERPOLATION_STEP / feedRate * 60000);  // 转换为毫秒
        if (stepTimeMs < 0.001) stepTimeMs = 0.001;                   // 限制最小步长时间
        if (stepTimeMs > 1000) stepTimeMs = 1000;                     // 限制最大步长时间

        // 计算插补步数
        int steps = (int)(distance / INTERPOLATION_STEP);
        if (steps == 0) steps = 1;  // 至少执行一步

        // 计算每步的增量
        double stepDx = dx / steps;
        double stepDy = dy / steps;

        // 执行插补步骤
        for (int i = 1; i <= steps; i++) {
          currentX = startX + stepDx * i;
          currentY = startY + stepDy * i;

          // 更新DAC输出
          dac.outPutValue(0x18, (unsigned int)(currentX * BEILV));
          dac.outPutValue(0x19, (unsigned int)(currentY * BEILV));

          // 根据F值延迟，控制速度
          delay((unsigned long)stepTimeMs);  //
        }

        // 确保最终位置准确
        currentX = targetX;
        currentY = targetY;
        dac.outPutValue(0x18, (unsigned int)(currentX * BEILV));
        dac.outPutValue(0x19, (unsigned int)(currentY * BEILV));
      } else {
        // 距离很小时直接移动到目标位置
        currentX = targetX;
        currentY = targetY;
        dac.outPutValue(0x18, (unsigned int)(currentX * BEILV));
        dac.outPutValue(0x19, (unsigned int)(currentY * BEILV));
      }
    } else if (gValue == 28) {
      // G28 - 回零
      currentX = 0;
      currentY = 0;
      dac.writeA(0);
      dac.writeB(0);
      commandExecuted = true;
    } else if (gValue == 90) {
      // G90 - 绝对坐标模式
      isAbsoluteMode = true;
      commandExecuted = true;
    } else if (gValue == 91) {
      // G91 - 相对坐标模式
      isAbsoluteMode = false;
      commandExecuted = true;
    } else if (gValue == 92) {
      // G92 - 设置零点
      currentX = 0;
      currentY = 0;
      dac.writeA(0);
      dac.writeB(0);
    } else if (gValue == 2 || gValue == 3) {
      // G2/G3 圆弧命令（直接跳到终点，使用F值控制速度）
      commandExecuted = true;

      // 保存起点坐标
      double startX = currentX;
      double startY = currentY;

      // 计算目标坐标
      double targetX = currentX;
      double targetY = currentY;

      if (parser.HasWord('X')) {
        double xVal = parser.GetWordValue('X');
        targetX = isAbsoluteMode ? xVal : (currentX + xVal);
      }
      if (parser.HasWord('Y')) {
        double yVal = parser.GetWordValue('Y');
        targetY = isAbsoluteMode ? yVal : (currentY + yVal);
      }

      // 更新进给速率（记录）
      if (parser.HasWord('F')) {
        currentFeedRate = parser.GetWordValue('F');
      }

      // I, J 参数（不处理，仅记录）
      // if (parser.HasWord('I')) ...
      // if (parser.HasWord('J')) ...

      // 计算移动距离（直线距离，用于速度控制）
      double dx = targetX - startX;
      double dy = targetY - startY;
      double distance = sqrt(dx * dx + dy * dy);

      // 根据F值控制速度
      if (distance > 0.1) {  // 当移动距离大于0.1mm时执行移动
        // 使用F值或默认值
        double feedRate = (currentFeedRate > 0) ? currentFeedRate : DEFAULT_FEED_RATE;

        // 计算移动时间（毫秒）
        double timeMs = (distance / feedRate) * 60 * 1000.0;

        // 限制延迟时间范围
        if (timeMs < 0.001) timeMs = 0.001;
        if (timeMs > 1000) timeMs = 1000;

        // 执行移动
        currentX = targetX;
        currentY = targetY;
        // dac.writeA(currentX);//
        // dac.writeB(currentY);//
        dac.outPutValue(0x18, (unsigned int)(currentX * BEILV));
        dac.outPutValue(0x19, (unsigned int)(currentY * BEILV));


        // 根据F值延迟，控制速度
        delay((unsigned long)timeMs);
      }
      // else {
      //   // 距离很小，直接更新坐标
      //   currentX = targetX;
      //   currentY = targetY;
      //   // dac.writeA(currentX);//
      //   // dac.writeB(currentY);//
      //   dac.outPutValue(0x18, (unsigned int)(currentX * BEILV));
      //   dac.outPutValue(0x19, (unsigned int)(currentY * BEILV));
      // }
    }
  }

  // 处理 M 命令
  if (parser.HasWord('M')) {
    double mValue = parser.GetWordValue('M');
    if (mValue == 3 || mValue == 4) {
      // M3/M4 - 开启激光
      laserEnabled = true;
      commandExecuted = true;
      // 若之前 S 为 0，不强制设置，保持当前 S；只是确保 PWM 输出处于当前占空比
      setLaserDutyByS(currentSPower);
    } else if (mValue == 5) {
      // M5 - 关闭激光
      laserEnabled = false;
      commandExecuted = true;
      setLaserDutyByS(0.0);
    } else if (mValue == 2) {
      // M2 - 程序结束
      sendOk();
    } else if (mValue == 9) {

      sendOk();
    }
  }

  // 发送响应
  if (commandExecuted) {
    // 标记活动并递增已处理行计数
    lastActivityTime = millis();
    lineCounter++;
    sendOk();
  } else {
    sendOk();  // 即使未识别命令也发送 ok（兼容性考虑）
  }
}

// 发送"ok"响应表示命令执行完成
void sendOk() {
  txEnqueueStr("ok\n");
}

// 发送错误信息
void sendError(int errorCode) {
  char buf[24];
  snprintf(buf, sizeof(buf), "error:%d\n", errorCode);
  txEnqueueStr(buf);
}

// 发送状态报告
void sendStatusReport() {
  // 发送 Grbl 状态报告格式（一次性拼接，入TX缓冲）
  // <Idle|Run|...>,MPos:x,y,z,FS:f,s|Ln:n
  char buf[125];
  const char *state = (millis() - lastActivityTime < ACTIVITY_TIMEOUT) ? "Run" : "Idle";
  char xbuf[16], ybuf[16];
  dtostrf(currentX, 0, 3, xbuf);
  dtostrf(currentY, 0, 3, ybuf);
  snprintf(buf, sizeof(buf), "<%s|MPos:%s,%s,0.000|FS:%ld,%d|Ln:%ld>\r\n",
           state,
           xbuf,
           ybuf,
           (long)currentFeedRate,
           (int)currentSPower,
           (unsigned long)lineCounter);
}

// 定期发送状态报告
void sendPeriodicStatus() {
  // 可选：定期发送状态报告（LaserGRBL 可能需要）
  // 如果不需要可以注释掉
  unsigned long currentTime = millis();
  if (currentTime - lastStatusTime >= STATUS_INTERVAL) {
    sendStatusReport();  // 取消注释以启用定期状态报告
    lastStatusTime = currentTime;
  }
}

// ---------- 激光 PWM 实现（UNO Timer1 -> D9，1kHz） ----------
// 设置激光PWM输出(1kHz)
void setupLaserPwm() {
  pinMode(LASER_PWM_PIN, OUTPUT);
  // 停止 Timer1
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1 = 0;

  // Fast PWM, TOP = ICR1 (WGM13:10 = 14 = 1110b)
  TCCR1A = (1 << WGM11);
  TCCR1B = (1 << WGM13) | (1 << WGM12);

  // 非反相输出 OC1A (D9)
  TCCR1A |= (1 << COM1A1);

  // 设置 TOP = 1999，对应 1kHz: f = 16MHz / (8 * (1 + TOP))
  ICR1 = 1999;

  // 初始占空比 0
  OCR1A = 0;

  // 预分频 8（CS11=1）
  TCCR1B |= (1 << CS11);
}

// 根据S值设置激光PWM占空比
void setLaserDutyByS(double s) {
  if (s < 0) s = 0;
  if (s > LASER_S_MAX) s = LASER_S_MAX;
  uint16_t top = ICR1;  // 1999
  uint16_t duty = (uint16_t)((s / LASER_S_MAX) * (top + 1));
  if (duty > top) duty = top;
  OCR1A = duty;
}