/*******************************************************************************
* Author:         Gladyshev Dmitriy (2020) 
* 
* Create Date:    22.10.2020  
* Design Name:    Светодиодные часы с метеостанцией
* Version:        3.0
* Target Devices: ESP8266 (Nodemcu)
* Tool versions:  Arduino 1.8.13
*
* URL:            https://19dx.ru/2020/10/esp8266-meteoclock/
* 
*******************************************************************************/

#define SECS_PER_HOUR (3600UL)

//Библиотека часов
#include <DS1307.h> 
// Библиотеки ESP8266
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
//Библиотеки для светодиодной матрицы
#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>
//Библиотека для беспроводного датчика https://github.com/invandy/Oregon_NR
// http://arduino.ru/forum/proekty/chtenie-i-emulyatsiya-datchikov-oregon-scientific-433mhz
#include <Oregon_NR.h>

// =========================================================================
#define ssid                   "********"             // Имя WiFi сети
#define password               "********"             // Пароль WiFi
#define TIMEZONE               7                      // Временная зона
#define ntpServerName          "ntp1.stratum2.ru"     // NTP сервер
#define BRIGHTNESS             7                      // Яркость дисплея (255 максимальная)
#define MATRIX_PIN             14                     // Номер GPIO подключения матрицы (14 = D5)
#define SENSOR_RF_PIN          12                     // Номер GPIO подключения приёмника беспроводного датчика (12 = D6)
#define SDA                    5                      // Номер GPIO шины I2C SDA (5 = D1)
#define SCL                    4                      // Номер GPIO шины I2C SCL (4 = D2)
#define ENABLE_DEBUG_RF        0                      // 1 - включает "осциллограф" - отображение данных, полученных с приёмника
#define ENABLE_DEBUG_DECODER   0                      // 1 - выводить в Serial сервисную информацию
// =========================================================================

//Массив количества дней в месяцах
uint8_t RTC_Months[2][12] = {
  {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
  {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
};

//Структура времени
struct tms
{
  int8_t          Seconds; /**< seconds after the minute - [ 0 to 59 ] */
  int8_t          Minutes; /**< minutes after the hour - [ 0 to 59 ] */
  int8_t          Hours; /**< hours since midnight - [ 0 to 23 ] */
  int8_t          Day; /**< day of the month - [ 1 to 31 ] */
  int8_t          WeekDay; /**< days since Sunday - [ 0 to 6 ] */
  int8_t          Month; /**< months since January - [ 0 to 11 ] */
  int16_t         Year;
};

int Mode = 1;           // Текущий режим отображения: 1 - часы, 2 - дата, 3 - температура
bool FirstSync = true;  // Флаг первой синхронизации после включения
IPAddress timeServerIP;
bool DotTimeState = true; //Состояние отображения разделителя часов
bool Transition = true; // флаг необходимости воспроизведения перехода 
unsigned long lastTimeModeSwitch = 0;

int CurrentTemp = 1000; //Текущая температура на улице (1000 = значит данные ещё не поступали)
unsigned long lastTimeSensorReceive = 0; //Время получения последнего сообщения от датчика

//Буфер для NTP сообщений
const int NTP_PACKET_SIZE = 48;      // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
unsigned int localPort = 53;         // local port to listen for UDP packets

String TimeStr = "--:--";
String TimeStrBlank = "-- --";
String DateStr = "--.--.----";

Oregon_NR oregon(SENSOR_RF_PIN, SENSOR_RF_PIN,          // приёмник на выводе GPIO12
                    255, true,    // Светодиод на D2 подтянут к +пит(true). Если светодиод не нужен, то номер вывода - 255
                    50, true);  // Буфер на приём посылки из 50 ниблов, включена сборка пакетов для v2
  

DS1307 rtc(SDA, SCL);
WiFiClient espClient;
WiFiUDP udp;
Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(32, 8, MATRIX_PIN,
  NEO_MATRIX_BOTTOM + NEO_MATRIX_RIGHT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
  NEO_GRB + NEO_KHZ800);

//Цвета для каждого режима в формате RGB
const uint16_t colors[] = {
  matrix.Color(0, 255, 0), 
  matrix.Color(255, 255, 0), 
  matrix.Color(0, 0, 255)
};

void sendNTPpacket(IPAddress& address);
bool isLeapYear(int y);
void WiFiEvent(WiFiEvent_t event);

/*******************************************************************************
* Function Name  : setup_wifi
* Description    : Настройка WiFi соединения
*******************************************************************************/

void setup_wifi() 
{
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.onEvent(WiFiEvent);

  randomSeed(micros());

  Serial.println("Starting UDP");
  udp.begin(localPort);
}

/*******************************************************************************
* Function Name  : setup
* Description    : 
*******************************************************************************/

void setup()
{
  rtc.halt(false);
  Serial.begin(115200);
  setup_wifi();
  
  matrix.begin();
  matrix.setTextWrap(false);
  matrix.setBrightness(BRIGHTNESS);
  matrix.setTextColor(colors[1]);

  //вкючение прослушивания радиоканала  
  oregon.start(); 
  oregon.receiver_dump = ENABLE_DEBUG_RF;       

  //RTC.set(now());
}

/*******************************************************************************
* Function Name  : loop
* Description    : Главный цикл
*******************************************************************************/

void loop() 
{
  //Каждые 500 мс запрашиваем время и дату
  static unsigned long lastGetRTC = 0;
  if (millis() - lastGetRTC >= 500)
  {
    lastGetRTC = millis();
    TimeStr = rtc.getTimeStr(FORMAT_SHORT);
    //String TimeStrBlank(TimeStr);
    TimeStrBlank = TimeStr;
    TimeStrBlank.replace(":", " ");
    DateStr = rtc.getDateStr();
    DateStr.remove(DateStr.length()-5);
    DateStr.replace(".", "/");
  }

  if (millis() - lastTimeSensorReceive >= 600000) //10 минут нет данных от датчика
  {
    CurrentTemp = 1000;
  }

  //Показываем часы
  if (Mode == 1)
  {
    //Плавный переход
    if (Transition)
    {
      for (int i = 0; i <= BRIGHTNESS; i++)
      {
        matrix.fillScreen(0);
        matrix.setTextColor(colors[0]);
        matrix.setCursor(1, 0);
        matrix.print(TimeStr);
        matrix.setBrightness(i);
        matrix.show();
        delay(30);
      }
      Transition = false;
    }
      
    static unsigned long lastTimeUpdate = millis();
    if (millis() - lastTimeUpdate >= 500)
    {
      lastTimeUpdate = millis();
      DotTimeState = !DotTimeState;
      matrix.fillScreen(0);
      matrix.setTextColor(colors[0]);
      matrix.setCursor(1, 0);
      if (DotTimeState)
        matrix.print(TimeStr);
      else
        matrix.print(TimeStrBlank);
      matrix.show();
    }

    if (millis() - lastTimeModeSwitch >= 15000)
    {
      for (int i = BRIGHTNESS; i >= 0; i--)
      {
        matrix.setBrightness(i);
        matrix.show();
        delay(30);
      }
      
      lastTimeModeSwitch = millis();
      Mode = 2;
      Transition = true;
      //matrix.setBrightness(7);
    }
  }

  //Показываем дату
  if (Mode == 2)
  {
    if (Transition)
    {
      for (int i = 0; i <= BRIGHTNESS; i++)
      {
        matrix.fillScreen(0);
        matrix.setTextColor(colors[1]);
        matrix.setCursor(1, 0);
        matrix.print(DateStr);
        matrix.setBrightness(i);
        matrix.show();
        delay(30);
      }
      Transition = false;
    }

    if (millis() - lastTimeModeSwitch >= 5000)
    {
      for (int i = BRIGHTNESS; i >= 0; i--)
      {
        matrix.setBrightness(i);
        matrix.show();
        delay(30);
      }
      
      Mode = 3;
      lastTimeModeSwitch = millis();
      Transition = true;
    }
  }

  //Показываем температуру
  if (Mode == 3)
  {
    if (Transition)
    {
      for (int i = 0; i <= BRIGHTNESS; i++)
      {
        matrix.fillScreen(0);
        matrix.setTextColor(colors[2]);
        matrix.setCursor(1, 0);
        if (CurrentTemp == 1000)
        {
          matrix.print(" -- C");
          matrix.drawCircle(22, 1, 1, colors[2]);
        }
        else
        {
          if (CurrentTemp > 0)
          {
            matrix.print("+");
          }
          else if (CurrentTemp == 0)
          {
            matrix.print(" ");
          }
          matrix.print(CurrentTemp);
          if ((CurrentTemp >= -9) && (CurrentTemp <= 9))
          {
            matrix.print(" ");
          }
          matrix.print(" C");
          matrix.drawCircle(22, 1, 1, colors[2]);
        }
        matrix.setBrightness(i);
        matrix.show();
        delay(30);
      }
      Transition = false;
    }

    if (millis() - lastTimeModeSwitch >= 5000)
    {
      for (int i = BRIGHTNESS; i >= 0; i--)
      {
        matrix.setBrightness(i);
        matrix.show();
        delay(30);
      }
      
      Mode = 1;
      lastTimeModeSwitch = millis();
      Transition = true;
    }
  }

  if (WiFi.status() == WL_CONNECTED)
  {

    static unsigned long lastSync = millis();
    //Синхронизация времени
    if ((FirstSync && (millis() - lastSync >= 15000)) || (millis() - lastSync >= 86400000))
    {
      WiFi.hostByName(ntpServerName, timeServerIP);
      sendNTPpacket(timeServerIP); // send an NTP packet to a time server
      // wait to see if a reply is available
      delay(1000);
      int cb = udp.parsePacket();
      if (!cb) {
        Serial.println("no packet yet");
      } else {
        FirstSync = false;
        Serial.print("packet received, length=");
        Serial.println(cb);
        // We've received a packet, read the data from it
        udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer
    
        //the timestamp starts at byte 40 of the received packet and is four bytes,
        // or two words, long. First, esxtract the two words:
    
        unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
        unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
        // combine the four bytes (two words) into a long integer
        // this is NTP time (seconds since Jan 1 1900):
        unsigned long secsSince1900 = highWord << 16 | lowWord /*+ TIMEZONE * SECS_PER_HOUR*/;
        Serial.print("Seconds since Jan 1 1900 = ");
        Serial.println(secsSince1900);
    
        // now convert NTP time into everyday time:
        Serial.print("Unix time = ");
        // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
        const unsigned long seventyYears = 2208988800UL;
        // subtract seventy years:
        unsigned long unixtime = secsSince1900 - seventyYears + TIMEZONE * SECS_PER_HOUR;
        // print Unix time:
        Serial.println(unixtime);

        uint16_t year;

        struct tms data;
        data.Seconds = unixtime % 60;  // Get seconds from unixtime
        unixtime /= 60;                 // Go to minutes
        data.Minutes = unixtime % 60;  // Get minutes
        unixtime /= 60;                 // Go to hours
        data.Hours = unixtime % 24;    // Get hours
        unixtime /= 24;                 // Go to days
        data.WeekDay = (unixtime + 3) % 7 + 1; // Get week day, monday is first day

        year = 1970;                // Process year
        while (1) 
        {
          if (isLeapYear(year)) 
          {
            if (unixtime >= 366) 
            {
              unixtime -= 366;
            } 
            else 
            {
              break;
            }
          } 
          else if (unixtime >= 365) 
          {
            unixtime -= 365;
          } 
          else 
          {
            break;
          }
          year++;
        }

        data.Year = year;
        // Get month
        for (data.Month = 0; data.Month < 12; data.Month++) 
        {
          if (isLeapYear(year)) {
            if (unixtime >= (uint32_t)RTC_Months[1][data.Month]) 
            {
              unixtime -= RTC_Months[1][data.Month];
            } 
            else 
            {
              break;
            }
          } 
          else if (unixtime >= (uint32_t)RTC_Months[0][data.Month]) 
          {
            unixtime -= RTC_Months[0][data.Month];
          } 
          else 
          {
            break;
          }
        }

        data.Month++;            // Month starts with 1
        data.Day = unixtime + 1;     // Date starts with 1

        //Записываем данные в RTC
        rtc.setTime(data.Hours, data.Minutes, data.Seconds);
        rtc.setDate(data.Day, data.Month, data.Year);

        Serial.print("The UTC time is ");
        Serial.print(data.Hours);
        Serial.print(":");
        Serial.print(data.Minutes);
        Serial.print(":");
        Serial.print(data.Seconds);
        Serial.print("   ");
        Serial.print(data.Day);
        Serial.print("/");
        Serial.print(data.Month);
        Serial.print("/");
        Serial.print(data.Year);
        Serial.println();
      }
      lastSync = millis();
    }

  }

  //Дукодируем данные от беспроводного датчика
  oregon_decoder();
}

/*******************************************************************************
* Function Name  : oregon_decoder
* Description    : Приём и обработка данных от беспроводного датчика
*******************************************************************************/

void oregon_decoder()
{
  //////////////////////////////////////////////////////////////////////
  //Захват пакета,/////////////////////////////////////////////////////
  oregon.capture(ENABLE_DEBUG_DECODER); // 1 - выводить в Serial сервисную информацию
  
  //Захваченные данные годны до следующего вызова capture
  //ОБработка полученного пакета//////////////////////////////////////////////
  if (oregon.captured)  {
    //Вывод информации в Serial
    Serial.print ((float) millis() / 1000, 1); //Время
    Serial.print ("s\t\t");
    //Версия протокола
    if (oregon.ver == 2) Serial.print("  ");
    if (oregon.ver == 3) Serial.print("3 ");
    
    //Информация о восстановлени пакета
    if (oregon.restore_sign & 0x01) Serial.print("s"); //восстановлены одиночные такты
    else  Serial.print(" ");
    if (oregon.restore_sign & 0x02) Serial.print("d"); //восстановлены двойные такты
    else  Serial.print(" ");
    if (oregon.restore_sign & 0x04) Serial.print("p "); //исправленна ошибка при распознавании версии пакета
    else  Serial.print("  ");
    if (oregon.restore_sign & 0x08) Serial.print("r "); //собран из двух пакетов (для режима сборки в v.2)
    else  Serial.print("  ");

    //Вывод полученного пакета.
    for (int q = 0;q < oregon.packet_length; q++)
      if (oregon.valid_p[q] == 0x0F) Serial.print(oregon.packet[q], HEX);
      else Serial.print(" ");
        
    //Время обработки пакета
    Serial.print("  ");
    Serial.print(oregon.work_time);
    Serial.print("ms ");
    
    if ((oregon.sens_type == THGN132 ||
    (oregon.sens_type & 0x0FFF) == RTGN318 ||
    (oregon.sens_type & 0x0FFF) == RTHN318 ||
    oregon.sens_type == THGR810 ||
    oregon.sens_type == THN132 ||
    oregon.sens_type == THN800 ||
    oregon.sens_type == BTHGN129 ||
    oregon.sens_type == BTHR968 ||
    oregon.sens_type == THGN500) && oregon.crc_c){
      Serial.print("\t");
      
      if (oregon.sens_type == THGN132) Serial.print("THGN132N");
      if (oregon.sens_type == THGN500) Serial.print("THGN500 ");
      if (oregon.sens_type == THGR810) Serial.print("THGR810 ");
      if ((oregon.sens_type & 0x0FFF) == RTGN318) Serial.print("RTGN318 ");
      if ((oregon.sens_type & 0x0FFF) == RTHN318) Serial.print("RTHN318 ");
      if (oregon.sens_type == THN132 ) Serial.print("THN132N ");
      if (oregon.sens_type == THN800 ) Serial.print("THN800  ");
      if (oregon.sens_type == BTHGN129 ) Serial.print("BTHGN129");
      if (oregon.sens_type == BTHR968 ) Serial.print("BTHR968 ");

      if (oregon.sens_type != BTHR968 && oregon.sens_type != THGN500)
      {
        Serial.print(" CHNL: ");
        Serial.print(oregon.sens_chnl);
      }
      else Serial.print("        ");
      Serial.print(" BAT: ");
      if (oregon.sens_battery) Serial.print("F "); else Serial.print("e ");
      Serial.print("ID: ");
      Serial.print(oregon.sens_id, HEX);
      
      if (oregon.sens_tmp >= 0 && oregon.sens_tmp < 10) Serial.print(" TMP:  ");
      if (oregon.sens_tmp < 0 && oregon.sens_tmp >-10) Serial.print(" TMP: ");
      if (oregon.sens_tmp <= -10) Serial.print(" TMP:");
      if (oregon.sens_tmp >= 10) Serial.print(" TMP: ");

      CurrentTemp = oregon.sens_tmp;
      lastTimeSensorReceive = millis();

      Serial.print(oregon.sens_tmp, 1);
      Serial.print("C ");
      if (oregon.sens_type == THGN132 ||
          oregon.sens_type == THGR810 ||
          oregon.sens_type == BTHGN129 ||
          oregon.sens_type == BTHR968 ||
          (oregon.sens_type & 0x0FFF) == RTGN318 ||
          oregon.sens_type == THGN500 ) {
        Serial.print("HUM: ");
        Serial.print(oregon.sens_hmdty, 0);
        Serial.print("%");
      }
      else Serial.print("        ");

      if (oregon.sens_type == BTHGN129 ||  oregon.sens_type == BTHR968)
      {
      Serial.print(" PRESS: ");
      Serial.print(oregon.get_pressure(), 1);
      Serial.print("Hgmm ");
      }
    }

  if (oregon.sens_type == WGR800 && oregon.crc_c){
      Serial.print("\tWGR800  ");
      Serial.print("        ");
      Serial.print(" BAT: ");
      if (oregon.sens_battery) Serial.print("F "); else Serial.print("e ");
      Serial.print("ID: ");
      Serial.print(oregon.sens_id, HEX);
      
      Serial.print(" AVG: ");
      Serial.print(oregon.sens_avg_ws, 1);
      Serial.print("m/s  MAX: ");
      Serial.print(oregon.sens_max_ws, 1);
      Serial.print("m/s  DIR: "); //N = 0, E = 4, S = 8, W = 12
      switch (oregon.sens_wdir)
      {
      case 0: Serial.print("N"); break;
      case 1: Serial.print("NNE"); break;
      case 2: Serial.print("NE"); break;
      case 3: Serial.print("NEE"); break;
      case 4: Serial.print("E"); break;
      case 5: Serial.print("SEEE"); break;
      case 6: Serial.print("SE"); break;
      case 7: Serial.print("SSE"); break;
      case 8: Serial.print("S"); break;
      case 9: Serial.print("SSW"); break;
      case 10: Serial.print("SW"); break;
      case 11: Serial.print("SWW"); break;
      case 12: Serial.print("W"); break;
      case 13: Serial.print("NWW"); break;
      case 14: Serial.print("NW"); break;
      case 15: Serial.print("NNW"); break;
      }
      
    }    

    if (oregon.sens_type == UVN800 && oregon.crc_c){
      Serial.print("\tUVN800  ");
      Serial.print("        ");
      Serial.print(" BAT: ");
      if (oregon.sens_battery) Serial.print("F "); else Serial.print("e ");
      Serial.print(" ID: ");
      Serial.print(oregon.sens_id, HEX);
      
      Serial.print(" UVI: ");
      Serial.print(oregon.UV_index);
      
    }    


    if (oregon.sens_type == RFCLOCK && oregon.crc_c){
      Serial.print("\tCLOCK   ");
      Serial.print(" CHNL: ");
      Serial.print(oregon.sens_chnl);
      Serial.print(" BAT: ");
      if (oregon.sens_battery) Serial.print("F "); else Serial.print("e ");
      Serial.print(" ID: ");
      Serial.print(oregon.sens_id, HEX);
      Serial.print(" TIME: ");
      Serial.print(oregon.packet[13], HEX);
      Serial.print(oregon.packet[12], HEX);
      Serial.print(':');
      Serial.print(oregon.packet[11], HEX);
      Serial.print(oregon.packet[10], HEX);
      Serial.print(':');
       Serial.print(oregon.packet[9], HEX);
      Serial.print(oregon.packet[8], HEX);
      Serial.print(" DATE: ");
      Serial.print(oregon.packet[15], HEX);
      Serial.print(oregon.packet[14], HEX);
      Serial.print('.');
      if (oregon.packet[17] ==1 || oregon.packet[17] ==3)   Serial.print('1');
      else Serial.print('0');
      Serial.print(oregon.packet[16], HEX);
      Serial.print('.');
      Serial.print(oregon.packet[19], HEX);
      Serial.print(oregon.packet[18], HEX);
    }    

    if (oregon.sens_type == PCR800 && oregon.crc_c){
      Serial.print("\tPCR800  ");
      Serial.print("        ");
      Serial.print(" BAT: ");
      if (oregon.sens_battery) Serial.print("F "); else Serial.print("e ");
      Serial.print(" ID: ");
      Serial.print(oregon.sens_id, HEX);
      Serial.print("   TOTAL: ");
      Serial.print(oregon.get_total_rain(), 1);
      Serial.print("mm  RATE: ");
      Serial.print(oregon.get_rain_rate(), 1);
      Serial.print("mm/h");
      
    }    
    
#if ADD_SENS_SUPPORT == 1
      if ((oregon.sens_type & 0xFF00) == THP && oregon.crc_c) {
      Serial.print("\tTHP     ");
      Serial.print(" CHNL: ");
      Serial.print(oregon.sens_chnl);
      Serial.print(" BAT: ");
      Serial.print(oregon.sens_voltage, 2);
      Serial.print("V");
      if (oregon.sens_tmp > 0 && oregon.sens_tmp < 10) Serial.print(" TMP:  ");
      if (oregon.sens_tmp < 0 && oregon.sens_tmp > -10) Serial.print(" TMP: ");
      if (oregon.sens_tmp <= -10) Serial.print(" TMP:");
      if (oregon.sens_tmp >= 10) Serial.print(" TMP: ");
      Serial.print(oregon.sens_tmp, 1);
      Serial.print("C ");
      Serial.print("HUM: ");
      Serial.print(oregon.sens_hmdty, 1);
      Serial.print("% ");
      Serial.print("PRESS: ");
      Serial.print(oregon.sens_pressure, 1);
      Serial.print("Hgmm");
      
    }
#endif
    Serial.println();
  }
  yield();
}


/*******************************************************************************
* Function Name  : sendNTPpacket
* Description    : Отправка NTP пакета
*******************************************************************************/

void sendNTPpacket(IPAddress& address)
{
  Serial.println("sending NTP packet...");
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  udp.beginPacket(address, 123); //NTP requests are to port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
}

/*******************************************************************************
* Function Name  : isLeapYear
* Description    : Возвращает true, если год високосный
*******************************************************************************/

bool isLeapYear(int y)
{
   bool r = false;
   if (y % 4 == 0) {
      if (y % 100 == 0) {
         if (y % 400 == 0) {
            r = true;
         }
      } 
      else r = true;
   }
   return r;
}

/*******************************************************************************
* Function Name  : WiFiEvent
* Description    : Обработка изменений состояния Wi-Fi соединения.
*                  Вызывается автоматически.
*******************************************************************************/
 
void WiFiEvent(WiFiEvent_t event) 
{
  // 0 WIFI_EVENT_STAMODE_CONNECTED    подключение к роутеру получение ip
  // 1 WIFI_EVENT_STAMODE_DISCONNECTED попытка переподключения к роутеру
  // 2 WIFI_EVENT_STAMODE_AUTHMODE_CHANGE
  // 3 WIFI_EVENT_STAMODE_GOT_IP подключен к роутеру
  // 4 WIFI_EVENT_STAMODE_DHCP_TIMEOUT Не получен адрес DHCP
  // 5 WIFI_EVENT_SOFTAPMODE_STACONNECTED подключен клент
  // 6 WIFI_EVENT_SOFTAPMODE_STADISCONNECTED отключен клент
  // 7 WIFI_EVENT_SOFTAPMODE_PROBEREQRECVED Режим точки доступа
  // 8 WIFI_EVENT_MAX,
  // 9 WIFI_EVENT_ANY = WIFI_EVENT_MAX,
  // 10 WIFI_EVENT_MODE_CHANGE
  
  Serial.printf("[WiFi-event] event: %d\n", event);
 
  switch(event) {
    case WIFI_EVENT_STAMODE_GOT_IP:
      Serial.println(F("[WiFi-event] WiFi connected"));
      Serial.print(F("IP address: "));
      Serial.println(WiFi.localIP());
      break;
    case WIFI_EVENT_STAMODE_DISCONNECTED:
      Serial.println(F("[WiFi-event] WiFi lost connection"));
      break;
  }
}