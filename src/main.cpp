#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

constexpr int TFT_CS=15,TFT_DC=0,TFT_RST=2,TFT_BL=5;
Adafruit_ST7789 tft(TFT_CS,TFT_DC,TFT_RST);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer firmwareUpdater;
String ssid,pass,otaUser="admin",otaPass,city="Berlin";
float latitude=52.52,longitude=13.405,weatherTemp=NAN;
int weatherCode=-1;
unsigned long lastWeather=0,lastFrame=0;
constexpr unsigned long WEATHER_START_DELAY_MS=15000UL; // allow stability check before first TLS request
constexpr unsigned long WEATHER_POLL_MS=900000UL;
unsigned long bootAt=0;
unsigned long weatherRetryMs=60000UL;
String weatherState="WAITING";
unsigned long lastMoodAt=0;
uint8_t mood=0;
uint32_t frames=0;
enum Mode{EYES,TIME,WEATHER,NET};
Mode current=EYES;
const uint16_t BG=0x0000,CYAN=0xFE60,PINK=0xD104,WHITE=0xFFFF;
uint8_t brightness=75;
void setBrightness(){analogWrite(TFT_BL,255-brightness);}

void saveConfig(){
  StaticJsonDocument<768> d;
  d["ssid"]=ssid;d["pass"]=pass;d["user"]=otaUser;d["ota"]=otaPass;
  d["city"]=city;d["lat"]=latitude;d["lon"]=longitude;d["brightness"]=brightness;
  File f=LittleFS.open("/settings.json","w");
  if(f){serializeJson(d,f);f.close();}
}
void loadConfig(){
  if(!LittleFS.exists("/settings.json"))return;
  File f=LittleFS.open("/settings.json","r");if(!f)return;
  StaticJsonDocument<768>d;
  if(deserializeJson(d,f)==DeserializationError::Ok){
    ssid=d["ssid"].as<String>();pass=d["pass"].as<String>();
    otaUser=d["user"]|String("admin");otaPass=d["ota"].as<String>();
    city=d["city"]|String("Berlin");latitude=d["lat"]|52.52f;longitude=d["lon"]|13.405f;brightness=(uint8_t)constrain((int)(d["brightness"]|75),0,180);
  }f.close();
}
String escapeHtml(String v){v.replace("&","&amp;");v.replace("<","&lt;");v.replace(">","&gt;");v.replace("\"","&quot;");return v;}
bool authorized(){
  if(otaPass.length()==0)return true; 
  if(server.authenticate(otaUser.c_str(),otaPass.c_str()))return true;
  server.requestAuthentication();return false;
}
void home(){
  String h=F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><style>body{background:#0b1220;color:#dff;font:16px system-ui;max-width:580px;margin:32px auto;padding:20px}input,button{display:block;padding:12px;margin:8px 0;width:100%;box-sizing:border-box;border-radius:8px}button{background:#61eee1}a{color:#61eee1}</style><h1>ROBO//GLITCH</h1><p>WiFi: ");
  h+=WiFi.status()==WL_CONNECTED?escapeHtml(WiFi.localIP().toString()):String("Setup-Modus");
  h+=F("</p><p>OTA: <a href='/update'>Firmwareupdate</a> · <a href='/status'>Status JSON</a></p><form method='post' action='/save'>");
  h+=F("WLAN SSID<input name='ssid' value='")+escapeHtml(ssid)+F("'>WLAN Passwort<input type='password' name='pass' placeholder='Unverändert lassen'>");
  h+=F("Stadt<input name='city' value='")+escapeHtml(city)+F("'>Breitengrad<input name='lat' value='")+String(latitude,5)+F("'>Längengrad<input name='lon' value='")+String(longitude,5)+F("'>");
  h+=F("OTA Benutzer<input name='user' value='")+escapeHtml(otaUser)+F("'>OTA Passwort<input type='password' name='ota' placeholder='Unverändert lassen'>");
  h+=F("Helligkeit <input type=number name=brightness min=0 max=180 value=")+String(brightness)+F(">");
  h+=F("<button>Speichern und neu starten</button></form>");
  server.send(200,"text/html; charset=utf-8",h);
}
void saveHandler(){
  if(WiFi.status()==WL_CONNECTED && !authorized())return;
  ssid=server.arg("ssid");if(server.arg("pass").length())pass=server.arg("pass");
  city=server.arg("city");latitude=server.arg("lat").toFloat();longitude=server.arg("lon").toFloat();
  otaUser=server.arg("user");if(!otaUser.length())otaUser="admin";
  if(server.arg("ota").length())otaPass=server.arg("ota");
  if(server.hasArg("brightness"))brightness=(uint8_t)constrain(server.arg("brightness").toInt(),0,180);
  saveConfig();server.send(200,"text/plain","Gespeichert. Einmaliger Neustart wegen geaenderter WLAN-Einstellungen ...");
  server.client().flush();delay(500);
  Serial.println("Configuration saved: intentional reboot");ESP.restart();
}
void statusHandler(){
  StaticJsonDocument<384>d;d["name"]="RoboGlitch";d["ip"]=WiFi.localIP().toString();d["rssi"]=WiFi.RSSI();
  d["uptime"]=millis()/1000;d["freeHeap"]=ESP.getFreeHeap();d["city"]=city;
  if(isfinite(weatherTemp))d["temperature"]=weatherTemp;
  d["weatherStatus"]=weatherState;d["weatherCode"]=weatherCode;
  String out;serializeJson(d,out);server.send(200,"application/json",out);
}
void connectNetwork(){
  WiFi.mode(WIFI_STA);WiFi.hostname("roboglitch");
  if(ssid.length()){
    WiFi.begin(ssid.c_str(),pass.c_str());unsigned long start=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-start<13000){delay(100);yield();}
  }
  if(WiFi.status()!=WL_CONNECTED){
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("RoboGlitch-Setup");
    Serial.print("Setup AP: ");Serial.println(WiFi.softAPIP());
  }else{
    Serial.print("IP: ");Serial.println(WiFi.localIP());
    configTime(3600,3600,"pool.ntp.org","time.google.com");
    MDNS.begin("roboglitch");
  }
}
void getWeather(){
  if(WiFi.status()!=WL_CONNECTED)return;
  if(ESP.getFreeHeap()<22000){weatherState="LOW MEMORY";weatherRetryMs=60000UL;return;}
  BearSSL::WiFiClientSecure client;
  client.setInsecure(); // Public weather endpoint, no credentials.
  client.setBufferSizes(512,512);
  client.setTimeout(6000);
  HTTPClient http;
  String url=String("https://api.open-meteo.com/v1/forecast?latitude=")+String(latitude,5)+
    "&longitude="+String(longitude,5)+"&current=temperature_2m,weather_code";
  if(!http.begin(client,url)){weatherState="CONNECT ERROR";weatherRetryMs=60000UL;return;}
  http.setTimeout(6000);
  int code=http.GET();
  Serial.printf("Weather HTTP %d, free heap %u\n",code,ESP.getFreeHeap());
  if(code==HTTP_CODE_OK){
    // HTTPClient::getString() handles chunked transfer encoding. Parsing
    // getStream() directly can incorrectly return Ok with no current object.
    String payload=http.getString();
    Serial.printf("Weather body bytes %u, preview: %.100s\n",
                  (unsigned)payload.length(),payload.c_str());
    StaticJsonDocument<768> data;
    DeserializationError err=deserializeJson(data,payload);
    JsonVariant temp=data["current"]["temperature_2m"];
    if(!err && temp.is<float>()){
      weatherTemp=temp.as<float>();
      weatherCode=data["current"]["weather_code"]|-1;
      weatherState="OK";
      weatherRetryMs=WEATHER_POLL_MS;
      Serial.printf("Weather temperature %.1f C\n",weatherTemp);
    }else{
      weatherState=err ? String("JSON ")+err.c_str() : "NO TEMPERATURE";
      weatherRetryMs=60000UL;
      Serial.printf("Weather parse/status failed: %s\n",weatherState.c_str());
    }
  }else{
    weatherState="HTTP "+String(code);
    weatherRetryMs=60000UL;
  }
  http.end();
  yield();
}
void centered(const String &s,int y,int size,uint16_t color){
  tft.setTextSize(size);tft.setTextColor(color);int w=s.length()*6*size;
  tft.setCursor(max(0,(240-w)/2),y);tft.print(s);
}
// Animated states: idle, curious, happy, angry, sleepy, surprised, confused, glitch.
const char* moodLabel[]={"IDLE","CURIOUS","HAPPY","ANGRY","SLEEPY","SURPRISED","CONFUSED","GLITCH"};
void drawEyes(){
  // Keep the border and header untouched. Only repaint the face panel.
  tft.fillRect(27,77,186,125,BG);
  tft.drawRoundRect(27,79,186,94,12,PINK);
  int look=(mood==1?12:(mood==6?-12:((int)(frames/5)%5-2)*2));
  bool blink=frames%37==0 || mood==4;
  int h=mood==5?52:38;
  for(int side=0;side<2;side++){
    int x=side==0?39:139;
    if(blink){
      tft.fillRoundRect(x,113,62,5,2,CYAN);
    }else if(mood==2){
      tft.drawRoundRect(x+3,93,56,37,15,CYAN);
      tft.drawFastHLine(x+12,132,37,PINK);
    }else if(mood==5){
      tft.fillRoundRect(x+9,91,42,h,17,CYAN);
      tft.fillRoundRect(x+25+look/3,108,10,18,5,BG);
    }else{
      // Angry angular eyes, with color and geometry variation.
      int slant=mood==3?19:(mood==6?4:10);
      if(side==0){
        tft.fillTriangle(x,96,x+62,96+slant,x+62,96+h,CYAN);
        tft.fillTriangle(x,96,x,96+h-8,x+62,96+h,CYAN);
      }else{
        tft.fillTriangle(x,96+slant,x+62,96,x,96+h,CYAN);
        tft.fillTriangle(x+62,96,x+62,96+h-8,x,96+h,CYAN);
      }
      tft.fillRoundRect(x+23+look,110,13,20,2,BG);
    }
  }
  if(mood==2)tft.drawRoundRect(101,151,38,10,6,PINK);
  else if(mood==5)tft.drawCircle(120,155,7,PINK);
  else if(mood==4)tft.drawFastHLine(105,155,30,PINK);
  else {
    tft.drawLine(102,155,120,158,PINK);
    tft.drawLine(120,158,138,155,PINK);
  }
  if(mood==7){
    for(int i=0;i<4;i++)tft.drawFastHLine(random(32,190),random(87,149),random(9,26),PINK);
  }
  centered(String("MOOD // ")+moodLabel[mood],181,1,CYAN);
}
void draw(){
  unsigned long now=millis();
  // Face remains the primary screen; short rotating data views.
  uint8_t segment=(now/12000UL)%4;
  Mode next=(Mode)segment;
  static bool first=true;
  static unsigned long lastDrawnMinute=0;
  static float lastDisplayedTemp=NAN;
  bool switchScene=first || next!=current;
  current=next;
  if(switchScene){
    tft.fillScreen(BG);
    tft.drawRoundRect(7,7,226,226,17,PINK);
    tft.drawRoundRect(13,13,214,214,14,CYAN);
    centered("ROBO//GLITCH",23,1,PINK);
    first=false;
  }
  if(current==EYES){
    if(now-lastMoodAt>2900UL){
      mood=(mood+1)%8;
      lastMoodAt=now;
    }
    if(switchScene || now-lastFrame>950UL)drawEyes();
  }else if(current==TIME){
    unsigned long minute=now/60000UL;
    if(switchScene || minute!=lastDrawnMinute){
      lastDrawnMinute=minute;
      tft.fillRect(22,48,196,148,BG);
      time_t utc=time(nullptr);struct tm *local=localtime(&utc);
      char b[16];
      if(utc>100000)strftime(b,sizeof(b),"%H:%M",local);
      else snprintf(b,sizeof(b),"--:--");
      centered("SYSTEM CLOCK",53,1,PINK);
      centered(b,98,4,CYAN);
      centered("LOCAL TIME",168,1,PINK);
    }
  }else if(current==WEATHER){
    if(switchScene || (isfinite(weatherTemp) && (!isfinite(lastDisplayedTemp) || weatherTemp!=lastDisplayedTemp))){
      lastDisplayedTemp=weatherTemp;
      tft.fillRect(22,48,196,148,BG);
      centered("WEATHER MODULE",50,1,PINK);
      centered(city.substring(0,18),72,1,WHITE);
      centered(isfinite(weatherTemp)?String(weatherTemp,1)+" C":String("--.- C"),104,3,CYAN);
      centered(weatherState,161,1,PINK);
    }
  }else if(switchScene){
    centered("NETWORK",50,1,PINK);
    centered(WiFi.status()==WL_CONNECTED?"CONNECTED":"SETUP AP",81,2,CYAN);
    centered(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():WiFi.softAPIP().toString(),132,1,WHITE);
    centered("RSSI "+String(WiFi.RSSI())+" dBm",169,1,PINK);
  }
  frames++;
}
void setup(){
  Serial.begin(115200);delay(200);
  bootAt=millis();
  Serial.printf("\\nROBOGLITCH startup | reset: %s | heap: %u bytes\\n",ESP.getResetReason().c_str(),ESP.getFreeHeap());
  randomSeed(ESP.getCycleCount());
  pinMode(TFT_BL,OUTPUT);analogWriteRange(255);analogWriteFreq(1000);setBrightness(); // GeekMagic Ultra: active-low backlight
  SPI.begin();
  tft.init(240,240,SPI_MODE3);tft.setRotation(2);tft.invertDisplay(true);tft.fillScreen(BG);
  Serial.println("TFT ST7789 initialized, SPI mode 3, inverted colors, backlight ON");
  centered("ROBOGLITCH",100,2,CYAN);
  if(!LittleFS.begin()){Serial.println("LittleFS failed");}
  loadConfig();setBrightness();connectNetwork();
  server.on("/",HTTP_GET,home);
  server.on("/save",HTTP_POST,saveHandler);
  server.on("/status",HTTP_GET,statusHandler);
  if(otaPass.length()){
    firmwareUpdater.setup(&server,"/update",otaUser.c_str(),otaPass.c_str());
  }else{
    Serial.println("WARNING: OTA disabled until an OTA password is set via the setup page");
  }
  server.begin();
  Serial.println("HTTP server ready.");
}
void loop(){
  server.handleClient();if(WiFi.status()==WL_CONNECTED)MDNS.update();
  if(millis()-lastFrame>=450){draw();lastFrame=millis();yield();}
  if(WiFi.status()==WL_CONNECTED && millis()-bootAt>=WEATHER_START_DELAY_MS &&
     (lastWeather==0 || millis()-lastWeather>=weatherRetryMs)){
    lastWeather=millis();
    getWeather();
  }
  yield();
}
