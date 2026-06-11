//////////////////////////////////////////////////////////////////////////////////	 
//本程序只供学习使用，未经作者许可，不得用于其它商业用途
//测试硬件：单片机CH32F203C8T6,F203C8T6最小系统开发板,主频144MHZ，晶振8MHZ
//QDtech-TFT液晶驱动 for CH32 IO模拟
//Chan@ShenZhen QDtech co.,LTD
//公司网站:www.qdtft.com
//wiki技术资料网站：http://www.lcdwiki.com
//我司提供技术支持，任何技术问题欢迎随时交流学习
//固话(传真) :+86 0755-21077707 
//手机: (销售)18823372746 （技术)15989313508
//邮箱:(销售/订单) sales@qdtft.com  (售后/技术服务)service@qdtft.com
//QQ:(售前咨询)3002706772 (技术支持)3002778157
//技术交流QQ群:778679828
//创建日期:2020/05/07
//版本：V1.0
//版权所有，盗版必究。
//Copyright(C) 深圳市全动电子技术有限公司 2018-2028
//All rights reserved
/****************************************************************************************************
//=========================================电源接线================================================//
//     LCD模块                CH32单片机
//      VCC          接        DC5V/3.3V      //电源
//      GND          接          GND          //电源地
//=======================================液晶屏数据线接线==========================================//
//本模块默认数据总线类型为SPI总线
//     LCD模块                CH32单片机    
//    SDI(MOSI)      接          PA7         //液晶屏SPI总线数据写信号
//    SDO(MISO)      接          PA6         //液晶屏SPI总线数据读信号，如果不需要读，可以不接线
//=======================================液晶屏控制线接线==========================================//
//     LCD模块 					      CH32单片机 
//      LCD_BL       接          PB6          //液晶屏背光控制信号（如果不需要控制，可以不接）
//       SCK         接          PA5         //液晶屏SPI总线时钟信号
//      LCD_RS       接          PB7          //液晶屏数据/命令控制信号
//      LCD_RST      接          PB8         //液晶屏复位控制信号
//      LCD_CS       接          PB9          //液晶屏片选控制信号
//=========================================触摸屏触接线=========================================//
//如果模块不带触摸功能或者带有触摸功能，但是不需要触摸功能，则不需要进行触摸屏接线
//	   LCD模块                CH32单片机 
//     CTP_INT       接          PA8          //电容触摸屏触摸中断信号
//     CTP_SDA       接          PA9          //电容触摸屏IIC总线数据信号
//     CTP_RST       接          PA10         //电容触摸屏触摸复位信号
//     CTP_SCL       接          PB5          //电容触摸屏IIC总线时钟信号
**************************************************************************************************/	
 /* @attention
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, QD electronic SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
**************************************************************************************************/
#include "ft6336.h"
#include "touch.h"
#include "ctpiic.h"
#include "debug.h" 
#include "string.h" 
#include "lcd.h"

extern u8 touch_flag;

/*****************************************************************************
 * @name       :u8 FT5426_WR_Reg(u16 reg,u8 *buf,u8 len)
 * @date       :2020-05-13 
 * @function   :Write data to ft5426 once
 * @parameters :reg:Start register address for written
								buf:the buffer of data written
								len:Length of data written
 * @retvalue   :0-Write succeeded 
								1-Write failed
******************************************************************************/ 
u8 FT6336_WR_Reg(u16 reg,u8 *buf,u8 len)
{
	u8 i;
	u8 ret=0;
	CTP_IIC_Start();	 
	CTP_IIC_Send_Byte(FT_CMD_WR);	//发送写命令 	 
	CTP_IIC_Wait_Ack(); 	 										  		   
	CTP_IIC_Send_Byte(reg&0XFF);   	//发送低8位地址
	CTP_IIC_Wait_Ack();  
	for(i=0;i<len;i++)
	{	   
		CTP_IIC_Send_Byte(buf[i]);  	//发数据
		ret=CTP_IIC_Wait_Ack();
		if(ret)
		{
			break;
		}
	}
  CTP_IIC_Stop();					//产生一个停止条件	    
	return ret; 
}

/*****************************************************************************
 * @name       :void FT5426_RD_Reg(u16 reg,u8 *buf,u8 len)
 * @date       :2020-05-13 
 * @function   :Read data to ft5426 once
 * @parameters :reg:Start register address for read
								buf:the buffer of data read
								len:Length of data read
 * @retvalue   :none
******************************************************************************/			  
void FT6336_RD_Reg(u16 reg,u8 *buf,u8 len)
{
	u8 i; 
 	CTP_IIC_Start();	
 	CTP_IIC_Send_Byte(FT_CMD_WR);   	//发送写命令 	 
	CTP_IIC_Wait_Ack(); 	 										  		   
 	CTP_IIC_Send_Byte(reg&0XFF);   	//发送低8位地址
	CTP_IIC_Wait_Ack();  
 	CTP_IIC_Start();  	 	   
	CTP_IIC_Send_Byte(FT_CMD_RD);   	//发送读命令		   
	CTP_IIC_Wait_Ack();	   
	for(i=0;i<len;i++)
	{	   
    	buf[i]=CTP_IIC_Read_Byte(i==(len-1)?0:1); //发数据	  
	} 
  CTP_IIC_Stop();//产生一个停止条件     
} 

/*****************************************************************************
 * @name       :u8 FT5426_Init(void)
 * @date       :2020-05-13 
 * @function   :Initialize the ft5426 touch screen
 * @parameters :none
 * @retvalue   :0-Initialization successful
								1-initialization failed
******************************************************************************/		
u8 FT6336_Init(void)
{
	u8 temp[2]; 	
	GPIO_InitTypeDef  GPIO_InitStructure;	
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA,ENABLE);//使能PC端口时钟

	GPIO_InitStructure.GPIO_Pin =GPIO_Pin_10;			// PC13端口配置
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;	//推挽输出
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
	GPIO_SetBits(GPIOA,GPIO_Pin_10);					//PC13输出1
	
	GPIO_InitStructure.GPIO_Pin=GPIO_Pin_8;				//PC1端口配置
	GPIO_InitStructure.GPIO_Mode=GPIO_Mode_IPU;			//上拉输入
	GPIO_Init(GPIOA, &GPIO_InitStructure);				//PC1上拉输入
	GPIO_SetBits(GPIOA,GPIO_Pin_8);						//上拉	

	CTP_IIC_Init();      	//初始化电容屏的I2C总线  
	FT_RST=0;				//复位
	Delay_Ms(10);
 	FT_RST=1;				//释放复位		    
	Delay_Ms(500);  	
//	temp[0]=0;
//	FT6336_WR_Reg(FT_DEVIDE_MODE,temp,1);	//进入正常操作模式 
//	FT6336_WR_Reg(FT_ID_G_MODE,temp,1);		//查询模式 
	//temp[0]=40;								//触摸有效值，22，越小越灵敏	
	//FT6336_WR_Reg(FT_ID_G_THGROUP,temp,1);	//设置触摸有效值
	FT6336_RD_Reg(FT_ID_G_FOCALTECH_ID,&temp[0],1);
	if(temp[0]!=0x11)
	{
		return 1;
	}
	FT6336_RD_Reg(FT_ID_G_CIPHER_MID,&temp[0],2);
	if(temp[0]!=0x26)
	{
		return 1;
	}
	if((temp[1]!=0x00)&&(temp[1]!=0x01)&&(temp[1]!=0x02))
	{
		return 1;
	}
	FT6336_RD_Reg(FT_ID_G_CIPHER_HIGH,&temp[0],1);
	if(temp[0]!=0x64)
	{
		return 1;
	}
//	temp[0]=12;								//激活周期，不能小于12，最大14
//	FT6336_WR_Reg(FT_ID_G_PERIODACTIVE,temp,1); 
	//读取版本号，参考值：0x3003
//	FT6336_RD_Reg(FT_ID_G_LIB_VERSION,&temp[0],2);  
//	if(temp[0]==0X10&&temp[1]==0X01)//版本:0X3003
//	{ 
//		printf("CTP ID:%x\r\n",((u16)temp[0]<<8)+temp[1]);
//		return 0;
//	} 
	return 0;
}

const u16 FT6336_TPX_TBL[2]={FT_TP1_REG,FT_TP2_REG};

/*****************************************************************************
 * @name       :u8 FT5426_Scan(void)
 * @date       :2020-05-13 
 * @function   :Scan touch screen (query mode)
 * @parameters :none
 * @retvalue   :Current touch screen status
								0-No touch
								1-With touch
******************************************************************************/	
u8 FT6336_Scan(void)
{
	u8 buf[4];
	u8 i=0;
	u8 res=0;
	u8 temp;
	u8 mode;
	static u8 t=0;//控制查询间隔,从而降低CPU占用率   
	t++;
	if((t%10)==0||t<10)//空闲时,每进入10次CTP_Scan函数才检测1次,从而节省CPU使用率
	{
		FT6336_RD_Reg(FT_REG_NUM_FINGER,&mode,1);//读取触摸点的状态  
		if(mode&&(mode<3))
		{
			temp=0XFF<<mode;//将点的个数转换为1的位数,匹配tp_dev.sta定义 
			tp_dev.sta=(~temp)|TP_PRES_DOWN|TP_CATH_PRES; 
			for(i=0;i<CTP_MAX_TOUCH;i++)
			{
				FT6336_RD_Reg(FT6336_TPX_TBL[i],buf,4);	//读取XY坐标值 
				if(tp_dev.sta&(1<<i))	//触摸有效?
				{
					switch(lcddev.dir)
					{
						case 0:
							tp_dev.x[i]=((u16)(buf[0]&0X0F)<<8)+buf[1];
							tp_dev.y[i]=((u16)(buf[2]&0X0F)<<8)+buf[3];						
							break;
						case 1:
							tp_dev.y[i]=lcddev.height-(((u16)(buf[0]&0X0F)<<8)+buf[1]);
							tp_dev.x[i]=((u16)(buf[2]&0X0F)<<8)+buf[3];						
							break;
						case 2:
							tp_dev.x[i]=lcddev.width-(((u16)(buf[0]&0X0F)<<8)+buf[1]);
							tp_dev.y[i]=lcddev.height-(((u16)(buf[2]&0X0F)<<8)+buf[3]);								
							break;
						case 3:
							tp_dev.y[i]=((u16)(buf[0]&0X0F)<<8)+buf[1];
							tp_dev.x[i]=lcddev.width-(((u16)(buf[2]&0X0F)<<8)+buf[3]);	
							break;
					} 
					//if((buf[0]&0XF0)!=0X80)tp_dev.x[i]=tp_dev.y[i]=0;//必须是contact事件，才认为有效
					//printf("x[%d]:%d,y[%d]:%d\r\n",i,tp_dev.x[i],i,tp_dev.y[i]);
				}			
			} 
			res=1;
			if(tp_dev.x[0]==0 && tp_dev.y[0]==0)mode=0;	//读到的数据都是0,则忽略此次数据
			t=0;		//触发一次,则会最少连续监测10次,从而提高命中率
		}
	}
	if(mode==0)//无触摸点按下
	{ 
		if(tp_dev.sta&TP_PRES_DOWN)	//之前是被按下的
		{
			tp_dev.sta&=~(1<<7);	//标记按键松开
		}else						//之前就没有被按下
		{ 
			tp_dev.x[0]=0xffff;
			tp_dev.y[0]=0xffff;
			tp_dev.sta&=0XE0;	//清除点有效标记	
		}	 
	} 
	if(t>240)t=10;//重新从10开始计数
	return res;
}
 





































