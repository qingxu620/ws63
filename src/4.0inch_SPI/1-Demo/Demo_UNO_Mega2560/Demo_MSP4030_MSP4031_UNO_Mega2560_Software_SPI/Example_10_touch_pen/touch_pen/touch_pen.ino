// IMPORTANT: LCDWIKI_SPI LIBRARY AND LCDWIKI_TOUCH LIBRARY MUST BE SPECIFICALLY
// CONFIGURED FOR EITHER THE TFT SHIELD OR THE BREAKOUT BOARD.

//This program is a demo of drawing

//when using the BREAKOUT BOARD only and using these software spi lines to the LCD,
//if you don't need to control the LED pin,you can set it to 3.3V and set the pin definition to -1.
//other pins can be defined by youself,for example
//pin usage as follow:
//                      CS  DC/RS  RESET  SDI/MOSI  SCK  SDO/MISO  LED    VCC     GND    
//Arduino Uno&Mega2560  10    9      8       11     13      12      5   5V/3.3V   GND 
//                      CTP_INT  CTP_SDA  CTP_RST  CTP_SCL
//Arduino Uno&Mega2560     7        A4       6        A5

//Remember to set the pins to suit your display module!

/*********************************************************************************
* @attention
*
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
* TIME. AS A RESULT, QD electronic SHALL NOT BE HELD LIABLE FOR ANY
* DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
* FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE 
* CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
**********************************************************************************/

#include <LCDWIKI_GUI.h> //Core graphics library
#include <LCDWIKI_SPI.h> //Hardware-specific library
#include <ft6336g.h> //touch library

//paramters define
#define MODEL ST7796S
#define CS   10    
#define CD   9
#define RST  8
#define MOSI  11
#define MISO  12
#define SCK   13
#define LED  5   //if you don't need to control the LED pin,you should set it to -1 and set it to 3.3V

//touch screen paramters define
#define OTT_MAX_TOUCH  2 
#define INT  7
#define CRST 6 
#define SCL  A5
#define SDA  A4

//the definiens of software spi mode as follow:
//if the IC model is known or the modules is unreadable,you can use this constructed function
LCDWIKI_SPI my_lcd(MODEL,CS,CD,MISO,MOSI,RST,SCK,LED); //model,cs,dc,miso,mosi,reset,sck,led

//the definiens of touch mode as follow:
FT6336 my_tp(INT,CRST,SCL,SDA);

#define  BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0
#define WHITE   0xFFFF

uint16_t color_mask[] = {0xF800,0x001F}; //color select

void show_string(uint8_t *str,int16_t x,int16_t y,uint8_t csize,uint16_t fc, uint16_t bc,boolean mode)
{
    my_lcd.Set_Text_Mode(mode);
    my_lcd.Set_Text_Size(csize);
    my_lcd.Set_Text_colour(fc);
    my_lcd.Set_Text_Back_colour(bc);
    my_lcd.Print_String(str,x,y);
}

void LCD_Draw_Line(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t Size,uint16_t colour)
{
  uint16_t t; 
  int xerr=0,yerr=0,delta_x,delta_y,distance; 
  int incx,incy,uRow,uCol; 
  if(x1<Size||x2<Size||y1<Size||y2<Size)
  {
    return;  
  }
  delta_x=x2-x1; //计算坐标增量 
  delta_y=y2-y1; 
  uRow=x1; 
  uCol=y1; 
  if(delta_x>0)incx=1; //设置单步方向 
  else if(delta_x==0)incx=0;//垂直线 
  else {incx=-1;delta_x=-delta_x;} 
  if(delta_y>0)incy=1; 
  else if(delta_y==0)incy=0;//水平线 
  else{incy=-1;delta_y=-delta_y;} 
  if( delta_x>delta_y)distance=delta_x; //选取基本增量坐标轴 
  else distance=delta_y; 
  for(t=0;t<=distance+1;t++ )//画线输出 
  {  
    my_lcd.Set_Draw_color(colour);
     my_lcd.Fill_Circle(uRow, uCol, Size);
    //gui_circle(uRow, uCol,color, size, 1);
    //LCD_DrawPoint(uRow,uCol);//画点 
    xerr+=delta_x ; 
    yerr+=delta_y ; 
    if(xerr>distance) 
    { 
      xerr-=distance; 
      uRow+=incx; 
    } 
    if(yerr>distance) 
    { 
      yerr-=distance; 
      uCol+=incy; 
    } 
  }  
}

void setup(void) 
{
  Serial.begin(9600);
  my_lcd.Set_Rotation(0);  
  my_lcd.Init_LCD();
  Serial.println(my_lcd.Read_ID(), HEX);
  if(my_tp.FT6336_Init(my_lcd.Get_Rotation(),my_lcd.Get_Display_Width(),my_lcd.Get_Display_Height()))
  {
    return;  
  }
  my_lcd.Fill_Screen(WHITE);
  show_string("RST",my_lcd.Get_Display_Width()-36,0,2,BLUE, BLACK,1);
  my_lcd.Set_Draw_color(RED);
}

void loop()
{
  uint16_t lastpos[2];
  while(1)
  { 
    my_tp.FT6336_Scan();
    //for(t=0; t<OTT_MAX_TOUCH;t++)
   // {   
      if(my_tp.ctp_status&(1<<0)) 
      {
        if(my_tp.x[0]<my_lcd.Get_Display_Width()&&my_tp.y[0]<my_lcd.Get_Display_Height()) 
        {
          if(lastpos[0]==0xFFFF) 
          {
             lastpos[0]=my_tp.x[0];
             lastpos[1]=my_tp.y[0];
           }
           if(my_tp.x[0]>(my_lcd.Get_Display_Width()-36)&&my_tp.y[0]<16)
           {
              my_tp.x[0] = 0xFFFF;
              my_tp.y[0] = 0xFFFF;
              my_lcd.Fill_Screen(WHITE);
              show_string("RST",my_lcd.Get_Display_Width()-36,0,2,BLUE, BLACK,1);
              my_lcd.Set_Draw_color(RED);
            } 
            else
            {
                    //  my_lcd.Set_Draw_color(color_mask[t]);
                //my_lcd.Fill_Circle(my_tp.x[t],my_tp.y[t],3);
                LCD_Draw_Line(lastpos[0],lastpos[1],my_tp.x[0],my_tp.y[0],2,color_mask[0]);
            }          
           lastpos[0]=my_tp.x[0];
           lastpos[1]=my_tp.y[0];
          }
       }
       else
       {
          lastpos[0]=0XFFFF;
        }
    // }
  }
}
