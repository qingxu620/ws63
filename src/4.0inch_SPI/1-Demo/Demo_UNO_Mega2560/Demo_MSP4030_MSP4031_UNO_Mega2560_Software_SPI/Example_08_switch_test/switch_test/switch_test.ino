// IMPORTANT: LCDWIKI_SPI LIBRARY AND LCDWIKI_TOUCH LIBRARY MUST BE SPECIFICALLY
// CONFIGURED FOR EITHER THE TFT SHIELD OR THE BREAKOUT BOARD.

//This program is a demo of showing switch

//when using the BREAKOUT BOARD only and using these software spi lines to the LCD,
//if you don't need to control the LED pin,you can set it to 3.3V and set the pin definition to -1.
//other pins can be defined by youself,for example
//pin usage as follow:
//pin usage as follow:
//                      CS  DC/RS  RESET  SDI/MOSI  SCK  SDO/MISO  LED    VCC     GND    
//Arduino Uno&Mega2560  10    9      8       11     13      12      5   5V/3.3V   GND 
//                      CTP_INT  CTP_SDA  CTP_RST  CTP_SCL
//Arduino Uno&Mega2560     7        A4       6        A5

//Remember to set the pins to suit your display module!

/**********************************************************************************
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
#include "switch_font.c"

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

#define BUTTON_X 40
#define BUTTON_Y 100
#define BUTTON_W 60
#define BUTTON_H 30
#define BUTTON_SPACING_X 20
#define BUTTON_SPACING_Y 20
#define BUTTON_TEXTSIZE 2

boolean switch_flag_1 = true,switch_flag_2 = true,switch_flag_3 = true,switch_flag_4 = true,switch_flag_5 = true,switch_flag_6 = true;  
int16_t menu_flag = 1,old_menu_flag;     

void show_string(uint8_t *str,int16_t x,int16_t y,uint8_t csize,uint16_t fc, uint16_t bc,boolean mode)
{
    my_lcd.Set_Text_Mode(mode);
    my_lcd.Set_Text_Size(csize);
    my_lcd.Set_Text_colour(fc);
    my_lcd.Set_Text_Back_colour(bc);
    my_lcd.Print_String(str,x,y);
}

void show_picture(const uint8_t *color_buf,int16_t buf_size,int16_t x1,int16_t y1,int16_t x2,int16_t y2)
{
    my_lcd.Set_Addr_Window(x1, y1, x2, y2); 
    my_lcd.Push_Any_Color(color_buf, buf_size, 1, 1);
}

boolean is_pressed(int16_t x1,int16_t y1,int16_t x2,int16_t y2,int16_t px,int16_t py)
{
    if((px > x1 && px < x2) && (py > y1 && py < y2))
    {
        my_tp.x[0] = 0xFFFF;
        my_tp.y[0] = 0xFFFF;
        return true;  
    } 
    else
    {
        return false;  
    }
 }

void show_system_menu(void)
{    
    show_string("time setting",10,150,2,BLACK, BLACK,1);
    show_string("date setting",10,170,2,BLACK, BLACK,1);
    show_string("alarm setting",10,190,2,BLACK, BLACK,1);
    show_string("talk setting",10,210,2,BLACK, BLACK,1);
    show_string("sound setting",10,230,2,BLACK, BLACK,1);
    switch(menu_flag)
    {
      case 1:
      {
          my_lcd.Set_Draw_color(BLUE);
          my_lcd.Fill_Rectangle(0, 147, my_lcd.Get_Display_Width()-1, 166);
          show_string("time setting",10,150,2,WHITE, BLACK,1);
          break;
      }
      case 2:
      {
          my_lcd.Set_Draw_color(BLUE);
          my_lcd.Fill_Rectangle(0, 167, my_lcd.Get_Display_Width()-1, 186);
          show_string("date setting",10,170,2,WHITE, BLACK,1);
          break;
      }
      case 3:
      {
          my_lcd.Set_Draw_color(BLUE);
          my_lcd.Fill_Rectangle(0, 187, my_lcd.Get_Display_Width()-1, 206);
          show_string("alarm setting",10,190,2,WHITE, BLACK,1);
          break;
      }
      case 4:
      {
          my_lcd.Set_Draw_color(BLUE);
          my_lcd.Fill_Rectangle(0, 207, my_lcd.Get_Display_Width()-1, 226);
          show_string("talk setting",10,210,2,WHITE, BLACK,1);
          break;
      }
      case 5:
      {
          my_lcd.Set_Draw_color(BLUE);
          my_lcd.Fill_Rectangle(0, 227, my_lcd.Get_Display_Width()-1, 246);
          show_string("sound setting",10,230,2,WHITE, BLACK,1);
          break;
      }
      default:
        break;
    }
}
                    
void setup(void) 
{    
 my_lcd.Init_LCD();
 my_lcd.Set_Rotation(0);
 if(my_tp.FT6336_Init(my_lcd.Get_Rotation(),my_lcd.Get_Display_Width(),my_lcd.Get_Display_Height()))
 {
    return;
 }
 my_lcd.Fill_Screen(WHITE); 
 
 my_lcd.Set_Draw_color(192, 192, 192);
 my_lcd.Draw_Fast_HLine(0, 3, my_lcd.Get_Display_Width());
 show_picture(switch_on_2,sizeof(switch_on_2)/2,5,5,34,34);
 show_string("switch is on ",60,11,2,GREEN, BLACK,1);

 my_lcd.Draw_Fast_HLine(0, 37, my_lcd.Get_Display_Width());
 
 show_string("wifi setting",5,40,2,BLACK, BLACK,1);
 show_picture(switch_on_3,sizeof(switch_on_3)/2,195,40,234,54);

  my_lcd.Draw_Fast_HLine(0, 57, my_lcd.Get_Display_Width());
 
 show_string("bt setting",5,60,2,BLACK, BLACK,1);
 show_picture(switch_on_3,sizeof(switch_on_3)/2,195,60,234,74);

 my_lcd.Draw_Fast_HLine(0, 77, my_lcd.Get_Display_Width());
 
 show_string("auto time",5,80,2,BLACK, BLACK,1);
 show_picture(switch_on_1,sizeof(switch_on_1)/2,204,80,218,94);

 my_lcd.Draw_Fast_HLine(0, 97, my_lcd.Get_Display_Width());
 
 show_string("enable lock",5,100,2,BLACK, BLACK,1);
 show_picture(switch_on_1,sizeof(switch_on_1)/2,204,100,218,114);

 my_lcd.Draw_Fast_HLine(0, 116, my_lcd.Get_Display_Width());
 
 show_string("system setting   >",5,119,2,BLUE, BLACK,1);
 my_lcd.Draw_Fast_HLine(0, 138, my_lcd.Get_Display_Width());
}

void loop(void)
{
  my_tp.FT6336_Scan();
  //for(t=0; t<OTT_MAX_TOUCH;t++)
  //{ 
    if (my_tp.ctp_status&(1<<0)) 
    {
     // px = my_tp.x[t];
     // py = my_tp.y[t];
     
      if(is_pressed(5,5,34,34,my_tp.x[0],my_tp.y[0]))
      {
          if(switch_flag_1)
          {
              show_picture(switch_off_2,sizeof(switch_off_2)/2,5,5,34,34);
              my_lcd.Set_Draw_color(WHITE);
              my_lcd.Fill_Rectangle(60, 11,  216, 27);
              show_string("switch is off ",60,11,2,RED, BLACK,1); 
              switch_flag_1 = false;
           }
           else
           {
             show_picture(switch_on_2,sizeof(switch_on_2)/2,5,5,34,34);
             my_lcd.Set_Draw_color(WHITE);
             my_lcd.Fill_Rectangle(60, 11,  216, 27);
             show_string("switch is on ",60,11,2,GREEN, BLACK,1);
             switch_flag_1 = true;
           }
           delay(1);
       }
       if(is_pressed(195,40,234,54,my_tp.x[0],my_tp.y[0]))
       {
          if(switch_flag_2)
          {
              show_picture(switch_off_3,sizeof(switch_off_3)/2,195,40,234,54);
              switch_flag_2 = false;
          }
          else
          {
             show_picture(switch_on_3,sizeof(switch_on_3)/2,195,40,234,54);
             switch_flag_2 = true;
          }
          delay(1);
        }
        if(is_pressed(195,60,234,74,my_tp.x[0],my_tp.y[0]))
        {
           if(switch_flag_3)
          {
              show_picture(switch_off_3,sizeof(switch_off_3)/2,195,60,234,74);
              switch_flag_3 = false;
          }
          else
          {
             show_picture(switch_on_3,sizeof(switch_on_3)/2,195,60,234,74);
             switch_flag_3 = true;
          }
          //delay(100);
         }
         if(is_pressed(205,81,217,93,my_tp.x[0],my_tp.y[0]))
         {
           if(switch_flag_4)
           {
             show_picture(switch_off_1,sizeof(switch_off_1)/2,204,80,218,94);
              switch_flag_4 = false;
           }
           else
           {
             show_picture(switch_on_1,sizeof(switch_on_1)/2,204,80,218,94);
              switch_flag_4 = true;
           }
          delay(1);
         }
         if(is_pressed(205,101,217,113,my_tp.x[0],my_tp.y[0]))
         {
           if(switch_flag_5)
           {
            show_picture(switch_off_1,sizeof(switch_off_1)/2,204,100,218,114);
            switch_flag_5 = false;
           }
           else
           {
             show_picture(switch_on_1,sizeof(switch_on_1)/2,204,100,218,114);
             switch_flag_5 = true;
            }
           delay(1);
         }
         if(is_pressed(5,119,my_lcd.Get_Display_Width()-1,137,my_tp.x[0],my_tp.y[0]))
         {
              my_lcd.Set_Draw_color(MAGENTA);
              my_lcd.Fill_Rectangle(0, 117, my_lcd.Get_Display_Width()-1, 137);
              //delay(100);
              my_lcd.Set_Draw_color(WHITE);
              my_lcd.Fill_Rectangle(0, 117, my_lcd.Get_Display_Width()-1, 137);
              if(switch_flag_6)
              {
                  show_string("system setting   <",5,119,2,BLUE, BLACK,1);
                  show_system_menu();
                  switch_flag_6 = false;
              }
              else
              {
                   show_string("system setting   >",5,119,2,BLUE, BLACK,1);
                   my_lcd.Set_Draw_color(WHITE);
                   my_lcd.Fill_Rectangle(0, 147, my_lcd.Get_Display_Width()-1, 250);
                   switch_flag_6 = true;
              }
         }
         if(!switch_flag_6)
         {
            old_menu_flag = menu_flag;     
            if(is_pressed(0,147,my_lcd.Get_Display_Width()-1,166,my_tp.x[0],my_tp.y[0]))
            {
                my_lcd.Set_Draw_color(BLUE);
                my_lcd.Fill_Rectangle(0, 147, my_lcd.Get_Display_Width()-1, 166);
                show_string("time setting",10,150,2,WHITE, BLACK,1);
                menu_flag = 1;
             }
             if(is_pressed(0,167,my_lcd.Get_Display_Width()-1,186,my_tp.x[0],my_tp.y[0]))
             {
                my_lcd.Set_Draw_color(BLUE);
                my_lcd.Fill_Rectangle(0, 167, my_lcd.Get_Display_Width()-1, 186);
                show_string("date setting",10,170,2,WHITE, BLACK,1);
                menu_flag = 2;
             }
             if(is_pressed(0,187,my_lcd.Get_Display_Width()-1,206,my_tp.x[0],my_tp.y[0]))
             {
                my_lcd.Set_Draw_color(BLUE);
                my_lcd.Fill_Rectangle(0, 187, my_lcd.Get_Display_Width()-1, 206);
                show_string("alarm setting",10,190,2,WHITE, BLACK,1);
                menu_flag = 3;
             }
             if(is_pressed(0,207,my_lcd.Get_Display_Width()-1,226,my_tp.x[0],my_tp.y[0]))
             {
                 my_lcd.Set_Draw_color(BLUE);
                 my_lcd.Fill_Rectangle(0, 207, my_lcd.Get_Display_Width()-1, 226);
                show_string("talk setting",10,210,2,WHITE, BLACK,1);
                menu_flag = 4;
             }
             if(is_pressed(0,227,my_lcd.Get_Display_Width()-1,246,my_tp.x[0],my_tp.y[0]))
             {
                my_lcd.Set_Draw_color(BLUE);
                my_lcd.Fill_Rectangle(0, 227, my_lcd.Get_Display_Width()-1, 246);
                show_string("sound setting",10,230,2,WHITE, BLACK,1);
                menu_flag = 5;
             }  
             if(old_menu_flag != menu_flag)
             {
                switch(old_menu_flag)
                {
                  case 1:
                  {
                      my_lcd.Set_Draw_color(WHITE);
                      my_lcd.Fill_Rectangle(0, 147, my_lcd.Get_Display_Width()-1, 166);
                      show_string("time setting",10,150,2,BLACK, BLACK,1);
                      break;
                  }
                  case 2:
                  {
                      my_lcd.Set_Draw_color(WHITE);
                      my_lcd.Fill_Rectangle(0, 167, my_lcd.Get_Display_Width()-1, 186);
                      show_string("date setting",10,170,2,BLACK, BLACK,1);
                      break;
                  }
                  case 3:
                  {
                      my_lcd.Set_Draw_color(WHITE);
                      my_lcd.Fill_Rectangle(0, 187, my_lcd.Get_Display_Width()-1, 206);
                      show_string("alarm setting",10,190,2,BLACK, BLACK,1);
                      break;
                  }
                  case 4:
                  {
                      my_lcd.Set_Draw_color(WHITE);
                      my_lcd.Fill_Rectangle(0, 207, my_lcd.Get_Display_Width()-1, 226);
                      show_string("talk setting",10,210,2,BLACK, BLACK,1);
                      break;
                  }
                  case 5:
                  {
                      my_lcd.Set_Draw_color(WHITE);
                      my_lcd.Fill_Rectangle(0, 227, my_lcd.Get_Display_Width()-1, 246);
                      show_string("sound setting",10,230,2,BLACK, BLACK,1);
                      break;
                  }
                  default:
                    break;                  
               }       
           }
           //delay(10);
       }
    }
  //}
}
