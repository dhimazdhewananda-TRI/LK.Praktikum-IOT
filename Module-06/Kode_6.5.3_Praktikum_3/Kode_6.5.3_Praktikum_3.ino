#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// Kredensial Jaringan Wi-Fi
const char* ssid = "dhewa";
const char* password = "88888888";

// Konfigurasi Broker MQTT Publik
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* clientId = "ESP8266_LabIoT_25782037_Task67";

// Definisi Topik (Berbasis NIM 25782037)
const char* topicPublish   = "polinela/labiot/25782037/telemetri";
const char* topicSubscribe = "polinela/labiot/25782037/kendali";
const char* topicAlert     = "polinela/labiot/25782037/alert";
const char* topicStatus    = "polinela/labiot/25782037/status";

// Alokasi Pin Perangkat Keras
const byte dhtPin = 2; // Pin D4 (GPIO 2)
const byte ledPin = 5; // Pin D1 (GPIO 5)
#define DHTTYPE DHT11

DHT dht(dhtPin, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastTelemetryTime = 0;
const unsigned long telemetryInterval = 4000; // Siklus kirim telemetri 4 detik

// State Latch untuk pencegahan spamming alert (Edge Safety Engine)
bool overheatLatch = false;
const float TEMP_THRESHOLD = 31.0;

void callback(char* topic, byte* payload, unsigned int length) {
  char incomingBuffer[length + 1];
  memcpy(incomingBuffer, payload, length);
  incomingBuffer[length] = '\0';

  Serial.println("----------------------------------------");
  Serial.printf("[KONTROL DITERIMA] Topik: %s | Payload: %s\n", topic, incomingBuffer);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, incomingBuffer);
  if (err) {
    Serial.println("Gagal membedah payload JSON!");
    return;
  }

  // Kendali Manual Override: Hanya diizinkan jika kondisi aman (tidak overheat)
  if (doc.containsKey("led")) {
    if (overheatLatch) {
      Serial.println("--> [OVERRIDE DITOLAK] Sistem BAHAYA (Overheat)! LED dikunci NYALA.");
    } else {
      int val = doc["led"];
      digitalWrite(ledPin, val == 1 ? HIGH : LOW);
      Serial.printf("--> [KENDALI MANUAL] Status LED diubah: %s\n", val == 1 ? "MENYALA" : "PADAM");
    }
  }
  Serial.println("----------------------------------------");
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Menghubungkan kembali ke Broker...");
    
    // Konfigurasi Last Will and Testament (LWT)
    const char* lwtTopic = topicStatus;
    const char* lwtMessage = "{\"status\": \"OFFLINE\", \"keterangan\": \"Perangkat Terputus Tak Terduga\"}";
    
    // Menghubungkan ke broker dengan parameter LWT (QoS 1, Retain true)
    if (client.connect(clientId, NULL, NULL, lwtTopic, 1, true, lwtMessage)) {
      Serial.println(" Sukses Terhubung!");
      
      // Publikasikan status ONLINE sesaat setelah berhasil terhubung (Retained)
      StaticJsonDocument<128> onlineDoc;
      onlineDoc["status"] = "ONLINE";
      onlineDoc["keterangan"] = "Perangkat Beroperasi Normal";
      char onlineBuffer[128];
      serializeJson(onlineDoc, onlineBuffer);
      client.publish(topicStatus, onlineBuffer, true);

      // Subskripsi topik kendali manual
      client.subscribe(topicSubscribe);
      Serial.printf("Subskripsi Topik Kendali: %s\n", topicSubscribe);
    } else {
      Serial.printf("Gagal (rc=%d). Coba lagi dalam 4 detik...\n", client.state());
      delay(4000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Terkoneksi!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); // Memelihara sesi TCP dan mendengarkan callback masuk

  unsigned long now = millis();
  if (now - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      // 1. Klasifikasi Status Lingkungan Berdasarkan Ambang Batas 31.0°C
      String kondisiSuhu = (t > TEMP_THRESHOLD) ? "PANAS" : "NORMAL";

      // 2. Edge Safety Engine & Alert Trigger (State Latch)
      if (t > TEMP_THRESHOLD) {
        digitalWrite(ledPin, HIGH); // Otomatis menyalakan LED indikator bahaya
        if (!overheatLatch) {
          overheatLatch = true; // Latch diaktifkan agar pesan alert tidak spamming setiap siklus
          
          // Publikasikan paket peringatan bahaya ke dedicated alert topic
          StaticJsonDocument<200> alertDoc;
          alertDoc["status"] = "BAHAYA";
          alertDoc["parameter"] = "OVERHEAT";
          alertDoc["nilai_suhu"] = serialized(String(t, 2));
          
          char alertBuffer[200];
          serializeJson(alertDoc, alertBuffer);
          client.publish(topicAlert, alertBuffer);
          Serial.printf("[ALERT BAHAYA TERKIRIM] %s\n", alertBuffer);
        }
      } else {
        // Reset latch jika suhu kembali aman/normal
        if (overheatLatch) {
          overheatLatch = false;
          Serial.println("[INFO] Suhu kembali normal. Latch bahaya direset.");
        }
      }

      // 3. Serialisasi Data Telemetri Rutin
      StaticJsonDocument<256> docOut;
      docOut["node_id"] = clientId;
      docOut["suhu"] = serialized(String(t, 2));
      docOut["kelembapan"] = serialized(String(h, 2));
      docOut["kondisi_suhu"] = kondisiSuhu;
      docOut["status_led"] = digitalRead(ledPin);
      docOut["uptime_detik"] = millis() / 1000;
      docOut["ip_lokal"] = WiFi.localIP().toString();

      char buffer[256];
      serializeJson(docOut, buffer);
      
      bool success = client.publish(topicPublish, buffer);
      if (success) {
        Serial.printf("[TELEMETRI TERKIRIM] %s\n", buffer);
      } else {
        Serial.println("[ERROR] Gagal memublikasikan telemetri!");
      }
    }
  }
}