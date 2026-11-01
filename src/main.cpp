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
String timezoneRule="CET-1CEST,M3.5.0/2,M10.5.0/3";
uint8_t faceTime=65, motionLevel=2;
bool nightMode=true;
unsigned long nextMoodAt=0;
unsigned long faceFrameAt=0;
uint8_t forcedMood=255;
unsigned long forcedMoodUntil=0;
void configureClock(){
  if(WiFi.status()==WL_CONNECTED)configTime(0,0,"pool.ntp.org","time.google.com");
  setenv("TZ",timezoneRule.c_str(),1);tzset();
}
void setBrightness(){analogWrite(TFT_BL,255-brightness);}

void saveConfig(){
  StaticJsonDocument<768> d;
  d["ssid"]=ssid;d["pass"]=pass;d["user"]=otaUser;d["ota"]=otaPass;
  d["city"]=city;d["lat"]=latitude;d["lon"]=longitude;d["brightness"]=brightness;d["tz"]=timezoneRule;d["faceTime"]=faceTime;d["motion"]=motionLevel;d["night"]=nightMode;
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
    timezoneRule=d["tz"]|String("CET-1CEST,M3.5.0/2,M10.5.0/3");
    faceTime=(uint8_t)constrain((int)(d["faceTime"]|65),30,90);
    motionLevel=(uint8_t)constrain((int)(d["motion"]|2),0,3);
    nightMode=d["night"]|true;
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
  h+=F("Zeitzone (POSIX): <input name=tz value='")+escapeHtml(timezoneRule)+F("'>");
  h+=F("<small>Deutschland: CET-1CEST,M3.5.0/2,M10.5.0/3 | UTC: UTC0 | UK: GMT0BST,M3.5.0/1,M10.5.0/2</small>");
  h+=F("Gesicht-Anteil in Prozent <input type=number name=faceTime min=30 max=90 value=")+String(faceTime)+F(">");
  h+=F("Animationsintensitaet 0-3 <input type=number name=motion min=0 max=3 value=")+String(motionLevel)+F(">");
  h+=F("<label><input type=checkbox name=night value=1 ");
  if(nightMode)h+=F("checked");
  h+=F("> Nachtmodus aktiv (22-7 Uhr)</label>");
  h+=F("<p><a href='/react?mood=2'>Freude</a> | <a href='/react?mood=5'>Ueberraschung</a> | <a href='/react?mood=7'>Glitch</a></p>");
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
  if(server.hasArg("tz") && server.arg("tz").length()<100)timezoneRule=server.arg("tz");
  if(server.hasArg("faceTime"))faceTime=(uint8_t)constrain(server.arg("faceTime").toInt(),30,90);
  if(server.hasArg("motion"))motionLevel=(uint8_t)constrain(server.arg("motion").toInt(),0,3);
  nightMode=server.hasArg("night");
  saveConfig();server.send(200,"text/plain","Gespeichert. Einmaliger Neustart wegen geaenderter WLAN-Einstellungen ...");
  server.client().flush();delay(500);
  Serial.println("Configuration saved: intentional reboot");ESP.restart();
}
void statusHandler(){
  StaticJsonDocument<768>d;d["name"]="RoboGlitch";d["ip"]=WiFi.localIP().toString();d["rssi"]=WiFi.RSSI();
  d["uptime"]=millis()/1000;d["freeHeap"]=ESP.getFreeHeap();d["city"]=city;
  if(isfinite(weatherTemp))d["temperature"]=weatherTemp;
  d["weatherStatus"]=weatherState;d["weatherCode"]=weatherCode;
  d["timezone"]=timezoneRule;d["mood"]=mood;d["version"]="0.3.1";
  time_t utc=time(nullptr);if(utc>100000){char buf[32];struct tm* local=localtime(&utc);strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",local);d["localTime"]=buf;}
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
    configureClock();
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

/*
 * Flicker-free eye renderer:
 * One reusable 8-bit tile (84 x 64 = 5.3 KB) is assembled in RAM, then
 * transferred as RGB565 pixels in a single SPI transaction per eye.
 * No LCD clear command occurs between frames.
 */
constexpr int EYE_W=84,EYE_H=64;
uint8_t previousCaptionMood=255;
void blitEyeTile(GFXcanvas8 &tile,int16_t screenX,int16_t screenY){
  static uint16_t scanline[EYE_W];
  const uint8_t* pixels=tile.getBuffer();
  if(!pixels)return;
  static const uint16_t palette[4]={BG,CYAN,PINK,WHITE};
  tft.startWrite();
  tft.setAddrWindow(screenX,screenY,EYE_W,EYE_H);
  for(int row=0;row<EYE_H;row++){
    const uint8_t* source=pixels+row*EYE_W;
    for(int col=0;col<EYE_W;col++)scanline[col]=palette[source[col]&3];
    tft.writePixels(scanline,EYE_W);
  }
  tft.endWrite();
}
void drawEyes(bool fresh){
  static GFXcanvas8 tile(EYE_W,EYE_H);
  if(!tile.getBuffer()){
    static bool warned=false;
    if(!warned){Serial.println("EYE TILE: out of memory");warned=true;}
    return;
  }
  unsigned long now=millis();
  float rate=motionLevel==3?195.0f:440.0f;
  int look=motionLevel==0?0:(int)(6.0f*sin(now/rate));
  if(mood==1)look=10;
  if(mood==6)look=-10;
  bool blink=((now%4700UL)<155UL)||mood==4;
  int bob=motionLevel==0?0:(int)(2.0f*sin(now/375.0f));

  for(int side=0;side<2;side++){
    tile.fillScreen(0); // clears RAM tile, never the visible display
    int x=7+bob;
    int y=5;
    if(blink){
      tile.fillRoundRect(x+3,30,66,5,2,1);
    }else if(mood==2){
      tile.drawRoundRect(x+4,13,62,36,16,1);
      tile.drawFastHLine(x+15,51,40,2);
    }else if(mood==5){
      tile.fillRoundRect(x+12,7,46,48,19,1);
      tile.fillRoundRect(x+29+look/3,25,11,15,5,0);
    }else{
      int h=42;
      int slope=mood==3?19:(mood==6?5:11);
      if(side==0){
        tile.fillTriangle(x,9,x+69,9+slope,x+69,9+h,1);
        tile.fillTriangle(x,9,x,9+h-9,x+69,9+h,1);
      }else{
        tile.fillTriangle(x,9+slope,x+69,9,x,9+h,1);
        tile.fillTriangle(x+69,9,x+69,9+h-9,x,9+h,1);
      }
      tile.fillRoundRect(x+27+look,28,12,20,3,0);
    }
    if(mood==7){
      for(int i=0;i<3;i++){
        tile.drawFastHLine(random(3,58),random(12,49),random(10,21),2);
      }
    }
    blitEyeTile(tile,side==0?32:124,87);
    yield();
  }
  if(fresh || previousCaptionMood!=mood){
    tft.fillRect(86,149,68,20,BG);
    if(mood==2)tft.drawRoundRect(102,152,36,11,5,PINK);
    else if(mood==5)tft.drawCircle(120,158,6,PINK);
    else if(mood==4)tft.drawFastHLine(107,158,26,PINK);
    else {
      tft.drawLine(101,154,120,162,PINK);
      tft.drawLine(120,162,139,154,PINK);
    }
    tft.fillRect(40,179,160,15,BG);
    centered(String("MOOD // ")+moodLabel[mood],181,1,CYAN);
    previousCaptionMood=mood;
  }
}
void draw(){
  unsigned long now=millis();
  const unsigned long cycle=60000UL;
  unsigned long phase=now%cycle;
  unsigned long faceMs=cycle*faceTime/100UL;
  Mode next=phase<faceMs?EYES:(phase<faceMs+(cycle-faceMs)/3?TIME:(phase<faceMs+2*(cycle-faceMs)/3?WEATHER:NET));
  static bool first=true;
  static unsigned long lastDrawnMinute=0;
  static float lastDisplayedTemp=NAN;
  bool switchScene=first||next!=current;
  current=next;
  if(first){
    tft.fillScreen(BG);
    tft.drawRoundRect(7,7,226,226,17,PINK);
    tft.drawRoundRect(13,13,214,214,14,CYAN);
    centered("ROBO//GLITCH",23,1,PINK);
    first=false;
  }
  if(switchScene){
    // Only change the screen's content window, preserve the border/header.
    tft.fillRect(20,53,200,151,BG);
    if(current==EYES)previousCaptionMood=255;
  }

  if(current==EYES){
    if(forcedMood!=255 && (int32_t)(forcedMoodUntil-now)>0)mood=forcedMood;
    else if(now>=nextMoodAt){
      forcedMood=255;
      mood=(uint8_t)random(0,8);
      if(weatherCode>=61 && weatherCode<=82 && random(0,3)==0)mood=6;
      if(WiFi.status()!=WL_CONNECTED)mood=6;
      nextMoodAt=now+(unsigned long)random(2800,6500);
    }
    const unsigned long interval=motionLevel==0?1100:(motionLevel==1?210:(motionLevel==2?110:75));
    if(switchScene || now-faceFrameAt>=interval){
      drawEyes(switchScene);
      faceFrameAt=now;
    }
  }else if(current==TIME){
    time_t utc=time(nullptr);
    if(switchScene || (utc>100000 && (unsigned long)(utc/60)!=lastDrawnMinute)){
      lastDrawnMinute=utc>100000?(unsigned long)(utc/60):0;
      if(!switchScene)tft.fillRect(26,91,188,70,BG);
      struct tm* local=localtime(&utc);
      char b[16];
      if(utc>100000)strftime(b,sizeof(b),"%H:%M",local);
      else snprintf(b,sizeof(b),"--:--");
      if(switchScene)centered("SYSTEM CLOCK",53,1,PINK);
      centered(b,98,4,CYAN);
      if(switchScene)centered("LOCAL TIME",168,1,PINK);
    }
  }else if(current==WEATHER){
    bool temperatureChanged=isfinite(weatherTemp)&&
      (!isfinite(lastDisplayedTemp)||weatherTemp!=lastDisplayedTemp);
    if(switchScene || temperatureChanged){
      lastDisplayedTemp=weatherTemp;
      if(!switchScene)tft.fillRect(24,98,192,52,BG);
      if(switchScene){
        centered("WEATHER MODULE",50,1,PINK);
        centered(city.substring(0,18),72,1,WHITE);
      }
      centered(isfinite(weatherTemp)?String(weatherTemp,1)+" C":String("--.- C"),104,3,CYAN);
      if(switchScene)centered(weatherState,161,1,PINK);
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
  loadConfig();setBrightness();configureClock();connectNetwork();
  server.on("/",HTTP_GET,home);
  server.on("/save",HTTP_POST,saveHandler);
  server.on("/status",HTTP_GET,statusHandler);
  server.on("/react",HTTP_GET,[](){
    if(WiFi.status()==WL_CONNECTED && !authorized())return;
    forcedMood=(uint8_t)constrain(server.arg("mood").toInt(),0,7);
    forcedMoodUntil=millis()+15000UL;
    server.send(200,"text/plain","RoboGlitch reaction activated");
  });
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
  if(millis()-lastFrame>=100){draw();lastFrame=millis();yield();}
  if(nightMode && time(nullptr)>100000){
    time_t localEpoch=time(nullptr);
    struct tm* now=localtime(&localEpoch);
    static uint8_t oldBrightness=255;
    uint8_t target=(now->tm_hour>=22||now->tm_hour<7)?min((int)brightness,22):brightness;
    if(target!=oldBrightness){analogWrite(TFT_BL,255-target);oldBrightness=target;}
  }
  if(WiFi.status()==WL_CONNECTED && millis()-bootAt>=WEATHER_START_DELAY_MS &&
     (lastWeather==0 || millis()-lastWeather>=weatherRetryMs)){
    lastWeather=millis();
    getWeather();
  }
  yield();
}
