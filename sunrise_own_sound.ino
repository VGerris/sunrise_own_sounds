// sunrise alarm clock with custom sound and pwm led
// Copyright Vincent Gerris
// This code is available under the GPL V3 license
// have fun!

#include <TM1638.h>
#include <AnythingEEPROM.h>
#include <DS1307new.h>
#include <SPI.h>
#include <ShiftPWM.h>   // include ShiftPWM.h after setting the pins!
#include <Wire.h>
#include <EEPROM.h>

#define DS1307_I2C_ADDRESS 0x68  // This is the I2C address

// wait this many ms to return to the main "screen" showing the time;
#define MS_DISPLAY_LAST_SCREEN  4000
volatile unsigned long timeOfLastInput = 0;
boolean isWaitingForTimeout = false;
boolean needToSaveTime      = false;
boolean needToSaveDate      = false;
boolean needToSaveDow       = false;
boolean needToSaveAL1       = false;
boolean needToSaveAL2       = false;

#define _DEBUG_ 1

#define EEPROM_OFFSET  100
//buttons
#define KEY_AL1        1 << 0
#define KEY_AL2        1 << 1  // also alarm day 1 (Sunday) on/off
#define KEY_TIME       1 << 2  // also alarm day 2 on/off
#define KEY_DATE       1 << 3
#define KEY_HOUR       1 << 4
#define KEY_MIN        1 << 5
#define KEY_DECR       1 << 6
#define KEY_INCR       1 << 7

struct SettingsStruct {
  int al1_hour;
  int al1_minute;
  int al2_hour;
  int al2_minute;
  int al1_days[7];
  int al2_days[7];

  //unsigned short cyclesper_kwh;
  //unsigned short max_watt;
} settings;

void read_settings() {
  EEPROM_readAnything(EEPROM_OFFSET, settings);
  if (settings.al1_hour == 0xff) settings.al1_hour = 7;
  if (settings.al1_minute == 0xff) settings.al1_minute = 45;
  if (settings.al2_hour == 0xff) settings.al1_hour = 8;
  if (settings.al2_minute == 0xff) settings.al1_minute = 0;
}

const int ShiftPWM_latchPin=8;
const bool ShiftPWM_invertOutputs = 0; // if invertOutputs is 1, outputs will be active low. Usefull for common anode RGB led's.
const bool ShiftPWM_balanceLoad = false; //for new version of ShiftPWM

char	*dayName[]   = {"Sun", "Mon", "tUe", "Wed", "Thu", "Fri", "Sat", "Sun"};
char	*monthName[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

unsigned char maxBrightness = 255;
unsigned char pwmFrequency = 75;
int numRegisters = 3;
//int temp_hour = 0;
//int temp_minute = 0;

TM1638 display(/*dio*/ 4, /*clk*/ 5, /*stb0*/ 3);
char idletext[9] = "--------";
unsigned long restore_time = 0;
boolean settingschanged = false;
unsigned long key_debounce = 0;
char txt_buffer[10] = "";
char date_buffer[10]= "";
//char time_buffer[10]= "";

void display_text (char* text, boolean keep = true) {
  display.setDisplayToString(text);
  if (keep) strcpy(idletext, text);
}

void display_numtext (unsigned short num, char* text, boolean keep = true) {
  char numstr[9] = "";
  itoa(num, numstr, 10);
  char str[9] = "        ";
  byte width = 4;
  strcpy(&str[width - strlen(numstr)], numstr);
  strcpy(&str[width], "    ");
  strcpy(&str[8 - strlen(text)], text);
  display_text(str, keep);
}

void restore_display () {
  display_text(idletext, false);
}

void save_settings() {
  EEPROM_writeAnything(EEPROM_OFFSET, settings);
}

byte second, minute, hour, dayOfWeek, dayOfMonth, month, year;
int command = 0;       // This is the command char, in ascii form, sent from the serial port     

//My LED setup...
//
//0 - UV
//1 - Blue
//2 - Red
//3 - Red
//4 - Yellow
//5 - Yellow
//6 - White
//7 - White
//
//8,9,10,11,12,13,14,15 - White
//
//16 - UV
//17 - UV
//18 - Blue
//19 - Red
//20 - Red
//21 - Yellow
//22 - White
//23 - White

int num_led_steps = 24;
int led_on_order[24] =  {16, 0,17, 1,18, 2,19, 3,20, 4,21, 5, 8, 9, 6,22, 7,23,10,11,12,13,14,15};
int led_off_order[24] = {-1,-1,-1,-1,-1,-1,-1,-1, 1,18,-1,-1, 2,19, 3,20,-1,-1,-1,-1,-1,-1,-1,-1};

long last_action_time = 0;
int alarm1_current_position = 0;
int alarm2_current_position = 0;

int alarm1_duration = 24; //number of minutes for alarm1 to take.
int alarm2_duration = 24; //number of minutes for alarm2 to take.

int time_between_up = 1000;

long now = 0;

int i,j;

// states of the state-machine;
enum
{
  STATE_SHOW_TIME,
  STATE_SET_TIME_HOUR,
  STATE_SET_TIME_MIN,
  STATE_SET_DATE_YEAR,
  STATE_SET_DATE_MONTH,
  STATE_SET_DATE_DAY,
  STATE_SET_DOW,
  STATE_SET_AL1_HOUR,
  STATE_SET_AL1_MIN,
  STATE_SET_AL1_DAYS,
  STATE_SET_AL2_HOUR,
  STATE_SET_AL2_MIN,
  STATE_SET_AL2_DAYS,
};

// current state changes based on the user input;
int crtState = STATE_SHOW_TIME;

void setup()
{  
  //Set up ShiftPWM
  pinMode(ShiftPWM_latchPin, OUTPUT);  
  SPI.setBitOrder(LSBFIRST);
  // SPI_CLOCK_DIV2 is only a tiny bit faster in sending out the last byte. 
  // SPI transfer and calculations overlap for the other bytes.
  SPI.setClockDivider(SPI_CLOCK_DIV4); 
  SPI.begin(); 

  //Begin Wire protocol for reading from clock chip
  Wire.begin();
  
  //Serial so you can read and report the time and alarm.
  Serial.begin(9600);
  
  //read the stored alarms
  read_settings();

  //Set up the shiftPWM constants
  ShiftPWM.SetAmountOfRegisters(numRegisters);
  ShiftPWM.Start(pwmFrequency,maxBrightness);  
    
  //Everything off.  
  ShiftPWM.SetAll(0);
  
  long calc1 = (long)(alarm1_duration * 60l * 1000l);
  calc1 = calc1 / ((long)(num_led_steps * maxBrightness));
  
   time_between_up = (int)calc1;
  
  Serial.print("Value Shift Amount: ");
  Serial.println(time_between_up);
  
}

void loop()
{      
  now = millis(); //get the time now.
  byte keys = display.getButtons();
  
  if (now - last_action_time > 1000) //read and deal with the alarm only every second.
  {
    last_action_time = now;
    //getDateDs1307(); //sets the global date/time constants
    if (crtState==STATE_SHOW_TIME) display_text(txt_buffer);
    
    if (settings.al1_days[(dayOfWeek-1)] == 1)
    {    
      if ((hour*60+minute) == ((settings.al1_hour*60+settings.al1_minute)-alarm1_duration)) //if we're an appropriate number of minutes before the alarm is to be finished, start the alarm.
      {
        alarm_go();
      }
      if ((hour*60+minute) == ((settings.al1_hour*60+settings.al1_minute)+120)) //turn the lights off after 2 hours.
      {
          for (int i=maxBrightness; i>=0; i--)
          {
            ShiftPWM.SetAll(i);
            delay(100);
          }
      }
    }
    
    if (settings.al2_days[(dayOfWeek-1)] == 1)
    {    
      if ((hour*60+minute) == ((settings.al2_hour*60+settings.al2_minute)-alarm2_duration)) //if we're an appropriate number of minutes before the alarm is to be finished, start the alarm.
      {
        alarm_go();
      }
      if ((hour*60+minute) == ((settings.al2_hour*60+settings.al2_minute)+120)) //turn the lights off after 2 hours.
      {
          for (int i=maxBrightness; i>=0; i--)
          {
            ShiftPWM.SetAll(i);
            delay(100);
          }
      }
    }
    
  }  
  
  // keys  int userAction = menuButton.checkButton();
  if (keys & KEY_AL1 || (keys & KEY_AL2 || keys & KEY_TIME || keys & KEY_DATE) && crtState!=STATE_SET_AL1_DAYS && crtState!=STATE_SET_AL2_DAYS)
  {
    // MENU button was pressed;
    timeOfLastInput = millis();        // record the time;
    isWaitingForTimeout = true;

    processMenuButton(keys);     // change the machine state;
  }
  else
  {
    // MENU button not pressed; check the PLUS button, which could be
    // active for only a few seconds after the MENU button was pushed;
    if (isWaitingForTimeout)
    {
      //userAction = plusButton.checkButton();
      //if (userAction != NO_ACTION)
      if (keys & KEY_AL2 || keys & KEY_TIME || keys & KEY_DATE || keys & KEY_HOUR || keys & KEY_MIN || keys & KEY_DECR ||keys & KEY_INCR )
      {
        timeOfLastInput = millis();    // record the last time a button was pushed;
        processPlusButton(keys); // change the machine state;
      }
    }

    if (isWaitingForTimeout && abs(millis() - timeOfLastInput) > MS_DISPLAY_LAST_SCREEN)
    {
      isWaitingForTimeout = false;
      // return to the main screen (showing the time);
      crtState = STATE_SHOW_TIME;
      restore_display();
    }
  }

  ExecuteState();
      
/** 
 * With this code you can set the date/time, retreive the date/time and use the extra memory of an RTC DS1307 chip.  
 * Serial Communication method with the Arduino that utilizes a leading CHAR for each command described below. 
 * Commands:
 * T(00-59)(00-59)(00-23)(1-7)(01-31)(01-12)(00-99) - T(sec)(min)(hour)(dayOfWeek)(dayOfMonth)(month)(year) - T Sets the date of the RTC DS1307 Chip. 
 * Example to set the time for 02-Feb-09 @ 19:57:11 for the 3 day of the week, use this command - T1157193020209
 * A(00-59)(00-23)(0-1)(0-1)(0-1)(0-1)(0-1)(0-1)(0-1) 
 * Set alarm to the alarm time. each 0/1 after the hour is the day of the week to alarm or not, sunday-saturday.
*/
 // ombouwen naar gewone actie ipv serieel
  /*  if (Serial.available()) {      // Look for char in serial que and process if found
    command = Serial.read();
    if (command == 84) {      //If command = "T" Set Date
      setDateDs1307();
    } 
    if (command == 65) {      // A
      setAlarm();
    }
  }
 */
  command = 0;                 // reset command 
  delay(100);   
}

//*********************************************************************************************************
// change state of the "state machine" based on the user input;
// also perform one-time actions required by the change of state (e.g. clear display);
void processMenuButton(byte keys)
{
#ifdef _DEBUG_
  Serial.println("Pressed main button");
#endif  

  restore_display();

  switch (crtState)
  {
    case STATE_SHOW_TIME:
      if (keys & KEY_AL1)
      {
        crtState = STATE_SET_AL1_HOUR;
        needToSaveAL1 = true;
      }
      else if (keys & KEY_AL2)
      {
        crtState = STATE_SET_AL2_HOUR;
        needToSaveAL2 = true;
      }
      else if (keys & KEY_TIME)
      {
        crtState = STATE_SET_TIME_HOUR;
        needToSaveTime = true;      // further split to not touch date 
      }
      else if (keys & KEY_DATE)
      {
        crtState = STATE_SET_DATE_YEAR;
        needToSaveTime = true;     //further split to not touch time   
      }
      break;
      
    case STATE_SET_AL1_HOUR:
      if (keys & KEY_AL1)
      {
        crtState = STATE_SET_AL1_MIN;
      }
      break;
      
    case STATE_SET_AL1_MIN:
      if (keys & KEY_AL1)
      {
        crtState = STATE_SET_AL1_DAYS;
      }
      break;
      
    case STATE_SET_AL1_DAYS:
      if (keys & KEY_AL1)
      {
        crtState = STATE_SET_AL1_HOUR;
      }
      break;
      
    case STATE_SET_AL2_HOUR:
      if (keys & KEY_AL2)
      {
        crtState = STATE_SET_AL2_MIN;
      }
      break;
      
    case STATE_SET_AL2_MIN:
      if (keys & KEY_AL2)
      {
        crtState = STATE_SET_AL2_DAYS;
      }
      break;
      
    case STATE_SET_AL2_DAYS:
      if (keys & KEY_AL1)
      {
        crtState = STATE_SET_AL2_HOUR;
      }
      break;

    case STATE_SET_TIME_HOUR:
      if (keys & KEY_TIME)
      {
        crtState = STATE_SET_TIME_MIN;
      }
      break;

    case STATE_SET_TIME_MIN:
      if (keys & KEY_TIME)
      {
        crtState = STATE_SET_TIME_HOUR;
      }
      break;

    case STATE_SET_DATE_YEAR:
      if (keys & KEY_DATE)
      {
        crtState = STATE_SET_DATE_MONTH;
      }
      break;

    case STATE_SET_DATE_MONTH:
      if (keys & KEY_DATE)
      {
        crtState = STATE_SET_DATE_DAY;
      }
      break;

    case STATE_SET_DATE_DAY:
      if (keys & KEY_DATE)
      {
        crtState = STATE_SET_DOW;
      }
      break;
      
    case STATE_SET_DOW:
      if (keys & KEY_DATE)
      {
        crtState = STATE_SET_DATE_YEAR;
      }
      break;
  }
}

void processPlusButton(char keys)
{
#ifdef _DEBUG_
  Serial.println("Pressed alter button");
#endif  

  switch (crtState)
  {
    case STATE_SET_TIME_HOUR:
      if (keys & KEY_INCR)
      {
        hour++;  if (hour>23) hour = 0;
      }
      if (keys & KEY_DECR)
      {
        hour--;  if (hour<=255 && hour > 23) hour = 23;
      }
      break;

    case STATE_SET_TIME_MIN:
      if (keys & KEY_INCR)
      {
        minute++;  if (minute>59) minute = 0;
      }
      if (keys & KEY_DECR)
      {
        minute--;  if (minute<=255 && minute > 59) minute = 59;
      }
      break;
      
    case STATE_SET_AL1_HOUR:
      if (keys & KEY_INCR)
      {
        settings.al1_hour++;  if (settings.al1_hour>23) settings.al1_hour = 0;
      }
      if (keys & KEY_DECR)
      {
        settings.al1_hour--;  if (settings.al1_hour<0) settings.al1_hour = 23;
      }
      break;

    case STATE_SET_AL1_MIN:
      if (keys & KEY_INCR)
      {
        settings.al1_minute++;  if (settings.al1_minute>59) settings.al1_minute = 0;
      }
      if (keys & KEY_DECR)
      {
        settings.al1_minute--;  if (settings.al1_minute<0) settings.al1_minute = 59;
      }
      break;
      
    case STATE_SET_AL1_DAYS:
      if (keys & KEY_AL2) settings.al1_days[0]=changeDayState(settings.al1_days[0]);
      if (keys & KEY_TIME) settings.al1_days[1]=changeDayState(settings.al1_days[1]);
      if (keys & KEY_DATE) settings.al1_days[2]=changeDayState(settings.al1_days[2]);
      if (keys & KEY_HOUR) settings.al1_days[3]=changeDayState(settings.al1_days[3]);
      if (keys & KEY_MIN) settings.al1_days[4]=changeDayState(settings.al1_days[4]);
      if (keys & KEY_DECR) settings.al1_days[5]=changeDayState(settings.al1_days[5]);
      if (keys & KEY_INCR) settings.al1_days[6]=changeDayState(settings.al1_days[6]);
      // LEDS on off for days
      break;      
      
    case STATE_SET_AL2_HOUR:
      if (keys & KEY_INCR)
      {
        settings.al2_hour++;  if (settings.al2_hour>23) settings.al2_hour = 0;
      }
      if (keys & KEY_DECR)
      {
        settings.al2_hour--;  if (settings.al2_hour<0) settings.al2_hour = 23;
      }
      break;

    case STATE_SET_AL2_MIN:
      if (keys & KEY_INCR)
      {
        settings.al2_minute++;  if (settings.al2_minute>59) settings.al2_minute = 0;
      }
      if (keys & KEY_DECR)
      {
        settings.al2_minute--;  if (settings.al2_minute<0) settings.al2_minute = 59;
      }
      break;
      
    case STATE_SET_AL2_DAYS:
      if (keys & KEY_AL2) settings.al2_days[0]=changeDayState(settings.al2_days[0]);
      if (keys & KEY_TIME) settings.al2_days[1]=changeDayState(settings.al2_days[1]);
      if (keys & KEY_DATE) settings.al2_days[2]=changeDayState(settings.al2_days[2]);
      if (keys & KEY_HOUR) settings.al2_days[3]=changeDayState(settings.al2_days[3]);
      if (keys & KEY_MIN) settings.al2_days[4]=changeDayState(settings.al2_days[4]);
      if (keys & KEY_DECR) settings.al2_days[5]=changeDayState(settings.al2_days[5]);
      if (keys & KEY_INCR) settings.al2_days[6]=changeDayState(settings.al2_days[6]);
      // LEDS on off for days
      break; 

    case STATE_SET_DATE_YEAR:
      if (keys & KEY_INCR)
      {
        year++;  if (year>50) year = 13;
      }
      if (keys & KEY_DECR)
      {
        year--;  if (year<13) year = 50;
      }   
      break;

    case STATE_SET_DATE_MONTH:
      if (keys & KEY_INCR)
      {
        month++;  if (month>12) month = 1;
      }
      if (keys & KEY_DECR)
      {
        month--;  if (month<1) month = 12;
      }
      break;

    case STATE_SET_DATE_DAY:
      if (keys & KEY_INCR)
      {
        dayOfMonth++;  if (dayOfMonth>31) dayOfMonth = 1;
      }
      if (keys & KEY_DECR)
      {
        dayOfMonth--;  if (dayOfMonth<1) dayOfMonth = 31;
      }
      break;

    case STATE_SET_DOW:
      if (keys & KEY_INCR)
      {
        dayOfWeek++;  if (dayOfWeek>7) dayOfWeek = 1;
      }
      if (keys & KEY_DECR)
      {
        dayOfWeek--;  if (dayOfWeek<1) dayOfWeek = 7;
      }
      break;
  }
}

char tmp_buffer[10]=""; 


//*********************************************************************************************************
void ExecuteState()
{
#ifdef _DEBUG_
  Serial.print("Current state is ");
  Serial.println(crtState, DEC);
#endif  

  switch (crtState)
  {
    case STATE_SHOW_TIME:
      if (needToSaveTime)
      {
        //setRtcTime();
        setDateDs1307();
        needToSaveTime = false;
      }
      if (needToSaveAL1)
      {
           save_settings();  
           needToSaveAL1 = false;
      }
      if (needToSaveAL2)
      {
           save_settings();  
           needToSaveAL2 = false;
      }
      
      getDateDs1307(); //sets the global date/time constants
      display_text(txt_buffer);
      for (i=0;i < 8;i++)
        display.setLED(TM1638_COLOR_NONE, i);

      //displayCurrentTime();
      break;

    case STATE_SET_TIME_HOUR:
	  displayTimeBlink(hour, minute, 0);
      break;

    case STATE_SET_TIME_MIN:
	displayTimeBlink(hour, minute, 1);
      break;
      
    case STATE_SET_AL1_HOUR:
	  displayTimeBlink(settings.al1_hour, settings.al1_minute, 0);
      break;

    case STATE_SET_AL1_MIN:
	displayTimeBlink(settings.al1_hour, settings.al1_minute, 1);
      break;
      
    case STATE_SET_AL1_DAYS:
        displayAlarmDays(settings.al1_days);
      break;
      
    case STATE_SET_AL2_HOUR:
	  displayTimeBlink(settings.al2_hour, settings.al2_minute, 0);
      break;

    case STATE_SET_AL2_MIN:
	  displayTimeBlink(settings.al2_hour, settings.al2_minute, 1);
      break;
    case STATE_SET_AL2_DAYS:
          displayAlarmDays(settings.al2_days);
      break;

    case STATE_SET_DATE_YEAR:
	displayDateBlink(0);
      break;

    case STATE_SET_DATE_MONTH:
	displayDateBlink(1);
      break;

    case STATE_SET_DATE_DAY:
	displayDateBlink(2);
      break;

    case STATE_SET_DOW:
	displayDayBlink();
      break;
  }
}
  
void alarm_go()
{
  for(i = 0; i < num_led_steps; i++)
  {
    for (j = 0; j <= maxBrightness; j++)
    {
      ShiftPWM.SetOne(led_on_order[i],j); //follow the order of turning lights on
      {
        ShiftPWM.SetOne(led_off_order[i],maxBrightness-j);
      }
      delay(time_between_up);
    }
  }
    for (int t=0;t<10;t++)
    {
      for (i = 0; i < 7; i++)
       display.setLED(TM1638_COLOR_GREEN, i);
      delay (500);
      for (i = 0; i < 7; i++)
       display.setLED(TM1638_COLOR_RED, i);
       delay(500);
      for (i = 0; i < 7; i++)
       display.setLED(TM1638_COLOR_GREEN, i);
      delay(500);
    }
       
      
 
  
}
  
// Convert normal decimal numbers to binary coded decimal
byte decToBcd(byte val)
{
  return ( (val/10*16) + (val%10) );
}
 
// Convert binary coded decimal to normal decimal numbers
byte bcdToDec(byte val)
{
  return ( (val/16*10) + (val%16) );
}

int changeDayState(int state)
{
  if (state != 1)
    return 1;
  else return 0;
}


void getDateDs1307()
{
  // Reset the register pointer
  Wire.beginTransmission(DS1307_I2C_ADDRESS);
  Wire.write(0x00);
  Wire.endTransmission();
 
  Wire.requestFrom(DS1307_I2C_ADDRESS, 7);
 
  // A few of these need masks because certain bits are control bits
  second     = bcdToDec(Wire.read() & 0x7f);
  minute     = bcdToDec(Wire.read());
  hour       = bcdToDec(Wire.read() & 0x3f);  // Need to change this if 12 hour am/pm
  dayOfWeek  = bcdToDec(Wire.read());
  dayOfMonth = bcdToDec(Wire.read());
  month      = bcdToDec(Wire.read());
  year       = bcdToDec(Wire.read());
  
  Serial.print(">");
  Serial.print(hour, DEC);
  Serial.print(":");
  Serial.print(minute, DEC);
  Serial.print(":");
  Serial.print(second, DEC);
  Serial.print(" ");
  Serial.print(month, DEC);
  Serial.print("/");
  Serial.print(dayOfMonth, DEC);
  Serial.print("/");
  Serial.print(year, DEC);
  Serial.print(",");
  Serial.print(settings.al1_hour);
  Serial.print(":");
  Serial.print(settings.al1_minute);
  Serial.print(",");
  for (i=0;i<7;i++)
  {
    if (settings.al1_days[i] == 1)
      Serial.print("*");
    else
      Serial.print("-");
  }
  
  Serial.print(",");
  Serial.print(settings.al2_hour);
  Serial.print(":");
  Serial.print(settings.al2_minute);
  Serial.print(",");
  for (i=0;i<7;i++)
  {
    if (settings.al2_days[i] == 1)
      Serial.print("*");
    else
      Serial.print("-");
  }
  
  Serial.println();
  sprintf(txt_buffer, "%02d-%02d-%02d", hour, minute, second);
  sprintf(date_buffer, "%02d-%02d-%02d", dayOfMonth, month, year);
  //sprintf(time_buffer, "%02d-%02dSEt", hour, minute);

  //display_text(txt_buffer);
  
}

void setDateDs1307()                
{
 
   second = 0;// fix when it is null
   
   Wire.beginTransmission(DS1307_I2C_ADDRESS);
   Wire.write(0x00);
   Wire.write(decToBcd(second));    // 0 to bit 7 starts the clock
   Wire.write(decToBcd(minute));
   Wire.write(decToBcd(hour));      // If you want 12 hour am/pm you need to set
                                   // bit 6 (also need to change readDateDs1307)
   Wire.write(decToBcd(dayOfWeek));
   Wire.write(decToBcd(dayOfMonth));
   Wire.write(decToBcd(month));
   Wire.write(decToBcd(year));
   Wire.endTransmission();
}

void displayTimeBlink(int hour, int minute, int whichToBlink)
{
  sprintf(tmp_buffer, "%02d-%02d   ", hour, minute);
  //*	blink the digits that we are setting;
  if (minute > 59) minute = 23;
  if (hour > 23) hour = 23;

  if (((millis() / 250)  % 2) == 0)
	{
		if (whichToBlink == 0)  // blinking hours
		{
			sprintf(tmp_buffer, "  -%02d   ", minute);
		}
		else
		{
			sprintf(tmp_buffer, "%02d-     ", hour);
		}
	}
  display_text(tmp_buffer);
}

//*********************************************************************************************************
// whichToBlink could be 0, 1 or 2, for year, month, day;
//
void displayDateBlink(int whichToBlink)
{
  sprintf(tmp_buffer, "%02d-%02d-%02d", dayOfMonth, month, year);
  //*	blink the digits that we are setting;
  if (((millis() / 250)  % 2) == 0)
	{
		if (whichToBlink == 0)  // blinking year
		{
			sprintf(tmp_buffer, "%02d-%02d-  ", dayOfMonth,month);
		}
		else if (whichToBlink == 1)  // blinking month
		{
			sprintf(tmp_buffer, "%02d-  -%02d", dayOfMonth,year);
		}
                else if (whichToBlink == 2) // blinking dayOfMonth
		{
			sprintf(tmp_buffer, "  -%02d-%02d", month, year);
		}
                
	}
  display_text(tmp_buffer);
}


//*********************************************************************************************************
void displayDayBlink()
{
	// copy the name of the day; blanks before and after to clear the screen around the 3-letter day;
	char buffer[10] = "        ";
	strcpy(buffer, dayName[dayOfWeek]);

	//*	blink the day that we are setting;
	if (((millis() / 250)  % 2) == 0)
	{
		strcpy(buffer, "        ");
	}

	// display the day (3 letters);
        display_text(buffer);    
}

void displayAlarmDays(int status[7])
{  
    for (i = 0; i < 7; i++)
      if (status[i] ==1)
        display.setLED(TM1638_COLOR_GREEN, i+1);
      else display.setLED(TM1638_COLOR_RED, i+1);
        
  // led red green
}


