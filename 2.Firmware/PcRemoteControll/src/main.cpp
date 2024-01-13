#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <Ticker.h>
#include <OneButton.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266mDNS.h>

#define PIN_LED 2     // LED指示灯
#define PIN_TEST 5    // 测试按钮
#define PIN_STATUS 13 // 电脑开机状态检测
#define PIN_RESTAR 14 // 重启按钮控制
#define PIN_PWR 15    // 电源按钮控制

String firmwareVersion = "v20240106-1";
String filesystemVersion = "";
unsigned long sysBootTime;

// 连接wifi配置
String ssid = "";
String password = "";
long timeRssi;

// wifi连接
WiFiClient client;
ESP8266WebServer webServer(80);
ESP8266HTTPUpdateServer updateServer;

long timeBtn;
Ticker ticker;
OneButton btnTest = OneButton(PIN_TEST, true, true); // pin, activeLow, pullupActive

// MQTT
PubSubClient mqttClient(client);
char buffer_mq[512];
char msgBuffer[512];
unsigned long lastConnectMQTTTime = 0;
int lastPowerStatus = 0;

// 连接腾讯mqtt配置
const char *productId = ""; // 腾讯物联网平台产品ID
const char *productSecret = "";
const char *mqtt_server = "";
const char *clientId = "";
const char *mqttUserName = "";
const char *mqttPassword = "";

// put function declarations here:
void btnTick();
void onTestBtnClick();
void connectWifi();
void connectMqtt();
void mqttCallback(char *topic, byte *payload, unsigned int length);
void pubMQTTmsg();

void loopPrintDebugMsg();
void loopTestButton();
void loopMQTT();
void loopReport();

String readFile(const char *path);
void writeFile(const char *path, const char *content);

void initWebServer();
void webRoot();
void webNotFound();
void webVueJs();
void webAxiosJs();
void webStatus();
void webDeviceInfo();
void webVersion();
void webMockClick();
void webReboot();

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.printf("setup()");
  pinMode(PIN_LED, OUTPUT);    // LED灯
  pinMode(PIN_TEST, INPUT);    // 测试按钮
  pinMode(PIN_STATUS, INPUT);  // 电脑开机状态检测
  pinMode(PIN_RESTAR, OUTPUT); // RESET 电脑重启键
  pinMode(PIN_PWR, OUTPUT);    // PWR   电脑开机键
  // 绑定点击事件处理
  btnTest.attachClick(onTestBtnClick);
  ticker.attach_ms(20, btnTick);

  // 从文件加载配置
  bool littleFSSuccess = LittleFS.begin();
  Serial.println(littleFSSuccess ? "LittleFS init success" : "An Error has occurred while mounting LittleFS");
  filesystemVersion = readFile("filesystem-version.txt");
  Serial.printf("firmware=%s,filesystem=%s\n", firmwareVersion.c_str(), filesystemVersion.c_str());

  connectWifi();
  // 开启webserver
  initWebServer();
  // 开启mDns
  bool mDns = MDNS.begin("computer");
  // 连接mqtt服务器
  connectMqtt();
  pubMQTTmsg();
}

void loop()
{
  // put your main code here, to run repeatedly:
  loopPrintDebugMsg();
  webServer.handleClient();
  MDNS.update();
  loopTestButton();
  loopMQTT();
  loopReport();
}

void loopPrintDebugMsg()
{
  if (millis() - timeRssi > 2000)
  {
    int rssi = WiFi.RSSI();
    Serial.printf("Wifi信号接收强度:%ddBm\n", rssi);
    timeRssi = millis();
    int value = digitalRead(PIN_STATUS);
    Serial.printf("开机状态:%d\n", value);
  }
}

// 测试按钮按下，模拟电脑电源按钮和重启按钮按下
void loopTestButton()
{
  if (millis() - timeBtn > 100)
  {
    int value = digitalRead(PIN_TEST);
    timeBtn = millis();
    if (value == 0)
    {
      // 按下导通
      digitalWrite(PIN_RESTAR, HIGH);
      digitalWrite(PIN_PWR, HIGH);
      digitalWrite(PIN_LED, LOW);
    }
    else
    {
      // 未按下，不导通
      digitalWrite(PIN_RESTAR, LOW);
      digitalWrite(PIN_PWR, LOW);
      digitalWrite(PIN_LED, HIGH);
    }
  }
}

void loopMQTT()
{
  boolean connected = mqttClient.loop();
  if (!connected)
  {
    unsigned long current_time = millis();
    if (current_time - lastConnectMQTTTime > 5 * 1000) //  最小时间间隔为5秒
    {
      Serial.println("mqttClient.loop() return false");
      connectMqtt();
      pubMQTTmsg();
    }
  }
}

void loopReport()
{
  int value = digitalRead(PIN_STATUS);
  if (lastPowerStatus != value)
  {
    pubMQTTmsg();
  }
}

void btnTick()
{
  btnTest.tick();
}

void onTestBtnClick()
{
  Serial.printf("test button click\n");
}

void connectWifi()
{
  Serial.printf("WiFi connecting:%s\n", ssid.c_str());
  WiFi.begin(ssid, password);

  pinMode(PIN_LED, OUTPUT);
  while (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(PIN_LED, HIGH);
    delay(100);
    digitalWrite(PIN_LED, LOW);
    delay(100);
    Serial.printf(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.printf("IP  address:%s\r\n", WiFi.localIP().toString().c_str());
  Serial.printf("mac address:%s\r\n", WiFi.macAddress().c_str());
  digitalWrite(PIN_LED, HIGH);
}

void connectMqtt()
{
  lastConnectMQTTTime = millis();
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);
  boolean result = mqttClient.connect(clientId, mqttUserName, mqttPassword);
  if (result)
  {
    Serial.println("mqtt connect success");
    // 订阅属性下发
    String down_property = String("$thing/down/property/替换为自己的productId/");
    down_property.concat(clientId);
    Serial.printf("down_property=%s\n", down_property.c_str());
    boolean result_sub1 = mqttClient.subscribe(down_property.c_str());
    if (result_sub1)
    {
      Serial.println("mqtt subscribe success");
    }
    else
    {
      Serial.println("mqtt subscribe failure");
    }

    // 行为
    String down_action = String("$thing/down/action/替换为自己的productId/");
    down_action.concat(clientId);
    Serial.printf("down_action=%s\n", down_action.c_str());
    boolean result_sub = mqttClient.subscribe(down_action.c_str());
    if (result_sub)
    {
      Serial.println("mqtt subscribe success");
    }
    else
    {
      Serial.println("mqtt subscribe failure");
    }
  }
  else
  {
    Serial.println("mqtt connect failure");
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  if (length < 512)
  {
    for (int i = 0; i < length; i++)
    {
      char c = (char)payload[i];
      buffer_mq[i] = c;
    }
    buffer_mq[length] = 0;
  }

  StaticJsonDocument<512> doc;
  deserializeJson(doc, buffer_mq);
  JsonObject obj = doc.as<JsonObject>();
  String method = obj["method"];
  Serial.printf("method=%s\n", method.c_str());
  if (method.equals("action"))
  {
    String actionId = obj["actionId"];
    int value = obj["params"]["value"];
    if (actionId.equals("power"))
    {
      Serial.printf("value=%d\n", value);
      digitalWrite(PIN_PWR, HIGH);
      digitalWrite(PIN_LED, LOW);
      delay(1000);
      digitalWrite(PIN_PWR, LOW);
      digitalWrite(PIN_LED, HIGH);
    }
    else if (actionId.equals("reset"))
    {
      Serial.printf("value=%d\n", value);
      digitalWrite(PIN_RESTAR, HIGH);
      digitalWrite(PIN_LED, LOW);
      delay(1000);
      digitalWrite(PIN_RESTAR, LOW);
      digitalWrite(PIN_LED, HIGH);
    }
  }
}

// 发布信息
void pubMQTTmsg()
{
  String topicString = "$thing/up/property/替换为自己的productId/dev1";
  char publishTopic[topicString.length() + 1];
  strcpy(publishTopic, topicString.c_str());

  // 建立发布信息。信息内容以Hello World为起始，后面添加发布次数。
  int rssi = WiFi.RSSI();
  int value = digitalRead(PIN_STATUS);
  String ipaddr = client.localIP().toString();
  String powerStatus = value == 1 ? "已开机" : "已关机";
  String messageString = "{\"method\":\"report\",\"clientToken\":\"\",\"params\":{\"rssi\":\"-50dBm\", \"ipaddr\":\"192.168.0.100\", \"power_switch\":\"\"}}";
  sprintf(msgBuffer, "{\"method\":\"report\",\"clientToken\":\"\",\"params\":{\"rssi\":\"%ddBm\", \"ipaddr\":\"%s\", \"power_switch\":\"%s\"}}", rssi, ipaddr.c_str(), powerStatus.c_str());

  // 实现ESP8266向主题发布信息
  if (mqttClient.publish(publishTopic, msgBuffer))
  {
    Serial.println("Publish Topic:");
    Serial.println(publishTopic);
    Serial.println("Publish message:");
    Serial.println(msgBuffer);
    lastPowerStatus = value;
  }
  else
  {
    Serial.println("Message Publish Failed.");
  }
}

String readFile(const char *path)
{
  Serial.printf("Reading file: %s\n", path);
  File file = LittleFS.open(path, "r");
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return "";
  }
  Serial.print("Read from file: ");
  String content = file.readString();
  Serial.println(content);
  file.close();
  return content;
}

void writeFile(const char *path, const char *content)
{
  Serial.printf("Writing file: %s\n", path);
  File file = LittleFS.open(path, "w");
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }
  if (file.print(content))
  {
    Serial.println("File write success");
  }
  else
  {
    Serial.println("Write failed");
  }
  file.close();
}

void initWebServer()
{
  webServer.on("/", webRoot);
  webServer.on("/vue.min.js", webVueJs);
  webServer.on("/axios.min.js", webAxiosJs);
  webServer.on("/status", webStatus);
  webServer.on("/deviceInfo", webDeviceInfo);
  webServer.on("/version", webVersion);
  webServer.on("/click", webMockClick);
  webServer.on("/reboot", webReboot);
  // webServer.on("/update", HTTP_GET, webRoot); // 覆盖固件升级默认页面
  webServer.onNotFound(webNotFound);
  updateServer.setup(&webServer);
  webServer.begin();
  Serial.println("webServer started");
}

void webRoot()
{
  File file = LittleFS.open("/index.html", "r");
  webServer.streamFile(file, "text/html; charset=utf-8");
  file.close();
}

void webNotFound()
{
  webServer.send(404, "text/html; charset=utf-8", "404");
}

void webVueJs()
{
  File file = LittleFS.open("/vue.min.js.gz", "r");
  webServer.streamFile(file, "application/javascript");
  file.close();
}

void webAxiosJs()
{
  File file = LittleFS.open("/axios.min.js.gz", "r");
  webServer.streamFile(file, "application/javascript");
  file.close();
}

/**
 * 获取设备状态接口
 */
void webStatus()
{
  int rssi = WiFi.RSSI();
  int freeHeap = ESP.getFreeHeap() / 1024;
  long runTime = (millis() - sysBootTime) / 1000; // 系统启动后累计运行时长，单位秒
  sprintf(msgBuffer, "{\"ip\": \"%s\", \"rssi\": \"%d dBm\", \"clientId\": \"%s\", \"freeHeap\": \"%d KB\", \"runTime\": %d}", WiFi.localIP().toString().c_str(), rssi, clientId, freeHeap, runTime);
  webServer.send(200, "application/json; charset=utf-8", msgBuffer);
}

/**
 * 获取设备信息接口
 */
void webDeviceInfo()
{
  uint32_t chipId = ESP.getChipId();
  uint8_t bootVersion = ESP.getBootVersion();
  String coreVersion = ESP.getCoreVersion();
  ESP.getSdkVersion();
  uint8_t cpuFreq = ESP.getCpuFreqMHz();
  uint32_t flashChipId = ESP.getFlashChipId();
  uint32_t flashChipSize = ESP.getFlashChipRealSize() / 1024;
  uint32_t sketchSize = ESP.getSketchSize() / 1024;
  uint32_t freeSketchSize = ESP.getFreeSketchSpace() / 1024;
  String sketchMD5 = ESP.getSketchMD5();
  sprintf(msgBuffer, "{\"chipId\": \"0x%02X\", \"cpuFreq\": \"%d MHz\", \"flashChipId\": \"0x%02X\", \"flashChipSize\": \"%d KB\", \"sketchSize\": \"%d KB\", \"freeSketchSize\": \"%d KB\", \"sketchMD5\": \"%s\", \"macAddr\": \"%s\"}", chipId, cpuFreq, flashChipId, flashChipSize, sketchSize, freeSketchSize, sketchMD5.c_str(), WiFi.macAddress().c_str());
  webServer.send(200, "application/json; charset=utf-8", msgBuffer);
}

/**
 * 固件版本信息
 */
void webVersion()
{
  sprintf(msgBuffer, "{\"firmware\": \"%s\", \"filesystem\": \"%s\"}", firmwareVersion.c_str(), filesystemVersion.c_str());
  webServer.send(200, "application/json; charset=utf-8", msgBuffer);
}

/**
 * 舵机执行点击
 */
void webMockClick()
{
  webServer.send(200, "text/html; charset=utf-8", "OK");
  digitalWrite(PIN_PWR, HIGH);
  digitalWrite(PIN_LED, LOW);
  delay(1000);
  digitalWrite(PIN_PWR, LOW);
  digitalWrite(PIN_LED, HIGH);
}

/**
 * 设备重启
 */
void webReboot()
{
  ESP.restart();
  webServer.send(200, "text/html; charset=utf-8", "OK");
}