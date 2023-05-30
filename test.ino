#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ArduinoJson.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <ESP8266WiFi.h>
#include <WebSocketsClient_Generic.h>
#include "CronAlarms.h"
#include <stdlib.h>

#define i2c_Address 0x3c  // initialize with the I2C addr 0x3C Typically eBay OLED's
//#define i2c_Address 0x3d //initialize with the I2C addr 0x3D Typically Adafruit OLED's

#define SCREEN_WIDTH 128  // OLED display width, in pixels
#define SCREEN_HEIGHT 64  // OLED display height, in pixels
#define OLED_RESET -1     //   QT-PY / XIAO
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

//constant initialize
const char *DEFAULT_WS_EVENT_HANDLER = "sync";
const char *SSID = "IC2";
const char *PASSWORD = "0335293294";
//const char *BACKEND_SERVER_IP_LOCAL = "wss://lionfish-app-bbz3e.ondigitalocean.app";
const char* BACKEND_SERVER_IP_LOCAL = "lionfish-app-bbz3e.ondigitalocean.app";
//#define BACKEND_SERVER_IP_LOCAL "wss://lionfish-app-bbz3e.ondigitalocean.app"
//#define WEBSOCKET_PORT 8080
//const char *BACKEND_SERVER_IP = "171.248.45.77";
//const int WEBSOCKET_PORT = 8080;

bool alreadyConnected = false;

WebSocketsClient webSocket;

long lastRefreshTime = 0;
int beatChart = 0;
unsigned long previousMillis = 0;
const long interval = 1000;

/*======= Define for heartbeat sensor MAX30102 -E12 =======*/
MAX30105 particleSensor;
const byte RATE_SIZE = 4;  // Increase this for more averaging. 4 is good.
byte rates[RATE_SIZE];     // Array of heart rates
byte rateSpot = 0;
long lastBeat = 0;  // Time at which the last beat occurred

float beatsPerMinute;
int beatAvg;
String strStatus = "No Active";
bool statusActive = false;


//web page
const char index_html[] PROGMEM = R"webpage(
<!DOCTYPE HTML>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <script src="https://code.highcharts.com/8.0/highcharts.js"></script>
    <style>
        body {
            min-width: 300px;
            max-width: 800px;
            height: 400px;
            margin: 0 auto;
        }

        h2 {
            font-family: Arial;
            font-size: 2.5rem;
            text-align: center;
        }
    </style>
</head>
<body>
    <h2>ESP8266 Chart Realtime</h2>
    <div id="heartbeat-chart" class="container"></div>
</body>
<script>
    var chartT = new Highcharts.Chart({
        chart: {
        type: 'spline',
        renderTo: 'heartbeat-chart',
        animation: Highcharts.svg, // don't animate in old IE
        marginRight: 10,
        events: {
            load: function () {
                setInterval(function () {
        var xhttp = new XMLHttpRequest();
        xhttp.onreadystatechange = function () {
            if (this.readyState == 4 && this.status == 200) {
                var x = (new Date()).getTime(),
                    y = parseFloat(this.responseText);
                    console.log(x);
                    console.log(y);
                      chartT.series[0].addPoint([x, y], true, true);
                      console.log(chartT.series[0]);
            }
        };
        xhttp.open("GET", "/realtime_heartbeat", true);
        xhttp.send();
    }, 1000);
            }
        }
    },

    time: {
        useUTC: false
    },

    title: {
        text: 'Live data'
    },

    accessibility: {
        announceNewData: {
            enabled: true,
            minAnnounceInterval: 15000,
            announcementFormatter: function (allSeries, newSeries, newPoint) {
                if (newPoint) {
                    return 'New point added. Value: ' + newPoint.y;
                }
                return false;
            }
        }
    },

    xAxis: {
        type: 'datetime',
        tickPixelInterval: 150
    },

    yAxis: {
        title: {
            text: 'Value'
        },
        plotLines: [{
            value: 0,
            width: 1,
            color: '#808080'
        }],
        min: 0
    },

    tooltip: {
        headerFormat: '<b>{series.name}</b><br/>',
        pointFormat: '{point.x:%Y-%m-%d %H:%M:%S}<br/>{point.y:.2f}'
    },

    legend: {
        enabled: false
    },

    exporting: {
        enabled: false
    },

    series: [{
        name: 'Heart-Beat data',
        data: (function () {
            // generate an array of random data
            var data = [],
                time = (new Date()).getTime(),
                i;

            for (i = -5; i <= 0; i += 1) {
                data.push({
                    x: time + i * 1000,
                    y: Math.random()
                });
            }
            return data;
        }())
    }]
    });
</script>
</html>)webpage";

void Realtime(int heartRate) {
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    // save the last time you updated the DHT values
    previousMillis = currentMillis;
    // Read temperature as Celsius (the default)
    if (isnan(heartRate)) {
      Serial.println("Failed to read from MAX30102 sensor!");
      Serial.println(heartRate);
    } else {
      beatChart = heartRate;
      Serial.println(beatChart);
    }
  }
}

void startWiFi() {
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to ");
  Serial.print(SSID);

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\n");
  Serial.println("Connection established!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void heartBeatMeasure() {
  long irValue = particleSensor.getIR();

  if (checkForBeat(irValue) == true) {
    // We sensed a beat!
    long delta = millis() - lastBeat;
    lastBeat = millis();

    beatsPerMinute = 60 / (delta / 1000.0);

    if (beatsPerMinute < 255 && beatsPerMinute > 20) {
      rates[rateSpot++] = (byte)beatsPerMinute;  // Store this reading in the array
      rateSpot %= RATE_SIZE;                     // Wrap variable

      // Take average of readings
      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++)
        beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }
  }

  if (irValue < 50000) {
    beatAvg = -1;
  }
}


void webSocketEvent(const WStype_t &type, uint8_t *payload, const size_t &length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WSc] Disconnected!\n");
      strStatus = "No Active";
      statusActive = false;
      break;
    case WStype_CONNECTED:
      Serial.printf("[WSc] Connected to url: %s\n", payload);
      webSocket.sendTXT("Connected from Device");
      strStatus = "Active";
      statusActive = true;
      break;
    case WStype_TEXT:
      Serial.printf("[WSc] get text: %s\n", payload);
      if (payload) {
        int level = atoi((char *)payload);
        //setLedColorByLevel(level);
      }
      break;
    case WStype_BIN:
      Serial.printf("[WSc] get binary length: %u\n", length);
      hexdump(payload, length);
      break;
  }
}

void connectWebSocket() {
  webSocket.begin("lionfish-app-bbz3e.ondigitalocean.app", 80, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2);
  Serial.print("Connected to WebSockets Server @ IP address: ");
}

void displayShow(int heartRate, float spo2, int refreshDisplayTime) {
  if (millis() - lastRefreshTime >= refreshDisplayTime) {
    display.clearDisplay();
    delay(20);
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(30, 0);
    display.print("MediHearth");

    // Heartbeat Title
    display.setCursor(0, 25);
    display.print("Heartbeat:");
    // Heartbeat Value
    if (heartRate == -1) {
      display.setCursor(65, 25);
      display.print("No Finger");
    } else {
      display.setCursor(75, 25);
      display.print(heartRate);
    }

    // Status Title
    display.setCursor(10, 55);
    display.print("Status:");
    if (statusActive == true) {
      display.setCursor(70, 55);
      display.print(strStatus);
    } else {
      display.setCursor(65, 55);
      display.print("No Active");
    }
    display.display();
    lastRefreshTime += refreshDisplayTime;
  }
}

void sendHearthBeatToServer() {
  // declare JSON template to send to server
  StaticJsonDocument<200> doc;
  // with arduinoJson
  doc["event"] = DEFAULT_WS_EVENT_HANDLER;
  // data declare with array index
  // 0 is macAddres
  // 1 is temperature
  // 2 is heartbeat
  // 3 is spO2
  JsonArray data = doc.createNestedArray("data");
  data.add(WiFi.macAddress());
  //data.add(mlx.readObjectTempC() + 3.5);
  data.add(beatAvg);
  data.add(WiFi.localIP());
  char payload[100];
  serializeJson(doc, payload);
  Serial.println();
  serializeJson(doc, Serial);

  webSocket.sendTXT(payload);
}

void setup() {
  Serial.begin(9600);

  /*======= Setup for oled LCD =======*/
  delay(250);
  display.begin(i2c_Address, true);

  // Clear logo of Adafruit
  display.clearDisplay();

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST))  // Use default I2C port, 400kHz speed
  {
    Serial.println("MAX30105 was not found. Please check wiring/power. ");
    while (1)
      ;
  }
  Serial.println("Place your index finger on the sensor with steady pressure.");

  startWiFi();
  connectWebSocket();

  // Print ESP8266 Local IP Address
  Serial.println(WiFi.localIP());

  particleSensor.setup();                     //Configure sensor with default settings
  particleSensor.setPulseAmplitudeRed(0x0A);  //Turn Red LED to low to indicate sensor is running
  particleSensor.setPulseAmplitudeGreen(0);   //Turn off Green LED

  //Cronjob
  Cron.create("*/20 * * * * *", sendHearthBeatToServer, false);
}

void loop() {
  webSocket.loop();
  heartBeatMeasure();
  displayShow(beatAvg, 0, 2000);
  Cron.delay();
  Realtime(beatAvg);
}