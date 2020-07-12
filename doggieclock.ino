/***************************************************
 * Doggie accident clock for Adafruit ePaper FeatherWings
 * For use with Adafruit tricolor and monochrome ePaper FeatherWings
 *
 * Written by Matthew R. Johnson
 *
 * Notes:
 * Update the secrets.h file with your WiFi details
 * Uncomment the ePaper display type you are using below.
 * Change the DEFAULT_SLEEP_SEC setting to define the update interval
 */
#include <string.h>
#include <cstring>
#include <stdio.h>

#include <Adafruit_GFX.h>
#include <Adafruit_EPD.h>
#include <ArduinoJson.h>
#include <TimeLib.h>

#include <HTTPClient.h>
#include <EEPROM.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <Fonts/Org_01.h>

#include "secrets.h"  // Contains definitions for WIFI_SSID and WIFI_PASSWORD


/* ========== USER CONFIGURABLE PARAMETERS ========== */

// Uncomment the following line if you are using 2.13" tricolor 212*104 EPD
#define EPD_TRICOLOR
// Uncomment the following line if you are using 2.13" monochrome 250*122 EPD
// #define EPD_MONOCHROME

// Default amount of time to sleep between updates, in seconds
#define DEFAULT_SLEEP_SEC 60

// The duration the button must be held to reset the counter
#define RESET_BUTTON_HOLD_SEC 10

// WiFi timeout in seconds
#define WIFI_TIMEOUT_SEC 60

// Text to display under ellapsed time
#define SUBTITLE_TEXT "...without a Leroy accident."  // substitute as desired

// Font for the words in the ellapsed time text
const GFXfont *timeFont = &FreeSans9pt7b;
// Font for the numbers in the ellapsed time text
const GFXfont *timeFontNum = &FreeSansBold9pt7b;
// Font for the subtitle text
const GFXfont *subFont = &FreeSerif9pt7b;
// Font for the info bar text
const GFXfont *infoFont = &Org_01;

/* ========== BATTERY VOLTAGE DATA ========== */

// Battery percentage at each voltage (in mV) is index * 5
const unsigned int BATT_MV_LUT[] = { 3270, 3610, 3690, 3710, 3730,
                                     3750, 3770, 3790, 3800, 3820,
                                     3840, 3850, 3870, 3910, 3950,
                                     3980, 4020, 4080, 4110, 4150, 4200 };


/* ========== DISPLAY SETUP ========== */

// ESP32 settings
#define SD_CS         14
#define SRAM_CS       32
#define EPD_CS        15
#define EPD_DC        33
#define LEDPIN        13
#define LEDPINON      HIGH
#define LEDPINOFF     LOW
#define VOLTDIVPIN    A13
#define BUTTONPIN     14
#define ESP_BUTTONPIN  (gpio_num_t)BUTTONPIN

#define EPD_RESET   -1 // can set to -1 and share with microcontroller Reset!
#define EPD_BUSY    -1 // can set to -1 to not use a pin (will wait a fixed delay)

#if defined(EPD_TRICOLOR)
    #define EPD_WIDTH 212
    #define EPD_HEIGHT 104
    Adafruit_IL0373 epd(212, 104, EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);
#elif defined(EPD_MONOCRHOME)
    #define EPD_WIDTH 250
    #define EPD_HEIGHT 122
    Adafruit_SSD1675 epd(250, 122, EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);
#else
    #error "Enable a #define for your EPD type (EPD_TRICOLOR or EPD_MONOCHROME)"
#endif


/* ========== EEPROM CONFIGURATION ========== */

#define EEPROM_VALID_CODE 0xBADD0660 // 3135047264
struct LastAccident {
    unsigned int validityCode;
    time_t time;
};
#define EEPROM_SIZE sizeof(LastAccident)
#define LAST_ACCIDENT_ADDR 0
#define MEMBER_SIZE(type, member) sizeof(((type *)0)->member)


/* ========== GFX FUNCTIONS ========== */

// get string length in pixels
// set text font prior to calling this
int getStringLength(const char * str)
{
    int16_t x, y;
    uint16_t w, h;
    epd.getTextBounds(str, 0, 0, &x, &y, &w, &h);
    return w;
}

// word wrap routine
// first time send string to wrap
// 2nd and additional times: use empty string
// returns substring of wrapped text.
// set text font prior to calling
// TODO: rewrite using strtok
char * wrapWord(const char * wrapString, int linesize)
{
    static char buff[255];
    int linestart = 0;
    static int lineend = 0;
    static int bufflen = 0;
    if( strlen(wrapString) == 0 )
    {
        // additional line from original string
        linestart = lineend + 1;
        lineend = bufflen;
        Serial.println("[WRAP] Existing string to wrap, starting at position " + String(linestart) + ": " + String(&buff[linestart]));
    }
    else
    {
        Serial.println("[WRAP] New string to wrap: " + String(wrapString));
        memset(buff,0,sizeof(buff));
        // new string to wrap
        linestart = 0;
        strcpy(buff,wrapString);
        lineend = strlen(buff);
        bufflen = strlen(buff);
    }
    uint16_t w;
    int lastwordpos = linestart;
    int wordpos = linestart + 1;
    while(true)
    {
        while(buff[wordpos] == ' ' && wordpos < bufflen)
            wordpos++;
        while(buff[wordpos] != ',' && wordpos < bufflen)
            wordpos++;
        if(buff[wordpos] == ',' && wordpos < bufflen)
            wordpos++;
        if(wordpos < bufflen)
            buff[wordpos] = '\0';
        w = getStringLength(&buff[linestart]);
        if(wordpos < bufflen)
            buff[wordpos] = ' ';
        if(w > linesize)
        {
            buff[lastwordpos] = '\0';
            lineend = lastwordpos;
            return &buff[linestart];
        }
        else if(wordpos >= bufflen)
        {
            // first word too long or end of string, send it anyway
            buff[wordpos] = '\0';
            lineend = wordpos;
            return &buff[linestart];
        }
        lastwordpos = wordpos;
        wordpos++;
    }
}

// return # of lines created from word wrap
// set text font prior to calling
int getLineCount(const char * str, int pixelWidth)
{
    int lineCount = 0;
    String line = wrapWord(str, pixelWidth);
    while(line.length() > 0)
    {
        lineCount++;
        line = wrapWord("", pixelWidth);
    }
    return lineCount;
}

// return the pixel height of a given font
int getLineHeight(const GFXfont * font)
{
    int height;
    if(font == NULL)
    {
        height = 12;  // default font height is 12
    }
    else
    {
        height = (uint8_t)pgm_read_byte(&font->yAdvance);
    }
    return height;
}


/* ========== TEXT FUNCTIONS ========== */

// Formats a timestamp as human readable string for logging
String getTimeString(time_t time)
{
    tmElements_t tm = {0};
    breakTime(time, tm);
    char datetime[20] = {0};
    snprintf(datetime, sizeof(datetime), "%d/%d/%d %d:%d:%d", tm.Month, tm.Day, tm.Year+1970, tm.Hour, tm.Minute, tm.Second);
    return String(datetime);
}

// Returns true if string contains only a number
bool strIsNumber(const char * str)
{
    char * endptr;
    int num = strtol(str, &endptr, 10);
    return ((num != 0) && (*endptr == '\0'));
}


/* ========== UTILITY FUNCTIONS ========== */

// Interrupt handler for button press
void buttonPressHandler()
{
    // Reset board, so long press detection can trigger
    ESP.restart();
}

// Returns true if button is pressed longer than configured duration
bool detectLongPress()
{
    bool isButtonLongPressed = false;
    unsigned long startTime = millis();
    Serial.println("[BUTTON] Button was held during boot");
    while(LOW == digitalRead(BUTTONPIN))
    {
        if ( (millis() - startTime) > (RESET_BUTTON_HOLD_SEC*1000))
        {
            isButtonLongPressed = true;
            break;
        }
    }

    if (isButtonLongPressed)
    {
        Serial.println("[BUTTON] Long press triggered");
    }
    else
    {
        Serial.println("[BUTTON] No long press");
    }

    return isButtonLongPressed;
}

// Measures battery voltage in mV
unsigned int getBatteryVoltage()
{
    unsigned int voldDivAdc = analogRead(VOLTDIVPIN);
    unsigned int voltage = map(voldDivAdc, 0 , 4095, 0, 3300);
    voltage *= 2.0 * 1.1;
    Serial.println("[BATT] Battery: " + String(voltage) + " mV");
    return voltage;
}

// Logs the wakeup method
void logWakeupReason()
{
    String TAG = "[BOOT]";
    esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();

    switch(wakeupReason)
    {
        case ESP_SLEEP_WAKEUP_EXT0: Serial.println(TAG + " " + "Wakeup caused by external signal using RTC_IO"); break;
        case ESP_SLEEP_WAKEUP_EXT1: Serial.println(TAG + " " + "Wakeup caused by external signal using RTC_CNTL"); break;
        case ESP_SLEEP_WAKEUP_TIMER: Serial.println(TAG + " " + "Wakeup caused by timer"); break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println(TAG + " " + "Wakeup caused by touchpad"); break;
        case ESP_SLEEP_WAKEUP_ULP: Serial.println(TAG + " " + "Wakeup caused by ULP program"); break;
        default: Serial.println(TAG + " " + "Wakeup caused by unknown reason: " + String(wakeupReason)); break;
    }
}

// Put device in sleep mode for provided duration (in seconds)
// Also conditionally configure buttonpress wakeup interrupt
void goToSleep(int sleepDuration, bool allowButtonWakeup = true)
{
    if (allowButtonWakeup) {
        pinMode(BUTTONPIN, INPUT_PULLUP);
        esp_sleep_enable_ext0_wakeup(ESP_BUTTONPIN, LOW);  // Arduino API doesn't expose this, so call esp function directly
        Serial.println("[SLEEP] Wake on button press configured");
    }

    Serial.println("[SLEEP] Going to sleep for " + String(sleepDuration) + " seconds...");
    ESP.deepSleep(sleepDuration * 1e6);
}

/* ========== CONVERSION FUNCTIONS ========== */

// Formats the difference between two timestamps as an ellapsed time string
String getTimeDeltaString(time_t t1, time_t t2)
{
    time_t delta = (t1 > t2) ? (t1 - t2) : (t2 - t1);
    Serial.println("[DELTA] Time delta is: " + String(delta));
    String deltaStr = "";

    // Choose one of Years/Months/Weeks
    if (delta >= SECS_PER_YEAR)
    {
        unsigned int years = delta / SECS_PER_YEAR;
        deltaStr += String(years) + " year";
        if (years != 1)
        {
            deltaStr += "s";
        }
        delta %= SECS_PER_YEAR;
    }
    else if (delta >= (SECS_PER_DAY*30))
    {
      if (deltaStr.length() > 0)
      {
        deltaStr += ", ";
      }
      unsigned int months = delta / (SECS_PER_DAY*30);
      deltaStr += String(months) + " month";
      if (months != 1)
      {
        deltaStr += "s";
      }
      delta %= SECS_PER_WEEK;
    }
    else if (delta >= SECS_PER_WEEK)
    {
      if (deltaStr.length() > 0)
      {
        deltaStr += ", ";
      }
      unsigned int weeks = delta / SECS_PER_WEEK;
      deltaStr += String(weeks) + " week";
      if (weeks != 1)
      {
        deltaStr += "s";
      }
      delta %= SECS_PER_WEEK;
    }

    // Add days if non-zero
    if (delta >= SECS_PER_DAY)
    {
        if (deltaStr.length() > 0)
        {
            deltaStr += ", ";
        }
        unsigned int days = delta / SECS_PER_DAY;
        deltaStr += String(days) + " day";
        if (days != 1)
        {
            deltaStr += "s";
        }
        delta %= SECS_PER_DAY;
    }

    // Add hours if non-zero
    if (delta >= SECS_PER_HOUR)
    {
        if (deltaStr.length() > 0)
        {
            deltaStr += ", ";
        }
        unsigned int hours = delta / SECS_PER_HOUR;
        deltaStr += String(hours) + " hour";
        if (hours != 1)
        {
            deltaStr += "s";
        }
        delta %= SECS_PER_HOUR;
    }

    // Add minutes if non-zero
    if (delta >= SECS_PER_MIN)
    {
        if (deltaStr.length() > 0)
        {
            deltaStr += ", ";
        }
        unsigned int mins = delta / SECS_PER_MIN;
        deltaStr += String(mins) + " minute";
        if (mins != 1)
        {
            deltaStr += "s";
        }
        delta %= SECS_PER_MIN;
    }

    // Add seconds if non-zero
    if (delta >= 1)
    {
        if (deltaStr.length() > 0)
        {
            deltaStr += ", ";
        }
        deltaStr += String(delta) + " second";
        if (delta != 1)
        {
            deltaStr += "s";
        }
    }

    Serial.println("[DELTA] Delta string: " + deltaStr);
    return deltaStr;
}

// Converts battery voltage (in mV) to percent using LUT
unsigned int getBatteryPercent(unsigned int voltage)
{
    unsigned int idx = 0;
    unsigned int lutLen = sizeof(BATT_MV_LUT) / sizeof(BATT_MV_LUT[0]);
    unsigned int lastLutVolt = BATT_MV_LUT[0];
    unsigned int currLutVolt = 0;
    unsigned int battPct = 0;

    if (voltage <= BATT_MV_LUT[0])
    {
        battPct = 0;
    }
    else if (voltage >= BATT_MV_LUT[lutLen-1])
    {
        battPct = 100;
    }
    else
    {
        for(idx = 1; idx < lutLen; idx++)
        {
            currLutVolt = BATT_MV_LUT[idx];
            if (voltage < currLutVolt)
            {
                battPct = map(voltage, lastLutVolt, currLutVolt, (idx-1)*5, idx*5);
                // battPct = idx * 5;
                break;
            }
        }
    }

    Serial.println("[BATT] " + String(voltage) + "mV = " + String(battPct) + "%");
    return battPct;
}


/* ========== NETWORK FUNCTIONS ========== */

// Retrieve page response from given URL
bool getURLResponse(String url, String &response)
{
    String TAG = "[HTTP]";
    HTTPClient http;
    int httpCode = 0;
    bool success = false;
    response = "";

    Serial.println(TAG + " " + "Connecting to URL: " + url);
    if(http.begin(url))
    {
        // Start connection and send HTTP header
        Serial.print(TAG + " " + "GET...");
        httpCode = http.GET();
        Serial.println(" Status Code: " + String(httpCode));

        // HTTPClient error
        if (httpCode < 0)
        {
            String errString = http.errorToString(httpCode);
            response = "HTTP GET " + url + " failed with status code " + String(httpCode) + ", Error: " + errString;
            Serial.println(TAG + " " + "GET failed, error: " + http.errorToString(httpCode));
        }
        else
        {
            // File found
            if (HTTP_CODE_OK == httpCode || HTTP_CODE_MOVED_PERMANENTLY == httpCode)
            {
                success = true;
                response = http.getString();
                Serial.println(TAG + " " + "Response:\n\t" + response);
            }
            else // Other error type
            {
                response = "HTTP GET " + url + " failed with status code " + String(httpCode);
                Serial.println(TAG + " " + "No Reponse, Check Status Code");
            }
        }
        http.end();
    }
    else {
        response = "Unable to connect to: " + url;
        Serial.println(TAG + " " + "Unable to connect");
    }

    return success;
}

// Access a network server to get current time
time_t getTimeFromNetwork(String &err)
{
    String TAG = "[JSON]";
    String TIME_URL = "http://worldtimeapi.org/api/timezone/Etc/UTC";
    String KEY_UNIXTIME = "unixtime";

    StaticJsonDocument<1024> doc;
    String response;
    time_t timestamp = 0;
    err = "";

    if (!getURLResponse(TIME_URL, response))
    {
        err = response;
    }
    else
    {
        DeserializationError jsonErr = deserializeJson(doc, response);
        if (jsonErr)
        {
            String jsonErrMsg = String(jsonErr.c_str());
            String errMsg = "Deserialization failed with \"" + jsonErrMsg + "\" error, JSON:";
            err = errMsg + " " + response;
            Serial.println(TAG + " " + errMsg + "\n\t" + response);
        }
        else
        {
            JsonVariant jv = doc[KEY_UNIXTIME];
            if (jv.isNull())
            {
                String errMsg = "Response is missing key: \"" + KEY_UNIXTIME + "\", JSON:";
                err = errMsg + " " + response;
                Serial.println(TAG + " " + errMsg + "\n\t" + response);
            }
            else
            {
                String timeStr = jv.as<String>();
                if (!strIsNumber(timeStr.c_str()))
                {
                    String errMsg = "Invalid timestamp: " + timeStr;
                    err = errMsg;
                    Serial.println(TAG + " " + errMsg);
                }
                else
                {
                    timestamp = timeStr.toInt();
                    Serial.println(TAG + " " + "Current Timestamp = " + String(timestamp));
                }
            }
        }
    }

    return timestamp;
}


/* ========== EPD FUNCTIONS ========== */

// Prints an error message to the display
void printError(String errMsg)
{
    Serial.println("[ERROR] Printing error to display: " + errMsg);
    epd.setTextColor(EPD_RED);
    epd.setFont(NULL);
    epd.setTextSize(1);
    epd.setTextWrap(true);

    epd.setCursor(0, 2);
    epd.print(errMsg);
}

// Prints ellapsed time to the display
void printEllapsedTime(String ellapsedTime)
{
    Serial.println("[ELLAPSED] Printing elapsed time to display: " + ellapsedTime);

    int x = 0, y = 0;
    int marginX = 4, marginY = 0;

    epd.setTextColor(EPD_BLACK);
    epd.setFont(timeFontNum);  // use the bolder font for text wrap calculations
    epd.setTextSize(1);
    epd.setTextWrap(false);

    int lineHeightTime = getLineHeight(timeFontNum);
    int lineHeightSubtitle = getLineHeight(subFont);
    int lineHeightInfo = getLineHeight(infoFont);
    int textWidth = epd.width() - marginX*2;
    int textHeight = epd.height() - lineHeightSubtitle - lineHeightInfo;
    int lineCount = getLineCount(ellapsedTime.c_str(), textWidth);
    int maxlines = textHeight / lineHeightTime;
    marginY = (textHeight - lineCount*lineHeightTime)/2 + lineHeightTime;

    Serial.println("[ELLAPSED] Available text width is " + String(textWidth));
    Serial.println("[ELLAPSED] Available text height is " + String(textHeight));
    Serial.println("[ELLAPSED] Line height is " + String(lineHeightTime));
    Serial.println("[ELLAPSED] Line count is " + String(lineCount));
    Serial.println("[ELLAPSED] Max lines is " + String(maxlines));

    int lineNum = 0;
    String line = wrapWord(ellapsedTime.c_str(), textWidth);
    while(line.length() > 0)
    {
        lineNum++;
        Serial.println("[ELLAPSED] Printing line " + String(lineNum) + "/" + String(lineCount) +": '" + line + "'");
        epd.setCursor(x + marginX, y + marginY);

        // Split on spaces and switch to bold font for numbers
        char * token = strtok(strdup(line.c_str()), " ");
        while (NULL != token)
        {
            String tokStr = token;
            tokStr = " " + tokStr;
            if(strIsNumber(token))
            {
                epd.setTextColor(EPD_RED);
                epd.setFont(timeFontNum);
                epd.setTextSize(1);
            }
            else
            {
                epd.setTextColor(EPD_BLACK);
                epd.setFont(timeFont);
                epd.setTextSize(1);
            }
            epd.print(tokStr);

            // Setup for next token
            x += getStringLength(tokStr.c_str());
            epd.setCursor(x + marginX, y + marginY);
            token = strtok(NULL, " ");
        }

        // End of line, reset for next line
        x = 0;
        y += lineHeightTime;
        line = wrapWord("", textWidth);
    }
}

// Prints subtitle to the display
void printSubtitle(String subtitle)
{
    Serial.println("[SUBTITLE] Printing subtitle to display: " + subtitle);
    int marginX = 6;
    int marginY = 10;

    epd.setTextColor(EPD_BLACK);
    epd.setFont(subFont);
    epd.setTextSize(1);
    epd.setTextWrap(false);

    int lineHeightSubtitle = getLineHeight(subFont);
    int lineHeightInfo = getLineHeight(infoFont);
    int lineWidth = getStringLength(subtitle.c_str());

    Serial.println("[SUBTITLE] Available text width is " + String(epd.width() - marginX*2));
    Serial.println("[SUBTITLE] Actual text width is " + String(lineWidth));
    Serial.println("[SUBTITLE] Line height is " + String(lineHeightSubtitle));

    // Draw subtitle text
    int cursorX = epd.width() - lineWidth - marginX;
    int cursorY = epd.height() - lineHeightInfo - marginY;
    epd.setCursor(cursorX, cursorY);
    epd.print(subtitle);
}

// Prints info bar to the display
void printInfoBar(unsigned int batteryVoltage, int wifiRSSI)
{
    int marginX = 4;
    int marginY = 2;
    int y = epd.height() - marginY;
    int lineHeightInfo = getLineHeight(infoFont);

    epd.setFont(infoFont);
    epd.setTextColor(EPD_INVERSE);
    epd.setTextSize(1);
    epd.setTextWrap(false);

    unsigned int batteryPercent = getBatteryPercent(batteryVoltage);
    String left = String(batteryPercent) + "% (" + batteryVoltage + " mV)";
    String rssiStr = (INT_MAX == wifiRSSI) ? "???" : String(wifiRSSI);
    String right = "[" + rssiStr + " dBm]";
    int leftWidth = getStringLength(left.c_str());
    int rightWidth = getStringLength(right.c_str());
    Serial.println("[INFOBAR] Printing info bar to display: " + left + " " + right);


    Serial.println("[INFOBAR] Available text width is " + String(epd.width() - marginX*2));
    Serial.println("[INFOBAR] Actual text width is " + String(leftWidth + rightWidth));
    Serial.println("[INFOBAR] Line height is " + String(lineHeightInfo));

    // Draw status bar
    epd.fillRect(0, epd.height() - lineHeightInfo - marginY, epd.width(), lineHeightInfo + marginY, EPD_BLACK);

    // Draw left aligned text
    epd.setCursor(marginX, y);
    epd.print(left);

    // Draw right aligned text
    epd.setCursor(epd.width() - rightWidth - marginX, y);
    epd.print(right);
}


/* ========== ARDUINO FUNCTIONS ========== */

void setup() {
    String ellapsedTime, subtitle, error;
    bool isButtonLongPressed = false;
    int rssi = INT_MAX;
    LastAccident lastAccident = {0};
    unsigned int sleepDuration = DEFAULT_SLEEP_SEC;

    pinMode(LEDPIN, OUTPUT);
    digitalWrite(LEDPIN, LEDPINON);

    pinMode(BUTTONPIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTONPIN), buttonPressHandler, FALLING);

    Serial.begin(115200);
    while (!Serial) {
        ; // wait for serial port to connect. Needed for native USB
    }
    Serial.println("================================================================================");

    logWakeupReason();

    epd.begin();
    Serial.println("[EPD] ePaper display initialized");
    epd.clearBuffer();
    epd.setTextWrap(false);

    // If long button press detected, then reset the last accident timestamp
    if(detectLongPress())
    {
        Serial.println("[BUTTON] Long press detected, resetting last accident timestamp");
        EEPROM.begin(EEPROM_SIZE);
        // Randomize which byte of validityCode is corrupted to wear-level EEPROM erases
        int writeAddess = LAST_ACCIDENT_ADDR  + (rand() % MEMBER_SIZE(LastAccident,validityCode));
        EEPROM.write(writeAddess, 0);
        EEPROM.commit();
        EEPROM.end();
    }

    Serial.print("[WIFI] Connecting to WiFi on " + String(WIFI_SSID));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int counter = 0;
    while (WiFi.status() != WL_CONNECTED && counter < WIFI_TIMEOUT_SEC) {
        delay(1000);
        Serial.print(".");
        counter++;
    }
    Serial.println();

    wl_status_t wifiStatus = WiFi.status();
    if(WL_CONNECTED != wifiStatus)
    {
        error = "WiFi connection timed out, status=" + String(wifiStatus);
        Serial.println(error);
        printError(error);
    }
    else
    {
        Serial.println("[WIFI] Connected to " + String(WIFI_SSID));
        rssi = WiFi.RSSI();

        time_t unixtime = getTimeFromNetwork(error);
        if (0 == unixtime)
        {
            printError(error);
        }
        else
        {
            setTime(unixtime);

            EEPROM.begin(EEPROM_SIZE);
            EEPROM.get(LAST_ACCIDENT_ADDR, lastAccident);
            if (EEPROM_VALID_CODE != lastAccident.validityCode)
            {
                Serial.println("[EEPROM] Last accident was reset, code: " + String(lastAccident.validityCode) + " " + String(sizeof(lastAccident.validityCode)));
                error = "Last accident was reset. Recording current time.";
                printError(error);

                lastAccident.time = now();
                lastAccident.validityCode = EEPROM_VALID_CODE;
                EEPROM.put(LAST_ACCIDENT_ADDR, lastAccident);
                EEPROM.commit();
                Serial.println("[EEPROM] Last accident set to: " + String(lastAccident.time));
                sleepDuration = 10;
            }
            else
            {
                Serial.println("[EEPROM] Last accident timestamp = " + String(lastAccident.time) + " (" + getTimeString(lastAccident.time) + ")");
                if (now() < lastAccident.time) {
                    Serial.println("[DELTA] Negative delta detected");
                    error = "Current time is older than last recorded accident time.";
                    printError(error);
                }
                else
                {
                    String time_str = getTimeDeltaString(now(), lastAccident.time);
                    printEllapsedTime(time_str);
                    printSubtitle(SUBTITLE_TEXT);
                }
            }
            EEPROM.end();
        }
    }
    printInfoBar(getBatteryVoltage(), rssi);

    Serial.println("[EPD] Updating display...");
    epd.display();
    epd.powerDown();

    goToSleep(sleepDuration);
}

void loop() {
    // should never get here, setup() puts the CPU to sleep
}
