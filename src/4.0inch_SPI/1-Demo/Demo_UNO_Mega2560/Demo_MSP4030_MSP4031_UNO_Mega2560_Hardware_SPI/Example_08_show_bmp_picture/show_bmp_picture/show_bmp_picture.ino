/***********************************************************************************
*This program is a demo of how to display a bmp picture from SD card
*This demo was made for LCD modules with 8bit or 16bit data port.
*This program requires the the LCDKIWI library.

* File                : show_bmp_picture.ino
* Hardware Environment: Arduino UNO
* Build Environment   : Arduino

*Set the pins to the correct ones for your development shield or breakout board.
*This demo use the BREAKOUT BOARD only and use these 8bit data lines to the LCD,
*pin usage as follow:
*                   CS  DC/RS  RESET  SDI/MOSI  SCK  SDO/MISO  SD_CS  LED    VCC     GND    
* Arduino Uno       10    9      8       11     13      12       4     5   5V/3.3V   GND                  
* Arduino Mega2560  10    9      8       51     52      50       4     5   5V/3.3V   GND

*Remember to set the pins to suit your display module!
*
* @attention
*
* THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
* WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
* TIME. AS A RESULT, QD electronic SHALL NOT BE HELD LIABLE FOR ANY
* DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
* FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE 
* CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
**********************************************************************************/

#include <SD.h>
#include <SPI.h>
#include <LCDWIKI_GUI.h> //Core graphics library
#include <LCDWIKI_SPI.h> //Hardware-specific library
//paramters define
#define MODEL ST7796S
#define CS   10    
#define CD   9
#define RST  8
#define SD_CS  4
#define LED  5   //if you don't need to control the LED pin,you should set it to -1 and set it to 3.3V

//the definiens of hardware spi mode as follow:
//if the IC model is known or the modules is unreadable,you can use this constructed function
LCDWIKI_SPI my_lcd(MODEL,CS,CD,RST,LED); //model,cs,dc,reset,led

#define  BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0
#define WHITE   0xFFFF

#define PIXEL_NUMBER  (my_lcd.Get_Display_Width()/4)
#define FILE_NUMBER 4
#define FILE_NAME_SIZE_MAX 20

uint32_t bmp_offset = 0;
uint16_t s_width = my_lcd.Get_Display_Width();  
uint16_t s_heigh = my_lcd.Get_Display_Height();
//int16_t PIXEL_NUMBER;

char file_name[FILE_NUMBER][FILE_NAME_SIZE_MAX];

uint16_t read_16(File fp)
{
    uint8_t low;
    uint16_t high;
    low = fp.read();
    high = fp.read();
    return (high<<8)|low;
}

uint32_t read_32(File fp)
{
    uint16_t low;
    uint32_t high;
    low = read_16(fp);
    high = read_16(fp);
    return (high<<8)|low;   
 }
 
bool analysis_bpm_header(File fp)
{
    if(read_16(fp) != 0x4D42)
    {
      return false;  
    }
    //get bpm size
    read_32(fp);
    //get creator information
    read_32(fp);
    //get offset information
    bmp_offset = read_32(fp);
    //get DIB infomation
    read_32(fp);
    //get width and heigh information
    uint32_t bpm_width = read_32(fp);
    uint32_t bpm_heigh = read_32(fp);
    if((bpm_width != s_width) || (bpm_heigh != s_heigh))
    {
      return false; 
    }
    if(read_16(fp) != 1)
    {
        return false;
    }
    read_16(fp);
    if(read_32(fp) != 0)
    {
      return false; 
     }
     return true;
}

void draw_bmp_picture(File fp)
{
  uint16_t i,j,k,l,m=0;
  uint8_t bpm_data[PIXEL_NUMBER*3] = {0};
  uint16_t bpm_color[PIXEL_NUMBER];
  fp.seek(bmp_offset);
  for(i = 0;i < s_heigh;i++)
  {
    for(j = 0;j<s_width/PIXEL_NUMBER;j++)
    {
      m = 0;
      fp.read(bpm_data,PIXEL_NUMBER*3);
      for(k = 0;k<PIXEL_NUMBER;k++)
      {
        bpm_color[k]= my_lcd.Color_To_565(bpm_data[m+2], bpm_data[m+1], bpm_data[m+0]); //change to 565
        m +=3;
      }
      for(l = 0;l<PIXEL_NUMBER;l++)
      {
        my_lcd.Set_Draw_color(bpm_color[l]);
        my_lcd.Draw_Pixel(j*PIXEL_NUMBER+l,i);
      }    
     }
   }    
}

void setup() 
{
    Serial.begin(9600);
   my_lcd.Init_LCD();
   //Serial.println(my_lcd.Read_ID(), HEX);
   my_lcd.Fill_Screen(BLUE);
   //s_width = my_lcd.Get_Display_Width();  
   //s_heigh = my_lcd.Get_Display_Height();
   //PIXEL_NUMBER = my_lcd.Get_Display_Width()/4;
   if(PIXEL_NUMBER == 60) //240*320
   {
       strcpy(file_name[0],"tulip.bmp");
       strcpy(file_name[1],"game.bmp");
       strcpy(file_name[2],"tree.bmp");
       strcpy(file_name[3],"flower.bmp");
   }
   else //320*480
   {
       strcpy(file_name[0],"01.bmp");
       strcpy(file_name[1],"02.bmp");
       strcpy(file_name[2],"03.bmp");
       strcpy(file_name[3],"04.bmp");
   }
  //Init SD_Card
   pinMode(SD_CS, OUTPUT);
   
    if (!SD.begin(SD_CS)) 
    {
      my_lcd.Set_Text_Back_colour(BLUE);
      my_lcd.Set_Text_colour(WHITE);    
      my_lcd.Set_Text_Size(1);
      my_lcd.Print_String("SD Card Init fail!",0,0);
    }
}

void loop() 
{
    int i = 0;
    File bmp_file;
    for(i = 0;i<FILE_NUMBER;i++)
    {
       bmp_file = SD.open(file_name[i]);
       if(!bmp_file)
       {
            my_lcd.Set_Text_Back_colour(BLUE);
            my_lcd.Set_Text_colour(WHITE);    
            my_lcd.Set_Text_Size(1);
            my_lcd.Print_String("didnt find BMPimage!",0,10);
            while(1);
        }
        if(!analysis_bpm_header(bmp_file))
        {  
            my_lcd.Set_Text_Back_colour(BLUE);
            my_lcd.Set_Text_colour(WHITE);    
            my_lcd.Set_Text_Size(1);
            my_lcd.Print_String("bad bmp picture!",0,0);
            return;
        }
          draw_bmp_picture(bmp_file);
         bmp_file.close(); 
         delay(2000);
     }
}
