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
constexpr unsigned long WEATHER_START_DELAY_MS=120000UL; // allow stability check before first TLS request
constexpr unsigned long WEATHER_POLL_MS=900000UL;
unsigned long bootAt=0;
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
  if(ESP.getFreeHeap()<22000){Serial.println("Weather: skipped (low heap)");return;}
  Serial.printf("Weather: requesting, free heap %u bytes\\n", ESP.getFreeHeap());
  BearSSL::WiFiClientSecure client;
  client.setInsecure(); // Public read-only weather endpoint; no credentials.
  client.setBufferSizes(512,512);
  client.setTimeout(4000);
  HTTPClient http;
  String url=String("https://api.open-meteo.com/v1/forecast?latitude=")+String(latitude,4)+
    "&longitude="+String(longitude,4)+"&current=temperature_2m,weather_code";
  if(!http.begin(client,url))return;
  http.setTimeout(4000);
  int response=http.GET();
  Serial.printf("Weather: HTTP %d, free heap %u bytes\\n",response,ESP.getFreeHeap());
  if(response==HTTP_CODE_OK){
    StaticJsonDocument<2048>d;
    if(deserializeJson(d,http.getStream())==DeserializationError::Ok){
      weatherTemp=d["current"]["temperature_2m"]|NAN;
      weatherCode=d["current"]["weather_code"]|-1;
    }
  }http.end();
  yield();
}
void centered(const String &s,int y,int size,uint16_t color){
  tft.setTextSize(size);tft.setTextColor(color);int w=s.length()*6*size;
  tft.setCursor(max(0,(240-w)/2),y);tft.print(s);
}
void eyes(){
  int look=((frames/10)%7)-3;
  bool blink=(frames%101==0 || frames%101==1);
  tft.fillTriangle(33,83,102,97,102,138,CYAN);
  tft.fillTriangle(33,83,33,119,102,138,CYAN);
  tft.fillTriangle(138,97,207,83,138,138,CYAN);
  tft.fillTriangle(207,83,207,119,138,138,CYAN);
  if(blink){tft.fillRect(28,105,184,11,BG);}
  else{
    tft.fillRoundRect(63+look,108,16,24,3,BG);
    tft.fillRoundRect(167+look,108,16,24,3,BG);
  }
  tft.drawLine(106,177,135,177,PINK);
  if(frames%37==0)for(int n=0;n<6;n++)tft.drawFastHLine(random(12,190),random(20,215),random(8,36),PINK);
  centered("ROBO // ONLINE",210,1,CYAN);
}
void draw(){
  Mode next=(Mode)((millis()/9000UL)%4);
  bool changed=(next!=current);
  current=next;
  static bool first=true;
  if(changed || first){
    tft.fillScreen(BG);
    yield();
    tft.drawRoundRect(5,5,230,230,15,CYAN);
    first=false;
  }else if(current==EYES){
    // Only clear the animated facial region, never the entire display per frame.
    tft.fillRect(24,75,192,125,BG);
  }else{
    return; // Text screens are static until the next scene switch.
  }
  if(current==EYES)eyes();
  else if(current==TIME){
    time_t now=time(nullptr);struct tm *tm=localtime(&now);
    char b[16];if(now>100000)strftime(b,sizeof(b),"%H:%M",tm);else snprintf(b,sizeof(b),"--:--");
    centered("SYSTEM CLOCK",45,1,PINK);centered(b,93,4,WHITE);
  }else if(current==WEATHER){
    centered("WEATHER // "+city,40,1,PINK);
    centered(isfinite(weatherTemp)?String(weatherTemp,1)+" C":String("--.- C"),94,3,CYAN);
    centered("CODE "+String(weatherCode),158,1,WHITE);
  }else{
    centered("NETWORK STATUS",43,1,PINK);
    centered(WiFi.status()==WL_CONNECTED?"CONNECTED":"SETUP AP",78,2,CYAN);
    centered(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():WiFi.softAPIP().toString(),122,1,WHITE);
    centered("RSSI "+String(WiFi.RSSI())+" dBm",159,1,CYAN);
  }frames++;
}
void setup(){
  Serial.begin(115200);delay(200);
  bootAt=millis();
  Serial.printf("\\nROBOGLITCH startup | reset: %s | heap: %u bytes\\n",ESP.getResetReason().c_str(),ESP.getFreeHeap());
  randomSeed(ESP.getCycleCount());
  pinMode(TFT_BL,OUTPUT);analogWriteRange(255);analogWriteFreq(1000);setBrightness(); // GeekMagic Ultra: active-low backlight
  SPI.begin();
  tft.init(240,240,SPI_MODE3);tft.setRotation(0);tft.invertDisplay(true);tft.fillScreen(BG);
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
  if(millis()-lastFrame>=500){lastFrame=millis();draw();yield();}
  if(WiFi.status()==WL_CONNECTED && millis()-bootAt>=WEATHER_START_DELAY_MS &&
     (lastWeather==0 || millis()-lastWeather>=WEATHER_POLL_MS)){
    lastWeather=millis();
    getWeather();
  }
  yield();
}
