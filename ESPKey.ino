/* ESPKey
*  Kenny McElroy
*
* This sketch runs on the ESPKey ESP8266 module carrier with the 
* intent of being physically attached to the Wiegand data wires
* between a card reader and the local control box.
* 
* Huge thanks to Brad Antoniewicz for sharing his Wiegand tools
* for Arduino.  This code was a great starting point and
* excellent reference: https://github.com/brad-anton/VertX
*
*/

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <FS.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

#ifdef ESP8266
extern "C" {
  #include "user_interface.h"
}
#endif

#define VERSION "131"

#define HOSTNAME "ESPKey-" // Hostname prefix for DHCP/OTA
#define CONFIG_FILE "/config.json"
#define AUTH_FILE "/auth.txt"
#define DBG_OUTPUT_PORT Serial	// This could be a file with some hacking
#define CARD_LEN 4     // minimum card length in bits
#define PULSE_WIDTH 34 // length of asserted pulse in microSeconds
#define PULSE_GAP 2000 - PULSE_WIDTH   // delay between pulses in microSeconds

// Pin number assignments
#define D0_ASSERT 12
#define D0_SENSE 13
#define D1_ASSERT 16
#define D1_SENSE 14
#define LED_ASSERT 4
#define LED_SENSE 5
#define CONF_RESET 0

// Default settings used when no configuration file exists
char log_name[20] = "Alpha";
bool ap_enable = true;
bool ap_hidden = false;
char ap_ssid[20] = "ESPKey-config"; // Default SSID.
IPAddress ap_ip(192, 168, 4, 1);
char ap_psk[20] = "accessgranted"; // Default PSK.
char station_ssid[20] = "";
char station_psk[20] = "";
char mDNShost[20] = "ESPKey";
String DoS_id = "7fffffff:31";
char ota_password[24] = "ExtraSpecialPassKey";
char www_username[20] = "";
char www_password[20] = "";
IPAddress syslog_server(0, 0, 0, 0);
unsigned int syslog_port = 514;
char syslog_service_name[20] = "accesscontrol";
char syslog_host[20] = "ESPKey";
byte syslog_priority = 36;

WiFiUDP Udp;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
File fsUploadFile;

//byte incoming_byte = 0; 
unsigned long config_reset_millis = 30000;
byte reset_pin_state = 1;

volatile byte reader1_byte = 0;
String reader1_string = "";
volatile int reader1_count = 0;
volatile unsigned long reader1_millis = 0;
volatile unsigned long last_aux_change = 0;
volatile byte last_aux = 1;
volatile byte expect_aux = 2;

void reader1_append(int value) {
  reader1_count++;
  reader1_millis = millis();
  reader1_byte = reader1_byte << 1;
  reader1_byte |= 1 & value;
  if (reader1_count % 4 == 0) {
    reader1_string += String(reader1_byte, HEX);
    reader1_byte = 0;
  }
}

void reader1_D0_trigger(void) {
  reader1_append(0);
}

void reader1_D1_trigger(void) {
  reader1_append(1);
}

byte hex_to_byte(byte in) {
  if (in>='0' && in<='9') return in-'0';
  else if (in>='A' && in<='F') return in-'A'+10;
  else if (in>='a' && in<='f') return in-'a'+10;
  else return in;
}

char c2h (unsigned char c) {
  //DBG_OUTPUT_PORT.println("c2h: " + String(c));
  return "0123456789abcdef"[0xF & c];
}

void fix_reader1_string() {
  byte loose_bits = reader1_count % 4;
  if (loose_bits > 0) {
    byte moving_bits = 4 - loose_bits;
    byte moving_mask = pow(2, moving_bits) - 1;
    //DBG_OUTPUT_PORT.println("lb: " + String(loose_bits) + " mb: " + String(moving_bits) + " mm: " + String(moving_mask, HEX));
    char c = hex_to_byte(reader1_string.charAt(0));
    for (unsigned long i=0; i<reader1_string.length(); i++) {
      reader1_string.setCharAt(i, c2h(c >> moving_bits));
      c &= moving_mask;
      c = (c << 4) | hex_to_byte(reader1_string.charAt(i+1));
      //DBG_OUTPUT_PORT.println("c: " + String(c, HEX) + " i: " + String(reader1_string.charAt(i)));
    }
    reader1_string += String((c >> moving_bits) | reader1_byte, HEX);
  }
  reader1_string += ":" + String(reader1_count);
}

void reader1_reset() {
  reader1_byte = 0;
  reader1_count = 0;
  reader1_string = "";
}

void drainD0(){
  digitalWrite(D0_ASSERT, HIGH);
}

byte toggle_pin(byte pin) {
  byte new_value = !digitalRead(pin);
  if (pin == LED_ASSERT) expect_aux = !new_value;
  digitalWrite(pin, new_value);
  return new_value;
}

void transmit_assert(byte pin) {
  digitalWrite(pin, HIGH);
  delayMicroseconds(PULSE_WIDTH);
  digitalWrite(pin, LOW);
  delayMicroseconds(PULSE_GAP);
}

void transmit_id_nope(volatile unsigned long sendValue, unsigned short bitcount) {
  DBG_OUTPUT_PORT.println("[-] Sending Data: " + String(sendValue, HEX) + ":" + String(bitcount));
  DBG_OUTPUT_PORT.print("\t");
  for (short x=bitcount-1; x>=0; x--) {
    byte v = bitRead(sendValue,x);
    DBG_OUTPUT_PORT.print(String(v)); 
    if (v == 0) {
      transmit_assert(D0_ASSERT);
    } else {
      transmit_assert(D1_ASSERT);
    }
  }
  DBG_OUTPUT_PORT.println();
}

void transmit_id(String sendValue, unsigned long bitcount) {
  DBG_OUTPUT_PORT.println("Sending data: " + sendValue + ":" + String(bitcount));
  unsigned long bits_available = sendValue.length() * 4;
  unsigned long excess_bits = 0;
  if (bits_available > bitcount) {
    excess_bits = bits_available - bitcount;
    sendValue = sendValue.substring(excess_bits/4);
    excess_bits %= 4;
    //DBG_OUTPUT_PORT.print("sending: " + sendValue + " with excess bits: " + excess_bits + "\n\t");
  } else if (bits_available < bitcount) {
    for (unsigned long i = bitcount - bits_available; i>0; i--) {
      transmit_assert(D0_ASSERT);
    }
  }
  for (unsigned long i=0; i<sendValue.length(); i++) {
    char c = hex_to_byte(sendValue.charAt(i));
    //DBG_OUTPUT_PORT.println("i:" + String(i) + " c:0x" + String(c,HEX));
    for (short x=3-excess_bits; x>=0; x--) {
      byte b = bitRead(c,x);
      //DBG_OUTPUT_PORT.println("x:" + String(x) + " b:" + b);
      if (b == 1) {
        transmit_assert(D1_ASSERT);
      } else {
        transmit_assert(D0_ASSERT);
      }
    }
    excess_bits = 0;
  }
  //DBG_OUTPUT_PORT.println();
}

void handleTxId(){
  if(!server.hasArg("v")) {server.send(500, F("text/plain"), F("BAD ARGS")); return;}
  String value = server.arg("v");
  //DBG_OUTPUT_PORT.println("handleTxId: " + value);
  String sendValue = value.substring(0, value.indexOf(":"));
  sendValue.toUpperCase();
  unsigned long bitcount = value.substring(value.indexOf(":") + 1).toInt();
  transmit_id(sendValue, bitcount);
  
  server.send(200, F("text/plain"), "");
}

bool loadConfig() {
  File configFile = SPIFFS.open(CONFIG_FILE, "r");
  if (!configFile) {
    DBG_OUTPUT_PORT.println(F("Failed to open config file"));
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    DBG_OUTPUT_PORT.println(F("Config file size is too large"));
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(buf.get(), size);

  StaticJsonBuffer<1024> jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    DBG_OUTPUT_PORT.println(F("Failed to parse config file"));
    return false;
  }

  // FIXME these should be testing for valid input before replacing defaults
  if (json.containsKey("log_name")) {
    strncpy(log_name, json["log_name"], 20);
    DBG_OUTPUT_PORT.println("Loaded log_name: " + String(log_name));
  }
  if (json.containsKey("ap_enable")) {
    ap_enable = (strcmp(json["ap_enable"], "true") == 0) ? true : false;
    DBG_OUTPUT_PORT.println("Loaded ap_enable: " + String(ap_enable));
  }
  if (json.containsKey("ap_hidden")) {
    ap_hidden = (strcmp(json["ap_hidden"], "true") == 0) ? true : false;
    DBG_OUTPUT_PORT.println("Loaded ap_hidden: " + String(ap_hidden));
  }
  if (json.containsKey("ap_ssid")) {
    strncpy(ap_ssid, json["ap_ssid"], 20);
    DBG_OUTPUT_PORT.println("Loaded ap_ssid: " + String(ap_ssid));
  }
  if (json.containsKey("ap_psk")) {
    strncpy(ap_psk, json["ap_psk"], 20);
    DBG_OUTPUT_PORT.println("Loaded ap_psk: " + String(ap_psk));
  }
  if (json.containsKey("station_ssid")) {
    strncpy(station_ssid, json["station_ssid"], 20);
    DBG_OUTPUT_PORT.println("Loaded station_ssid: " + String(station_ssid));
  }
  if (json.containsKey("station_psk")) {
    strncpy(station_psk, json["station_psk"], 20);
    DBG_OUTPUT_PORT.println("Loaded station_psk: " + String(station_psk));
  }
  if (json.containsKey("mDNShost")) {
    strncpy(mDNShost, json["mDNShost"], 20);
    DBG_OUTPUT_PORT.println("Loaded mDNShost: " + String(mDNShost));
  }
  if (json.containsKey("DoS_id")) {
    DoS_id = String((const char*)json["DoS_id"]);
    DBG_OUTPUT_PORT.println("Loaded DoS_id: " + DoS_id);
  }
  if (json.containsKey("ota_password")) {
    strncpy(ota_password, json["ota_password"], 24);
    DBG_OUTPUT_PORT.println("Loaded ota_password: " + String(ota_password));
  }
  if (json.containsKey("www_username")) {
    strncpy(www_username, json["www_username"], 20);
    DBG_OUTPUT_PORT.println("Loaded www_username: " + String(www_username));
  }
  if (json.containsKey("www_password")) {
    strncpy(www_password, json["www_password"], 20);
    DBG_OUTPUT_PORT.println("Loaded www_password: " + String(www_password));
  }
  if (json.containsKey("syslog_server")) {
    char buf[20];
    strncpy(buf, json["syslog_server"], 20);
    syslog_server.fromString(buf);
    DBG_OUTPUT_PORT.println("Loaded syslog_server: " + String(syslog_server, HEX));
  }
  if (json.containsKey("syslog_port")) {
    syslog_port = json["syslog_port"];
    DBG_OUTPUT_PORT.println("Loaded syslog_port: " + String(syslog_port));
  }
  if (json.containsKey("syslog_service_name")) {
    strncpy(syslog_service_name, json["syslog_service_name"], 20);
    DBG_OUTPUT_PORT.println("Loaded syslog_service_name: " + String(syslog_service_name));
  }
  if (json.containsKey("syslog_host")) {
    strncpy(syslog_host, json["syslog_host"], 20);
    DBG_OUTPUT_PORT.println("Loaded syslog_host: " + String(syslog_host));
  }
  if (json.containsKey("syslog_priority")) {
    syslog_priority = json["syslog_priority"];
    DBG_OUTPUT_PORT.println("Loaded syslog_priority: " + String(syslog_priority));
  }

  return true;
}

//format bytes
String formatBytes(size_t bytes){
  if (bytes < 1024){
    return String(bytes)+"B";
  } else if(bytes < (1048576)){
    return String(bytes/1024.0)+"KB";
  } else if(bytes < (1073741824)){
    return String(bytes/1048576.0)+"MB";
  } else {
    return String(bytes/1073741824.0)+"GB";
  }
}

String getContentType(String filename){
  if(server.hasArg("download")) return F("application/octet-stream");
  else if(filename.endsWith(".htm")) return F("text/html");
  else if(filename.endsWith(".html")) return F("text/html");
  else if(filename.endsWith(".css")) return F("text/css");
  else if(filename.endsWith(".js")) return F("application/javascript");
  else if(filename.endsWith(".json")) return F("text/json");
  else if(filename.endsWith(".png")) return F("image/png");
  else if(filename.endsWith(".gif")) return F("image/gif");
  else if(filename.endsWith(".jpg")) return F("image/jpeg");
  else if(filename.endsWith(".ico")) return F("image/x-icon");
  else if(filename.endsWith(".svg")) return F("image/svg+xml");
  else if(filename.endsWith(".xml")) return F("text/xml");
  else if(filename.endsWith(".pdf")) return F("application/x-pdf");
  else if(filename.endsWith(".zip")) return F("application/x-zip");
  else if(filename.endsWith(".gz")) return F("application/x-gzip");
  return "text/plain";
}

void append_log(String text) {
  File file = SPIFFS.open("/log.txt", "a");
  if(file) {
    file.println(String(millis()) + " " + text);
    DBG_OUTPUT_PORT.println("Appending to log: " + String(millis()) + " " + text);
    file.close();
  }
  else
    DBG_OUTPUT_PORT.println("Failed opening log file.");
}

void syslog(String text) {
  if (WiFi.status() != WL_CONNECTED || syslog_server == INADDR_NONE) return;
  char buf[101];
  text = "<"+String(syslog_priority)+">"+String(syslog_host)+" "+String(syslog_service_name)+": "+text;
  text.toCharArray(buf, sizeof(buf)-1);
  Udp.beginPacket(syslog_server, syslog_port);
  Udp.write(buf);
  Udp.endPacket();
}

bool basicAuthFailed() {
  if (strlen(www_username) > 0 && strlen(www_password) > 0) {
    if (!server.authenticate(www_username, www_password)) {
      server.requestAuthentication();
      return true;
    }
  }
  return false;    // This is good
}

bool handleFileRead(String path){
  DBG_OUTPUT_PORT.println("handleFileRead: " + path);
  if(path.equals(F("/"))) path = F("/static/index.htm");
  if(path.endsWith(F("/"))) path += F("index.htm");
  String contentType = getContentType(path);
  String pathWithGz = path + F(".gz");
  if(SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)){
    if(SPIFFS.exists(pathWithGz))
      path += F(".gz");
    File file = SPIFFS.open(path, "r");
    server.sendHeader("Now", String(millis()));
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

void handleFileUpload(){
  if (basicAuthFailed()) return;
  if(server.uri() != F("/edit")) return;
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START){
    String filename = upload.filename;
    if(!filename.startsWith("/")) filename = "/"+filename;
    DBG_OUTPUT_PORT.println("handleFileUpload Name: " + filename);
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    //DBG_OUTPUT_PORT.println("handleFileUpload Data: " + upload.currentSize);
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile)
      fsUploadFile.close();
    DBG_OUTPUT_PORT.println("handleFileUpload Size: " + upload.totalSize);
  }
}

void handleFileDelete(){
  if (basicAuthFailed()) return;
  if(server.args() == 0) return server.send(500, F("text/plain"), F("BAD ARGS"));
  String path = server.arg(0);
  DBG_OUTPUT_PORT.println("handleFileDelete: " + path);
  if(path == "/")
    return server.send(500, F("text/plain"), F("BAD PATH"));
  if(!SPIFFS.exists(path))
    return server.send(404, F("text/plain"), F("FileNotFound"));
  SPIFFS.remove(path);
  server.send(200, F("text/plain"), "");
  path = String();
}

void handleFileCreate(){
  if (basicAuthFailed()) return;
  if(server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  DBG_OUTPUT_PORT.println("handleFileCreate: " + path);
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if(file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileList() {
  if (basicAuthFailed()) return;
  if(!server.hasArg("dir")) {server.send(500, "text/plain", "BAD ARGS"); return;}
  
  String path = server.arg("dir");
  DBG_OUTPUT_PORT.println("handleFileList: " + path);
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while(dir.next()){
    File entry = dir.openFile("r");
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir)?"dir":"file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }
  
  output += "]";
  server.send(200, "text/json", output);
}

void handleDoS(){
  if (basicAuthFailed()) return;
  drainD0();
  server.send(200, F("text/plain"), "");
  append_log(F("DoS mode set by API request."));
}

void handleRestart() {
  if (basicAuthFailed()) return;
  append_log(F("Restart requested by user."));
  server.send(200, "text/plain", "OK");
  ESP.restart();
}

void resetConfig(void) {
  if (millis() > 30000) return;
  if (digitalRead(CONF_RESET) == 0 & reset_pin_state == 1) {
    reset_pin_state = 0;
    config_reset_millis = millis();
  } else {
    reset_pin_state = 1;
    if (millis() > (config_reset_millis + 2000)) {
      append_log(F("Config reset by pin."));
      SPIFFS.remove(CONFIG_FILE);
      ESP.restart();
    }
  }
}

void auxChange(void) {
  volatile byte new_value = digitalRead(LED_SENSE);
  if (new_value == expect_aux) {
    last_aux = new_value;
    expect_aux = 2;
    return;
  }
  if (new_value != last_aux) {
    if (millis() - last_aux_change > 10) {
      last_aux_change = millis();
      last_aux = new_value;
      append_log("Aux changed to "+String(new_value));
    }
  }
}

void setup() {
  // Outputs
  pinMode(D0_ASSERT, OUTPUT); 
  digitalWrite(D0_ASSERT, LOW); 
  pinMode(D1_ASSERT, OUTPUT); 
  digitalWrite(D1_ASSERT, LOW); 
  pinMode(LED_ASSERT, OUTPUT); 
  digitalWrite(LED_ASSERT, LOW); 

  // Inputs
  pinMode(D0_SENSE, INPUT);
  pinMode(D1_SENSE, INPUT);
  pinMode(LED_SENSE, INPUT);
  pinMode(CONF_RESET, INPUT);

  // Input interrupts
  attachInterrupt(digitalPinToInterrupt(D0_SENSE), reader1_D0_trigger, FALLING);
  attachInterrupt(digitalPinToInterrupt(D1_SENSE), reader1_D1_trigger, FALLING);
  attachInterrupt(digitalPinToInterrupt(LED_SENSE), auxChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(CONF_RESET), resetConfig, CHANGE);

  DBG_OUTPUT_PORT.begin(115200);
  DBG_OUTPUT_PORT.print("\n");
  DBG_OUTPUT_PORT.setDebugOutput(true);

  delay(100);

  DBG_OUTPUT_PORT.println("Chip ID: 0x" + String(ESP.getChipId(), HEX));

  // Set Hostname.
  String dhcp_hostname(HOSTNAME);
  dhcp_hostname += String(ESP.getChipId(), HEX);
  WiFi.hostname(dhcp_hostname);

  // Print hostname.
  DBG_OUTPUT_PORT.println("Hostname: " + dhcp_hostname);

  if (!SPIFFS.begin()) {
    DBG_OUTPUT_PORT.println(F("Failed to mount file system"));
    return;
  } else {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {
      String fileName = dir.fileName();
      // This is a dirty hack to deal with readers which don't pull LED up to 5V
      if (fileName == String("/auth.txt")) detachInterrupt(digitalPinToInterrupt(LED_SENSE));

      size_t fileSize = dir.fileSize();
      DBG_OUTPUT_PORT.printf("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
  }

  // If a log.txt exists, use ap_ssid=ESPKey-<chipid> instead of the default ESPKey-config
  // A config file will take precedence over this
  if (SPIFFS.exists("/log.txt")) dhcp_hostname.toCharArray(ap_ssid, sizeof(ap_ssid));
  append_log(F("Starting up!"));

  // Load config file.
  if (! loadConfig()) {
    DBG_OUTPUT_PORT.println(F("No configuration information available.  Using defaults."));
  }

  // Check WiFi connection
  // ... check mode
  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
    delay(10);
  }

  // ... Compare file config with sdk config.
  if (WiFi.SSID() != station_ssid || WiFi.psk() != station_psk) {
    DBG_OUTPUT_PORT.println(F("WiFi config changed.  Attempting new connection"));

    // ... Try to connect as WiFi station.
    WiFi.begin(station_ssid, station_psk);

    DBG_OUTPUT_PORT.println("new SSID: "+String(WiFi.SSID()));

    // ... Uncomment this for debugging output.
    //WiFi.printDiag(Serial);
  } else {
    // ... Begin with sdk config.
    WiFi.begin();
  }

  DBG_OUTPUT_PORT.println(F("Wait for WiFi connection."));

  // ... Give ESP 10 seconds to connect to station.
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000)
  {
    DBG_OUTPUT_PORT.write('.');
    //DBG_OUTPUT_PORT.print(WiFi.status());
    delay(500);
  }
  DBG_OUTPUT_PORT.println();

  // Check connection
  if(WiFi.status() == WL_CONNECTED)
  {
    // ... print IP Address
    DBG_OUTPUT_PORT.print("IP address: ");
    DBG_OUTPUT_PORT.println(WiFi.localIP());
  } else {
    if(ap_enable) {
      DBG_OUTPUT_PORT.println(F("Can not connect to WiFi station. Going into AP mode."));
      
      // Go into software AP mode.
      WiFi.mode(WIFI_AP);
      WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0));
      WiFi.softAP(ap_ssid, ap_psk, 0, ap_hidden);

      DBG_OUTPUT_PORT.print(F("IP address: "));
      DBG_OUTPUT_PORT.println(WiFi.softAPIP());
    } else {
      DBG_OUTPUT_PORT.println(F("Can not connect to WiFi station. Bummer."));
      //WiFi.mode(WIFI_OFF);
    }
  }

  syslog(String(log_name)+F(" starting up!"));

  // Start OTA server.
  ArduinoOTA.setHostname(dhcp_hostname.c_str());
  ArduinoOTA.setPassword(ota_password);
  ArduinoOTA.begin();
  
  if (MDNS.begin(mDNShost)) {
    DBG_OUTPUT_PORT.println("Open http://"+String(mDNShost)+".local/");
  } else {
    DBG_OUTPUT_PORT.println("Error setting up MDNS responder!");
  }
  
  
  //SERVER INIT
  server.on("/dos", HTTP_GET, handleDoS);
  server.on("/txid", HTTP_GET, handleTxId);
  server.on("/format", HTTP_DELETE, [](){
    if (basicAuthFailed()) return false;
    if(SPIFFS.format()) server.send(200, "text/plain", "Format success!");
  });
  //list directory
  server.on("/list", HTTP_GET, handleFileList);
  //load editor
  server.on("/edit", HTTP_GET, [](){
    if (basicAuthFailed()) return false;
    if(!handleFileRead("/static/edit.htm")) server.send(404, "text/plain", "FileNotFound");
  });
  //create file
  server.on("/edit", HTTP_PUT, handleFileCreate);
  //delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);
  //first callback is called after the request has ended with all parsed arguments
  //second callback handles file uploads at that location
  server.on("/edit", HTTP_POST, [](){ server.send(200, "text/plain", ""); }, handleFileUpload);
  server.on("/restart", HTTP_GET, handleRestart);

  //called when the url is not defined here
  //use it to load content from SPIFFS
  server.onNotFound([](){
    if (basicAuthFailed()) return false;
    if(!handleFileRead(server.uri()))
      server.send(404, "text/plain", "FileNotFound");
  });
  
  server.on("/version", HTTP_GET, [](){
    if (basicAuthFailed()) return false;
    String json = "{\"version\":\""+String(VERSION)+"\",\"log_name\":\""+String(log_name)+"\",\"ChipID\":\""+String(ESP.getChipId(), HEX)+"\"}\n";
    server.send(200, "text/json", json);
    json = String();
  });
  //get heap status, analog input value and all GPIO statuses in one json call
  server.on("/all", HTTP_GET, [](){
    if (basicAuthFailed()) return false;
    String json = "{";
    json += "\"heap\":"+String(ESP.getFreeHeap());
    json += ", \"analog\":"+String(analogRead(A0));
    json += ", \"gpio\":"+String((uint32_t)(((GPI | GPO) & 0xFFFF) | ((GP16I & 0x01) << 16)));
    json += "}";
    server.send(200, "text/json", json);
    json = String();
  });
  server.serveStatic("/static", SPIFFS, "/static","max-age=86400");
  httpUpdater.setup(&server);	// This doesn't do authentication
  server.begin();
  MDNS.addService("http", "tcp", 80);
  DBG_OUTPUT_PORT.println(F("HTTP server started"));
}

String grep_auth_file() {
  char buffer [64];
  char* this_id;
  int cnt = 0;

  File f = SPIFFS.open(AUTH_FILE, "r");
  if (!f) {
    DBG_OUTPUT_PORT.println(F("Failed to open auth file"));
    return "";
  }
 
  while (f.available() > 0) {
    char c = f.read();
    buffer[cnt++] = c;
    if ((c == '\n') || (cnt == sizeof(buffer)-1)) {
      buffer[cnt] = '\0';
      cnt = 0;
      this_id = strtok(buffer, " ");
      if (reader1_string == String(this_id)) {
        return String(strtok(NULL, "\n"));
      }
    }
  }
  return "";
}

void loop() {
  // Check for serial data
  /*
  if (DBG_OUTPUT_PORT.available() > 0 ) {
    incoming_byte = DBG_OUTPUT_PORT.read();
    DBG_OUTPUT_PORT.println(incoming_byte);
    delay(10);
  }
  */

  // Check for card reader data
  if(reader1_count >= CARD_LEN && (reader1_millis + 5 <= millis() || millis() < 10)) {
    fix_reader1_string();
    String name = grep_auth_file();
    if (name != "") {
      if (toggle_pin(LED_ASSERT)) {
        name += " enabled "+String(log_name);
      } else {
        name += " disabled "+String(log_name);
      }
      append_log(name);
      syslog(name);
    } else if (reader1_string == DoS_id) {
      drainD0();
      append_log("DoS mode enabled by control card");
    } else {
      append_log(reader1_string);
    }
    reader1_reset();
  }

  // Check for HTTP requests
  server.handleClient();
  ArduinoOTA.handle();

  /*
  // Blink LED for testing
  digitalWrite(LED_ASSERT, HIGH);
  delay(100);
  digitalWrite(LED_ASSERT, LOW);
  delay(4900);
  */

  // Standard delay
  delay(100);
}
