# HiSpark Studio插件版本环境搭建

**备注：仅LiteOS 系统版本支持，OpenHarmony系统版本不支持windows环境搭建**

## 1. VSCode安装

- 下载安装[VSCode](https://code.visualstudio.com/Download) ，选择“windows”安装，根据推荐步骤安装即可。

![image-20250716160500019](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250716160500019.png)

## 2. 安装HiSpark Studio插件

- A. 在Vscode扩展里面搜索“Chinese”，点击安装即可。

![image-20251021111615915](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021111615915-17610397011281.png)

- B. 在VSCode扩展里面搜索“HiSpark Studio”，点击安装即可。

![image-20250716161307500](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250716161307500.png)

- C. 安装完成后，在vscode侧边栏出现“HiSpark Studio”图标，点击“Home”主页。

![image-20250716162432995](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250716162432995.png)

## 3. 安装工具链

前提：目前安装工具链分为“自动安装工具链”和“手动安装工具链”，如果在使用“自动安装工具链”遇到网络、代理等问题，VSCode右下角显示“下载失败”等字样，请参考3.2 手动安装工具链。

### 3.1 自动安装工具链

- A. 点击“Download Toolchain”安装工具及插件。

![image-20251021112938224](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021112938224.png)

- B. 等待安装，如果安装成功，VScode右下角会显示当前状态，出现”环境准备完成“等字样，代表工具安装完成，如下图所示。

![image-20250716162816180](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20250716162816180.png)

- C. 如果出现安装失败（原因：可能网络不通或者网络代理问题，需要更换网络或者更改代理，如果更换后，还是无法下载，请参考“3.2 手动安装工具链”），安装成功可跳过“3.2 手动安装工具链''。

![image-20251021143906612](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021143906612.png)

### 3.2 手动安装工具链

- A. 如果出现安装失败（原因：可能网络不通或者网络代理问题，需要配置代理，如果无法配置代理，请参考“3.2 手动安装工具链”），安装成功可跳过“3.2 手动安装工具链''。

![image-20251021143906612](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021143906612.png)

- B. 将工具脚本“install_vscode_extension.bat”下载到任意目录，这里以E盘为例，下载链接：https://hispark-obs.obs.cn-east-3.myhuaweicloud.com/install_vscode_extension.bat 。

![image-20251021162507036](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021162507036.png)

- C. 下载到指定目录完成后，双击脚本，弹出对话框选择“运行”

  ![image-20251021162730982](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021162730982.png)

- D. 等待下载工具以及配置完成，出现“您可能需要重启VSCode 以使更改生效”字样，重启VSCode，并按键盘任何按键退出命令行模式。

    ![image-20251021164926606](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021164926606.png)
    
- E. 如果没有重启VSCode，请将VSCode重启之后，在HiSpark Studio插件里点击“Download Toochain”进行配置安装。

    ![image-20251021165956096](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021165956096.png)

- F. 等待出现“环境准备完成”字样，代表环境配置完成。

  ![image-20251021170323506](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021170323506.png)

## 4. SDK包下载

前提：目前SDK宝下载分为git下载SDK包和手动下载SDK包，电脑端需要安装git，如果没有安装git，或者git安装下载较慢，可以参考“4.2 手动下载SDK”。

### 4.1 git下载SDK包

- A. 前提：电脑端需要安装git，如果没有安装git，请先安装git，如果已有，可以跳过此步骤，git下载链接：https://git-scm.com/install/windows。（注意：如果下载较慢或者无法下载，可以参考“4.2 手动下载SDK”）

![image-20251021145120866](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021145120866.png)

- B. 根据需要下载对应SDK包，这里以”WS63 SDK，点击“Download SDK from HiSpark”，选择“WS63 SDK”。

![image-20251021145330518](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021145330518.png)

- C. 选择完成以后，选择想下载到的目录，这里以E盘为例(**注意：路径层级不要太深，且不能包含中文目录**)。

![image-20250716163305452](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250716163305452.png)

### 4.2 手动下载SDK包

- A. WS63 SDK下载链接：https://gitee.com/HiSpark/fbb_ws63 ，点击“克隆/下载”，在弹框中选择“下载ZIP”。

  ![image-20251021150800671](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021150800671.png)

- B. 选择下载“ZIP”后，选择需要下载的目录，这里以E盘为例。

  ![image-20251021151058741](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021151058741.png)

- C. 等待下载完成，大约510M。

  ![image-20251021151128289](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021151128289.png)

- D. 下载完成后，解压“fbb_ws63-master.zip”，右键该文件选择“解压到当前文件夹或者Extract Here”。

  ![image-20251021151922359](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021151922359.png)

- E. 解压完成后，如下图所示

  ![image-20251021154433352](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021154433352.png)

## 5. 新建工程

- A. 点击“新建工程”，根据提示填写对应信息，芯片选择“WS63”， 工程类型选择“普通工程”，工程名填写“demo”。

![image-20250716163414703](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250716163414703.png)

- B. 工程路径选择“xxx/src”目录，软件包选择"xxx/src"（注意：路径选择需要选到src目录以下），路径为“fbb_ws63-master/src”目录，点击“选择文件夹”。

![image-20251021155638209](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021155638209.png)

- C. 选择完成后，点击“完成”，等待新建，新建成功如图所示。

  ![image-20251021155211680](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/tools/image-20251021155211680.png)

## 6. 编译工程

- A. 新建成功后，点击“build”进行编译

![image-20250716163653427](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250716163653427.png)

- B. 编译完成，显示“SUCCESS”，如下图所示。

![image-20250307164622717](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250307164622717.png)

## 7. 镜像烧录

- A. 硬件搭建：Typec线将板端与PC端连接

![image-20240801173105245](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20240801173105245.png)

- B. 安装驱动“CH341SER驱动”（[CH341SER驱动下载地址](https://www.wch.cn/downloads/CH341SER_EXE.html)，**如果该链接失效或者无法下载，用户自行百度下载即可**），安装CH341SER驱动，安装前单板需要与PC端相连，点击安装即可，显示**驱动安装成功代表成功**，如果出现**驱动预安装成功代表安装失败**

![image-20240801173439645](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20240801173439645.png)

![image-20240801173618611](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20240801173618611.png)

- C. 安装成功后，在HiSpark Studio工具中点击“工程配置”按钮，选择“程序加载”，传输方式选择“serial”，端口选择“comxxx”，com口在设备管理器中查看。

![image-20250716164922699](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250716164922699.png)

- D. 配置完成后，点击工具“程序加载”按钮烧录，出现“Connecting, please reset device...”字样时，复位开发板，等待烧录结束。

![image-20250716170835615](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250716170835615.png)

- E. 在HiSpark Studio底端选择“监视器”，选择端口（**开发板需要与电脑通过typec连接**），如果没有端口显示可以刷新一下，点击“开始监视”，复位开发板，出现“flashboot version”字样代表编译烧录成功

![image-20250307171224611](../vendor/HiHope_NearLink_DK_WS63E_V03/doc/media/readme/image-20250307171224611.png)
