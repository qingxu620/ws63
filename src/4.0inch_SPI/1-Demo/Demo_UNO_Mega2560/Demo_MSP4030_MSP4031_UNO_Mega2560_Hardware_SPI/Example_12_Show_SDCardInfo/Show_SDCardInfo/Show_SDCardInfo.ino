// IMPORTANT: LCDWIKI_SPI LIBRARY MUST BE SPECIFICALLY
// CONFIGURED FOR EITHER THE TFT SHIELD OR THE BREAKOUT BOARD.

//This program is a demo of Showing SD card information

//when using the BREAKOUT BOARD only and using these hardware spi lines to the LCD,
//the SDA pin and SCK pin is defined by the system and can't be modified.
//if you don't need to control the LED pin,you can set it to 3.3V and set the pin definition to -1.
//other pins can be defined by youself,for example
//pin usage as follow:
//                  CS  DC/RS  RESET  SDI/MOSI  SCK  SDO/MISO  SD_CS  LED    VCC     GND    
//Arduino Uno       10    9      8       11     13      12       4     5   5V/3.3V   GND                  
//Arduino Mega2560  10    9      8       51     52      50       4     5   5V/3.3V   GND
//                  CTP_INT  CTP_SDA  CTP_RST  CTP_SCL
//Arduino Uno         7        A4       6        A5
//Arduino Mega2560    7        20       6        21

//Remember to set the pins to suit your display module!

/***********************************************************************************
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
#include <SD.h>

//paramters define
#define MODEL ST7796S
#define CS   10    
#define CD   9
#define RST  8
#define SD_CS 4
#define LED  5   //if you don't need to control the LED pin,you should set it to -1 and set it to 3.3V

//define some colour values
#define  BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0
#define WHITE   0xFFFF

//the definiens of hardware spi mode as follow:
//if the IC model is known or the modules is unreadable,you can use this constructed function
LCDWIKI_SPI mylcd(MODEL,CS,CD,RST,LED); //model,cs,dc,reset,led

// set up variables using the SD utility library functions:
Sd2Card card;
SdVolume volume;
SdFile root;

void show_string(uint8_t *str,int16_t x,int16_t y,uint8_t csize,uint16_t fc, uint16_t bc,boolean mode)
{
    mylcd.Set_Text_Mode(mode);
    mylcd.Set_Text_Size(csize);
    mylcd.Set_Text_colour(fc);
    mylcd.Set_Text_Back_colour(bc);
    mylcd.Print_String(str,x,y);
}

void setup()
{
 // Open serial communications and wait for port to open:
  Serial.begin(9600);

  mylcd.Init_LCD();
  mylcd.Fill_Screen(BLACK);

//  Serial.print("\nInitializing SD card...");
  pinMode(SD_CS, OUTPUT);     


  // we'll use the initialization code from the utility libraries
  // since we're just testing if the card is working!
  if (!card.init(SPI_HALF_SPEED, SD_CS)) {
    mylcd.Fill_Screen(BLUE);
    show_string("initialization failed!!",10,10,1,RED, BLUE,0);
    return;
  } 

  // print the type of card
  show_string("Card type:",10,10,2,WHITE, BLACK,0);
  switch(card.type()) {
    case SD_CARD_TYPE_SD1:
      show_string("SD1",130,10,2,WHITE, BLACK,0);
      break;
    case SD_CARD_TYPE_SD2:
      show_string("SD2",130,10,2,WHITE, BLACK,0);
      break;
    case SD_CARD_TYPE_SDHC:
      show_string("SDHC",130,10,2,WHITE, BLACK,0);
      break;
    default:
      show_string("Unknown",130,10,2,WHITE, BLACK,0);
  }

  // Now we will try to open the 'volume'/'partition' - it should be FAT16 or FAT32
  if (!volume.init(card)) {
    //Serial.println("Could not find FAT16/FAT32 partition.\nMake sure you've formatted the card");
    show_string("Could not find FAT16/FAT32 partition.",10,30,1,RED, BLACK,0);
    show_string("Make sure you've formatted the card.",10,40,1,RED, BLACK,0);
    return;
  }


  // print the type and size of the first FAT-type volume
  uint32_t volumesize;
  show_string("Volume type is FAT",10,30,2,WHITE, BLACK,0);
  mylcd.Print_Number_Int(volume.fatType(), 226, 30, 0, ' ', 10);
 
  volumesize = volume.blocksPerCluster();    // clusters are collections of blocks
  volumesize *= volume.clusterCount();       // we'll have a lot of clusters
  volumesize *= 512;                            // SD card blocks are always 512 bytes
  
  show_string("size (Kbytes): ",10,70,2,WHITE, BLACK,0);
  volumesize /= 1024;
  mylcd.Print_Number_Int(volumesize, 181, 70, 0, ' ', 10);

  show_string("size (Mbytes): ",10,90,2,WHITE, BLACK,0);
  volumesize /= 1024;
  mylcd.Print_Number_Int(volumesize, 181, 90, 0, ' ', 10);
}


void loop(void) {
  
}
