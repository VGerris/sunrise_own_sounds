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

#define _DEBUG_ 1

#define EEPROM_OFFSET  100
//buttons
#define KEY_AL1         1 << 0
#define KEY_AL2       1 << 1
#define KEY_TIME       1 << 2
#define KEY_DATE       1 << 3
#define KEY_HOUR        1 << 4
#define KEY_MIN      1 << 5
#define KEY_DECR       1 << 6
#define KEY_INCR       1 << 7

struct SettingsStruct {
  unsigned short  al1_hour;
  unsigned short  al1_minutes;
  unsigned short  al2_hour;
  unsigned short  al2_minutes;
  char al1_days[9];
  char al2_days[9];

  //unsigned short cyclesper_kwh;
  //unsigned short max_watt;
} settings;

const int ShiftPWM_latchPin=8;
const bool ShiftPWM_invertOutputs = 0; // if invertOutputs is 1, outputs will be active low. Usefull for common anode RGB led's.
const bool ShiftPWM_balanceLoad = false; //for new version of ShiftPWM

char	*dayName[]   = {"Sun", "Mon", "tUe", "Wed", "Thu", "Fri", "Sat", "Sun"};
char	*monthName[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

unsigned char maxBrightness = 255;
unsigned char pwmFrequency = 75;
int numRegisters = 3;
int temp_hour = 0;
int temp_minute = 0;

TM1638 display(/*dio*/ 4, /*clk*/ 5, /*stb0*/ 3);
char idletext[9] = "--------";
unsigned long restore_time = 0;
boolean settingschanged = false;
unsigned long key_debounce = 0;
char txt_buffer[10] = "";
char date_buffer[10]= "";
char time_buffer[10]= "";

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


int alarm_hour = 0;
int alarm_minute = 0;
int days_to_alarm[7] = {0,0,0,0,0,0,0};
int alarm_current_position = 0;

long last_action_time = 0;

int alarm_duration = 24; //number of minutes for alarm to take.
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
  
  //read the stored alarm time.
  alarm_hour = EEPROM.read(0);
  alarm_minute = EEPROM.read(1);
  days_to_alarm[0] = EEPROM.read(2);
  days_to_alarm[1] = EEPROM.read(3);
  days_to_alarm[2] = EEPROM.read(4);
  days_to_alarm[3] = EEPROM.read(5);
  days_to_alarm[4] = EEPROM.read(6);
  days_to_alarm[5] = EEPROM.read(7);
  days_to_alarm[6] = EEPROM.read(8);

  //Set up the shiftPWM constants
  ShiftPWM.SetAmountOfRegisters(numRegisters);
  ShiftPWM.Start(pwmFrequency,maxBrightness);  
    
  //Everything off.  
  ShiftPWM.SetAll(0);
  
  long calc = (long)(alarm_duration * 60l * 1000l);
  calc = calc / ((long)(num_led_steps * maxBrightness));
  
   time_between_up = (int)calc;
  
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
    
    if (days_to_alarm[(dayOfWeek-1)] == 1)
    {    
      if ((hour*60+minute) == ((alarm_hour*60+alarm_minute)-alarm_duration)) //if we're an appropriate number of minutes before the alarm is to be finished, start the alarm.
      {
        alarm_go();
      }
      if ((hour*60+minute) == ((alarm_hour*60+alarm_minute)+120)) //turn the lights off after 2 hours.
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
  if (keys & KEY_TIME || keys & KEY_DATE)
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
      if (keys & KEY_INCR || keys & KEY_DECR)
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
  Serial.println("Pressed MENU button");
#endif  

  restore_display();
  needToSaveTime = true;

  switch (crtState)
  {
    case STATE_SHOW_TIME:
      if (keys & KEY_TIME)
      {
        crtState = STATE_SET_TIME_HOUR;
      }
      else if (keys & KEY_DATE)
      {
        crtState = STATE_SET_DATE_YEAR;
      }
/*
      else if (userAction == BTN_DBL_PUSH)
      {
        crtState = STATE_SET_DOW;
      }
*/
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
  Serial.println("Pressed PLUS button");
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
          getDateDs1307(); //sets the global date/time constants
      display_text(txt_buffer);
      //displayCurrentTime();
      break;

    case STATE_SET_TIME_HOUR:
	  displayTimeBlink(hour, minute, 0);
      break;

    case STATE_SET_TIME_MIN:
	displayTimeBlink(hour, minute, 1);
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
  Serial.print(alarm_hour);
  Serial.print(":");
  Serial.print(alarm_minute);
  Serial.print(",");
  for (i=0;i<7;i++)
  {
    if (days_to_alarm[i] == 1)
      Serial.print("*");
    else
      Serial.print("-");
  }
  Serial.println();
  sprintf(txt_buffer, "%02d-%02d-%02d", hour, minute, second);
  sprintf(date_buffer, "%02d-%02d-%02d", dayOfMonth, month, year);
  sprintf(time_buffer, "%02d-%02dSEt", hour, minute);
  //temp_hour = hour;
  //temp_minute = minute;
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

void setAlarm()
{
   alarm_minute = (byte) ((Serial.read() - 48) *10 +  (Serial.read() - 48));
         Serial.print(alarm_minute);

   alarm_hour  = (byte) ((Serial.read() - 48) *10 +  (Serial.read() - 48));
   days_to_alarm[0]  = (byte)  (Serial.read() - 48);
   days_to_alarm[1]  = (byte)  (Serial.read() - 48);
   days_to_alarm[2]  = (byte)  (Serial.read() - 48);
   days_to_alarm[3]  = (byte)  (Serial.read() - 48);
   days_to_alarm[4]  = (byte)  (Serial.read() - 48);
   days_to_alarm[5]  = (byte)  (Serial.read() - 48);
   days_to_alarm[6]  = (byte)  (Serial.read() - 48);
   EEPROM.write(0,alarm_hour);
   EEPROM.write(1,alarm_minute);
   EEPROM.write(2,days_to_alarm[0]);
   EEPROM.write(3,days_to_alarm[1]);
   EEPROM.write(4,days_to_alarm[2]);
   EEPROM.write(5,days_to_alarm[3]);
   EEPROM.write(6,days_to_alarm[4]);
   EEPROM.write(7,days_to_alarm[5]);
   EEPROM.write(8,days_to_alarm[6]);
   
}

void setAlarm1()
{
   alarm_minute = (byte) ((Serial.read() - 48) *10 +  (Serial.read() - 48));
   alarm_hour  = (byte) ((Serial.read() - 48) *10 +  (Serial.read() - 48));
   days_to_alarm[0]  = (byte)  (Serial.read() - 48);
   days_to_alarm[1]  = (byte)  (Serial.read() - 48);
   days_to_alarm[2]  = (byte)  (Serial.read() - 48);
   days_to_alarm[3]  = (byte)  (Serial.read() - 48);
   days_to_alarm[4]  = (byte)  (Serial.read() - 48);
   days_to_alarm[5]  = (byte)  (Serial.read() - 48);
   days_to_alarm[6]  = (byte)  (Serial.read() - 48);
   EEPROM.write(0,alarm_hour);
   EEPROM.write(1,alarm_minute);
   EEPROM.write(2,days_to_alarm[0]);
   EEPROM.write(3,days_to_alarm[1]);
   EEPROM.write(4,days_to_alarm[2]);
   EEPROM.write(5,days_to_alarm[3]);
   EEPROM.write(6,days_to_alarm[4]);
   EEPROM.write(7,days_to_alarm[5]);
   EEPROM.write(8,days_to_alarm[6]);
   
}
