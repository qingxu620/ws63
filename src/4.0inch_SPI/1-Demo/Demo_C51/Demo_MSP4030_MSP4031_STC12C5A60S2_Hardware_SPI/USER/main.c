//////////////////////////////////////////////////////////////////////////////////	 
//本程序只供学习使用，未经作者许可，不得用于其它任何用途
//测试硬件：单片机STC12C5A60S2,晶振11.0592M  单片机工作电压5V
//QDtech-TFT液晶驱动 for C51
//xiao冯@ShenZhen QDtech co.,LTD
//公司网站:www.qdtft.com
//淘宝网站：http://qdtech.taobao.com
//wiki技术网站：http://www.lcdwiki.com
//我司提供技术支持，任何技术问题欢迎随时交流学习
//固话(传真) :+86 0755-23594567 
//手机:15989313508（冯工） 
//邮箱:lcdwiki01@gmail.com    support@lcdwiki.com    goodtft@163.com
//技术支持QQ:3002773612  3002778157
//技术交流QQ群:324828016
//创建日期:2018/7/7
//版本：V1.0
//版权所有，盗版必究。
//Copyright(C) 深圳市全动电子技术有限公司 2018-2028
//All rights reserved
//********************************************************************************
//=========================================电源接线================================================//
//5V接DC 5V电源
//GND接地
//=======================================液晶屏数据线接线==========================================//
//本模块默认数据总线类型为SPI
//液晶屏模块              单片机
// SDI(MOSI)      接       P15       //SPI写信号
// SDO(MISO)      接       P16       //SPI读信号，如果不需要读功能，此管脚可不接
//=======================================液晶屏控制线接线==========================================//
//液晶屏模块              单片机
//  LCD_CS        接       P13       //液晶屏片选控制信号
//  LCD_RST       接       P33       //液晶屏复位控制信号
//  LCD_RS        接       P12       //液晶屏数据/命令控制信号
//  SCK           接       P17       //SPI时钟信号
//  LED           接       P32       //液晶屏背光控制信号（如果不需要控制，可以不接）
//=========================================触摸屏接线=========================================//
//不使用触摸或者模块本身不带触摸，则可不连接
//触摸屏模块              单片机
//  CTP_SCL       接       P36       //电容触摸屏IIC总线时钟信号
//  CTP_RST       接       P37       //电容触摸屏触摸复位信号
//  CTP_SDA       接       P34       //电容触摸屏IIC总线数据信号
//  CTP_INT       接       P35       //电容触摸屏触摸中断信号
//**************************************************************************************************/	
 /* @attention
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, QD electronic SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
**************************************************************************************************/
#include "sys.h"
#include "lcd.h"
#include "gui.h"
#include "test.h"
#include "touch.h"



//主函数
void main(void)
{ 
	//液晶屏初始化
	LCD_Init();

	//循环进行各项测试	
	while(1)
	{
		main_test(); 		    //测试主界面
//		Test_Read();        //读ID和GRAM测试（有异常）
		Test_Color();  		  //简单刷屏填充测试
		Test_FillRec();		  //GUI矩形绘图测试
		Test_Circle(); 		  //GUI画圆测试
		Test_Triangle();    //GUI三角形绘图测试
		English_Font_test();//英文字体示例测试
		Chinese_Font_test();//中文字体示例测试
		Pic_test();			    //图片显示示例测试
		Test_Dynamic_Num(); //动态数字显示
		Rotate_Test();      //旋转显示测试
		//如果不带触摸，或者不需要触摸功能，请注释掉下面触摸屏测试项
		Touch_Test();		    //触摸屏手写测试  
	}
}
