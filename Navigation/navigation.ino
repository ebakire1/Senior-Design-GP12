#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <SPI.h>         
#include <SD.h>
#include <driver/i2s.h>  
#include <mbedtls/base64.h> // Re-enabled for STT JSON building
#include <cctype>

// ───────────── Wi-Fi ─────────────
const char* ssid     = "Evansiphone"; // Your WiFi SSID
const char* pass     = "testing?";    // Your WiFi Password

// ───────────── API Keys ─────────────
const char* google_api_key   = "AIzaSyAQQ6A-MWtmhYh7xGJfMaJ75-Fpa4skKbc"; // For Google STT & Directions
const char* deepgram_api_key = "4ab68c1e276bc464652a484d8fc831ff46c2f1a3"; // For Deepgram TTS

// ───────────── SD (VSPI) pins ─────────────
#define SD_CARD_CS_PIN 5    // CS to GPIO5; MOSI=23, MISO=19, SCK=18 (Default VSPI)

// ───────────── I²S (INMP441 Microphone) pins & port ─────────────
#define I2S_MIC_PORT_NUM   I2S_NUM_0
#define I2S_MIC_WS_PIN     25 // Word Select
#define I2S_MIC_SD_PIN     32 // Data In from Mic
#define I2S_MIC_SCK_PIN    26 // Serial Clock

// ───────────── I²S (MAX98357A Amplifier) pins & port ─────────────
#define I2S_SPEAKER_PORT_NUM I2S_NUM_1 
#define I2S_SPEAKER_BCK_PIN  26 // Bit Clock (Shared with Mic SCK)
#define I2S_SPEAKER_WS_PIN   25 // Word Select (Shared with Mic WS)
#define I2S_SPEAKER_DATA_PIN 22 // Data Out to Amp

// ───────────── MAX98357A Amplifier Shutdown Pin Control ─────────────
// !!! IMPORTANT: GPIO 32 is used by I2S_MIC_SD_PIN. Choose a DIFFERENT free GPIO for AMP_SD_PIN !!!
// Example: Using GPIO 4. Verify and change if needed. Wire MAX98357A SD pin to this GPIO.
#define AMP_SD_PIN   4  

// ───────────── STT Recording Params ─────────────
#define STT_SAMPLE_RATE     16000
#define STT_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT 
#define STT_CHAN_FMT        I2S_CHANNEL_FMT_ONLY_RIGHT 
#define STT_COMM_FMT        I2S_COMM_FORMAT_I2S_MSB 
#define STT_DMA_COUNT       4
#define STT_DMA_LEN         1024
#define STT_RECORD_MS       5000               // 5 seconds
#define STT_RAW_FILE        "/rec.raw"
#define STT_JSON_FILE       "/rec.json"

// ───────────── TTS Playback Params ─────────────
#define I2S_SPEAKER_SAMPLE_RATE 14500 

// ───────────── Directions API params ─────────────
const char* fixed_origin      = "28.601496,-81.198220"; 

const char* fixed_destination = "UCF+Engineering+1+Orlando+FL"; // Fallback destination

WiFiClientSecure secureClient; // Secure client for HTTPS

// Forward declaration for stripHtml if its definition is later
String stripHtml(const char* html); 


String urlEncode(const char* msg) {
  String encodedMsg = "";
  while (*msg) {
    if (isalnum(*msg)) { // Keep alphanumeric characters as they are
      encodedMsg += *msg;
    } else if (*msg == ' ') {
      encodedMsg += "+"; // Replace spaces with '+'
    } else {
      // You could add encoding for other special characters here if needed
    }
    msg++;
  }
  return encodedMsg;
}

//**************************************************************************************************
//  STT Related Functions (from user's STT code)
//**************************************************************************************************

String extractDestination_STT(const String& transcript) {
  String lower = transcript;
  lower.toLowerCase();
  const String key = "directions ";
  if (!lower.startsWith(key)) {
    return String(); 
  }
  String dest = transcript.substring(key.length());
  dest.trim();       
  return dest;       
}

void initMicI2S() {
  Serial.println("\n🔊 Initializing I2S for Microphone (INMP441)...");
  i2s_config_t i2s_mic_config = {
    .mode            = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate     = STT_SAMPLE_RATE,
    .bits_per_sample = STT_BITS_PER_SAMPLE,
    .channel_format  = STT_CHAN_FMT,
    .communication_format = STT_COMM_FMT, 
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count   = STT_DMA_COUNT,
    .dma_buf_len     = STT_DMA_LEN,
    .use_apll        = false,
    .tx_desc_auto_clear = false, 
    .fixed_mclk      = 0
  };
  i2s_pin_config_t mic_pin_config = {
    .bck_io_num   = I2S_MIC_SCK_PIN,
    .ws_io_num    = I2S_MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE, 
    .data_in_num  = I2S_MIC_SD_PIN
  };
  i2s_driver_uninstall(I2S_MIC_PORT_NUM); 
  esp_err_t err = i2s_driver_install(I2S_MIC_PORT_NUM, &i2s_mic_config, 0, NULL);
   if (err != ESP_OK) {
    Serial.printf("❌ I2S Mic driver install failed: %s\n", esp_err_to_name(err));
    return;
  }
  err = i2s_set_pin(I2S_MIC_PORT_NUM, &mic_pin_config);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S Mic set pin failed: %s\n", esp_err_to_name(err));
    return;
  }
  Serial.println("✅ I2S Mic driver installed.");
}

void recordToRaw_STT() {
  Serial.println("\n🔴 Recording 5 s of raw PCM to " STT_RAW_FILE);
  File f = SD.open(STT_RAW_FILE, FILE_WRITE);
  if (!f) {
    Serial.println("❌ STT: open raw failed");
    while (1) delay(1000);
  }
  uint8_t buf[STT_DMA_LEN * (STT_BITS_PER_SAMPLE / 8)]; 
  size_t  bytes_read_from_i2s;
  unsigned long t0 = millis();
  while (millis() - t0 < STT_RECORD_MS) {
    esp_err_t result = i2s_read(I2S_MIC_PORT_NUM, buf, sizeof(buf), &bytes_read_from_i2s, portMAX_DELAY);
    if (result == ESP_OK) {
        f.write(buf, bytes_read_from_i2s);
    } else {
        Serial.printf("❌ STT: I2S read error: %s\n", esp_err_to_name(result));
    }
  }
  f.close();
  Serial.println("⏹ STT: Done recording raw PCM");
}

void buildJsonOnSD_STT() {
  Serial.println("\n📦 Building STT JSON payload to " STT_JSON_FILE);
  File raw = SD.open(STT_RAW_FILE, FILE_READ);
  if (!raw) {
    Serial.println("❌ STT: raw open failed for JSON build");
    return;
  }
  SD.remove(STT_JSON_FILE); 
  File js = SD.open(STT_JSON_FILE, FILE_WRITE);
  if (!js) {
    raw.close();
    Serial.println("❌ STT: json open failed for JSON build");
    return;
  }

  const char* pre =
    "{\"config\":{\"encoding\":\"LINEAR16\","
               "\"sampleRateHertz\":16000,"
               "\"languageCode\":\"en-US\"},"
     "\"audio\":{\"content\":\"";
  js.print(pre);

  const size_t CHUNK_FOR_BASE64 = 1023;  
  static uint8_t  inBuf[CHUNK_FOR_BASE64];
  static uint8_t  b64[ (CHUNK_FOR_BASE64 / 3 + 1) * 4 + 1]; 
  size_t          bytes_read_from_raw, base64_output_len;
  while ((bytes_read_from_raw = raw.read(inBuf, CHUNK_FOR_BASE64)) > 0) {
    mbedtls_base64_encode(b64, sizeof(b64), &base64_output_len, inBuf, bytes_read_from_raw);
    js.write(b64, base64_output_len);
  }
  raw.close();

  const char* suf = "\"}}";
  js.print(suf);
  js.close();
  Serial.println("✅ STT: JSON built on SD");
}

String sendJsonFile_STT() {
  Serial.println("\n🚀 Sending " STT_JSON_FILE " to Google STT");
  String extracted_destination = "";

  File js = SD.open(STT_JSON_FILE, FILE_READ);
  if (!js) {
    Serial.println("❌ STT: json open failed for sending");
    return extracted_destination; 
  }

  HTTPClient http;
  String url = String("https://speech.googleapis.com/v1/speech:recognize?key=") + google_api_key;
  
  http.begin(secureClient, url); 
  http.addHeader("Content-Type", "application/json; charset=utf-8");
  http.setTimeout(30000); 

  int code = http.sendRequest("POST", &js, js.size());
  long fileSizeSent = js.size(); 
  js.close(); 
  Serial.printf("  STT: Sent %ld bytes. HTTP Response Code: %d\n", fileSizeSent, code);

  if (code > 0) {
    String resp = http.getString();
    Serial.println("  STT Response Payload:");
    Serial.println(resp);

    DynamicJsonDocument doc(1024); 
    DeserializationError error = deserializeJson(doc, resp);

    if (error) {
      Serial.print(F("  STT: deserializeJson() failed: "));
      Serial.println(error.c_str());
    } else {
      if (doc["results"] && doc["results"][0] && doc["results"][0]["alternatives"] && doc["results"][0]["alternatives"][0] && doc["results"][0]["alternatives"][0]["transcript"]) {
        const char* transcript_char = doc["results"][0]["alternatives"][0]["transcript"];
        String transcript(transcript_char);
        Serial.print("🎤 You said: ");
        Serial.println(transcript);
        extracted_destination = extractDestination_STT(transcript);
        if (extracted_destination.length() > 0) {
          Serial.println("🚀 Destination (from STT): " + extracted_destination);
        } else {
          Serial.println("⚠️ Invalid input – must start with “directions ” or no destination found.");
        }
      } else {
        Serial.println("⚠️ No transcript found in STT JSON response.");
      }
    }
  } else {
    Serial.println("❌ STT: HTTP POST failed: " + http.errorToString(code));
  }
  http.end();
  return extracted_destination;
}

//**************************************************************************************************
//  TTS and Navigation Related Functions
//**************************************************************************************************

// Definition of stripHtml (moved earlier or use forward declaration)
String stripHtml(const char* html_content) { // Renamed parameter to avoid conflict with global html
  String s = html_content;
  int a, b;
  while ((a = s.indexOf('<')) >= 0 && (b = s.indexOf('>', a)) >= 0) {
    String before = s.substring(0, a);
    String after  = s.substring(b + 1);
    s = before + " " + after; 
  }
  s.trim(); 
  return s;
}

void initSpeakerI2S() { 
  Serial.println(F("🔊 Initializing I2S Audio Output for Speaker (MAX98357A) using ESP-IDF..."));
  i2s_config_t i2s_speaker_config = { 
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = I2S_SPEAKER_SAMPLE_RATE, 
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, 
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t speaker_pin_config = {
    .bck_io_num = I2S_SPEAKER_BCK_PIN,
    .ws_io_num = I2S_SPEAKER_WS_PIN,
    .data_out_num = I2S_SPEAKER_DATA_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE 
  };

  i2s_driver_uninstall(I2S_SPEAKER_PORT_NUM); 
  esp_err_t err = i2s_driver_install(I2S_SPEAKER_PORT_NUM, &i2s_speaker_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S Speaker driver install failed: %s\n", esp_err_to_name(err));
    return;
  }
  err = i2s_set_pin(I2S_SPEAKER_PORT_NUM, &speaker_pin_config);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S Speaker set pin failed: %s\n", esp_err_to_name(err));
    return;
  }
  Serial.println(F("✅ I2S Speaker Output Initialized (ESP-IDF)."));
}

void playRawAudioFile_TTS(const char* filename) { 
  Serial.printf("▶️ Attempting to play RAW audio file (TTS): %s\n", filename);
  File audioFile = SD.open(filename, FILE_READ);
  if (!audioFile) {
    Serial.printf("❌ Failed to open RAW audio file %s\n", filename);
    return;
  }
  if (audioFile.size() == 0) {
    Serial.printf("⚠️ File %s is empty (0 bytes).\n", filename);
    audioFile.close();
    return;
  }
  Serial.printf("🔊 Playing %s (Size: %d bytes)\n", filename, audioFile.size());

  const size_t i2s_write_buffer_size = 1024 * 2; 
  uint8_t i2s_write_buff[i2s_write_buffer_size];
  size_t bytes_read_from_sd = 0;
  size_t bytes_written_to_i2s = 0;
  unsigned long total_bytes_played = 0;

  i2s_zero_dma_buffer(I2S_SPEAKER_PORT_NUM); 

  while ((bytes_read_from_sd = audioFile.read(i2s_write_buff, i2s_write_buffer_size)) > 0) {
    esp_err_t err = i2s_write(I2S_SPEAKER_PORT_NUM, i2s_write_buff, bytes_read_from_sd, &bytes_written_to_i2s, portMAX_DELAY);
    if (err != ESP_OK) {
      Serial.printf("❌ I2S write error: %s\n", esp_err_to_name(err));
      break;
    }
    if (bytes_written_to_i2s < bytes_read_from_sd) {
      Serial.printf("⚠️ I2S underrun: tried to write %u, only wrote %u\n", bytes_read_from_sd, bytes_written_to_i2s);
    }
    total_bytes_played += bytes_written_to_i2s;
    yield(); 
  }
  audioFile.close();
  Serial.printf("🎵 Finished sending %s to I2S. Total bytes: %lu\n", filename, total_bytes_played);

  if (total_bytes_played > 0) {
    unsigned long actual_sample_rate = I2S_SPEAKER_SAMPLE_RATE; 
    int bytes_per_sample = 2; 
    unsigned long estimated_playback_duration_ms = (total_bytes_played * 1000) / (actual_sample_rate * bytes_per_sample);
    
    unsigned long wait_time_ms = estimated_playback_duration_ms + 100; 
    const unsigned long MINIMUM_PLAY_WAIT_MS = 400; 

    if (wait_time_ms < MINIMUM_PLAY_WAIT_MS) {
        Serial.printf("  Calculated wait time %lu ms is less than minimum %lu ms. Using minimum.\n", wait_time_ms, MINIMUM_PLAY_WAIT_MS);
        wait_time_ms = MINIMUM_PLAY_WAIT_MS;
    }
    
    Serial.printf("  Waiting %lu ms for audio to finish playing (estimated: %lu ms from %lu bytes at %lu Hz)...\n", 
                  wait_time_ms, estimated_playback_duration_ms, total_bytes_played, actual_sample_rate);
    delay(wait_time_ms); 
  }
  
  i2s_zero_dma_buffer(I2S_SPEAKER_PORT_NUM); 
  Serial.println(F("✅ I2S DMA buffer cleared after playback."));
}

bool synthToSd_TTS(const char* text, const char* outPath) { 
  Serial.printf("🤖 Synthesizing (Deepgram TTS - RAW PCM) to SD: \"%s\" → %s\n", text, outPath);

  if (strlen(deepgram_api_key) < 10 || strcmp(deepgram_api_key, "YOUR_DEEPGRAM_API_KEY") == 0 ) {
      Serial.println(F("❌ Deepgram API key is not set or is placeholder. Please update."));
      return false;
  }

  String jsonPayload = "{\"text\":\"" + String(text) + "\"}";
  Serial.println(F("  TTS JSON Request (for Deepgram RAW PCM):"));
  Serial.println(jsonPayload);

  HTTPClient http;
  String url = "https://api.deepgram.com/v1/speak?encoding=linear16&sample_rate=16000&model=aura-asteria-en"; 
  Serial.printf("  POST to: %s\n", url.c_str());

  http.begin(secureClient, url); 
  http.addHeader("Content-Type", "application/json");
  String authHeader = String("Token ") + deepgram_api_key;
  http.addHeader("Authorization", authHeader);
  
  http.setTimeout(180000); 

  int httpCode = http.POST(jsonPayload);
  Serial.printf("  Deepgram TTS HTTP Response Code: %d\n", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    SD.remove(outPath); 
    File ttsFile = SD.open(outPath, FILE_WRITE);
    if (!ttsFile) {
      Serial.printf("❌ Failed to open %s on SD card for writing!\n", outPath);
      http.end();
      return false;
    }
    
    Serial.printf("  Saving Deepgram TTS output (RAW PCM) to %s ...\n", outPath);
    
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        Serial.println(F("❌ Failed to get stream pointer from HTTPClient."));
        ttsFile.close();
        SD.remove(outPath);
        http.end();
        return false;
    }

    uint8_t buff[1024] = { 0 }; 
    size_t totalBytesWritten = 0;
    size_t bytesReadFromStream = 0;
    unsigned long lastDataTime = millis();
    const unsigned long streamReadTimeout = 60000; 

    Serial.println(F("  Starting manual stream read/write loop (for RAW audio)..."));
    while (http.connected() && (stream->available() || (millis() - lastDataTime < streamReadTimeout))) {
        if (!http.connected()){ 
            Serial.println(F("❌ HTTP connection lost during stream read."));
            break;
        }
        if (stream->available()) {
            bytesReadFromStream = stream->readBytes(buff, sizeof(buff));
            if (bytesReadFromStream > 0) {
                size_t written = ttsFile.write(buff, bytesReadFromStream);
                if (written != bytesReadFromStream) {
                    Serial.printf("❌ SD Write Error: Tried to write %u, but wrote %u\n", bytesReadFromStream, written);
                    totalBytesWritten = (size_t)-1; 
                    break;
                }
                totalBytesWritten += written;
                lastDataTime = millis(); 
            }
        } else {
            yield(); 
            delay(10); 
        }
    }
    Serial.println(F("  Finished manual stream read/write loop."));
    ttsFile.close();

    if (totalBytesWritten != (size_t)-1 && totalBytesWritten > 0) {
        Serial.printf("✅ Saved successfully! File size: %u bytes.\n", totalBytesWritten);
        http.end(); 
        return true;
    } else if (totalBytesWritten == 0 && httpCode == HTTP_CODE_OK) {
        Serial.println(F("❌ Wrote 0 bytes to SD card, but HTTP response was OK. Stream was empty or timed out immediately."));
        SD.remove(outPath); 
        http.end();
        return false;
    }
    else { 
        Serial.printf("❌ Failed to write stream to file. Bytes written: %u. Check for SD errors or earlier stream issues.\n", totalBytesWritten);
        SD.remove(outPath); 
        http.end();
        return false;
    }
  } else {
    Serial.printf("❌ Deepgram TTS request failed. HTTP Code: %d\n", httpCode);
    String errorPayload = http.getString(); 
    if (errorPayload.length() > 0) {
        Serial.println(F("  Error payload:"));
        Serial.println(errorPayload);
    }
    http.end();
    return false;
  }
}

//──────────────────────────────────────────────────────────
//  Process Navigation: Get directions and speak steps
//──────────────────────────────────────────────────────────
void processNavigationAndTTS(String destinationQuery) {
  Serial.println(F("\n🗺️ Fetching Directions from Google Maps API..."));
  if (destinationQuery.isEmpty()) {
      Serial.println(F("  No destination provided. Using fixed destination for test."));
      destinationQuery = fixed_destination; // Use fixed_destination
  }
  String dirURL = String("https://maps.googleapis.com/maps/api/directions/json")
                + "?origin="      + String(fixed_origin) 
                + "&destination=" + urlEncode(destinationQuery.c_str())
                + "&mode=walking" 
                + "&key="         + String(google_api_key); 

  Serial.print(F("   GET Request URL: "));
  Serial.println(dirURL);

  HTTPClient http_dir; 
  http_dir.begin(secureClient, dirURL); 
  int httpCode_dir = http_dir.GET();
  Serial.printf("   Directions API HTTP Response Code: %d\n", httpCode_dir);

  if (httpCode_dir != HTTP_CODE_OK) {
    Serial.printf("❌ Directions API GET request failed. Error: %s\n", http_dir.errorToString(httpCode_dir).c_str());
    if (http_dir.getSize() > 0) {
      Serial.println(F("   Error payload:"));
      Serial.println(http_dir.getString());
    }
    http_dir.end();
    Serial.println(F("Halting due to Directions API error."));
    return; 
  }

  String payload = http_dir.getString();
  http_dir.end();
  Serial.println(F("✅ Directions API Response Received."));

  Serial.println(F("📄 Parsing Directions JSON..."));
  StaticJsonDocument<20 * 1024> doc_directions; 
  DeserializationError error = deserializeJson(doc_directions, payload);

  if (error) {
    Serial.print(F("❌ Directions JSON parsing failed: "));
    Serial.println(error.c_str());
    Serial.println(F("   Received payload (first 500 chars):"));
    Serial.println(payload.substring(0, 500));
    Serial.println(F("Halting due to JSON parsing error."));
    return; 
  }
  Serial.println(F("✅ Directions JSON Parsed Successfully."));

  if (!doc_directions.containsKey("routes") || !doc_directions["routes"].is<JsonArray>() || doc_directions["routes"].as<JsonArray>().size() == 0) {
    Serial.println(F("❌ No 'routes' found in Directions API response or routes array is empty."));
    return;
  }
  if (!doc_directions["routes"][0].containsKey("legs") || !doc_directions["routes"][0]["legs"].is<JsonArray>() || doc_directions["routes"][0]["legs"].as<JsonArray>().size() == 0) {
    Serial.println(F("❌ No 'legs' found in the first route."));
    return;
  }
   if (!doc_directions["routes"][0]["legs"][0].containsKey("steps") || !doc_directions["routes"][0]["legs"][0]["steps"].is<JsonArray>() || doc_directions["routes"][0]["legs"][0]["steps"].as<JsonArray>().size() == 0) {
    Serial.println(F("❌ No 'steps' found in the first leg of the first route."));
    return;
  }

  JsonArray steps = doc_directions["routes"][0]["legs"][0]["steps"];
  Serial.printf("   Found %d navigation steps.\n", steps.size());

  int stepIdx = 0;
  for (JsonObject step : steps) {
    stepIdx++;
    const char* htmlInstr = step["html_instructions"];
    if (!htmlInstr) {
      Serial.printf("⚠️ Step %02d: No 'html_instructions' found. Skipping.\n", stepIdx);
      continue;
    }

    String instr = stripHtml(htmlInstr); 
    Serial.printf("\nProcessing Step %02d: \"%s\"\n", stepIdx, instr.c_str());

    char fn[20]; 
    snprintf(fn, sizeof(fn), "/step%02d.raw", stepIdx); 

    if (synthToSd_TTS(instr.c_str(), fn)) { 
        Serial.printf("✅ RAW audio file %s created for Step %02d.\n", fn, stepIdx);
        playRawAudioFile_TTS(fn); 
    } else {
        Serial.printf("❌ TTS→SD failed on step %02d.\n", stepIdx);
    }
    
    Serial.printf("Pausing for 10 seconds after Step %02d...\n", stepIdx); 
    delay(10000); 
  }
  
  Serial.println(F("\n✅ All navigation steps processed."));
}


//**************************************************************************************************
//  Main Setup and Loop
//**************************************************************************************************
void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println(F("\n\n🚀 ESP32 Voice Navigation System Booting Up..."));

  // 1. Initialize Wi-Fi
  Serial.print(F("📡 Connecting to Wi-Fi: "));
  Serial.print(ssid);
  WiFi.begin(ssid, pass);
  int wifi_retries = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_retries < 30) { 
    Serial.print('.');
    delay(500);
    wifi_retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n✅ Wi-Fi Connected!"));
    Serial.print(F("   IP Address: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("\n❌ Wi-Fi Connection Failed. Halting."));
    while (1) delay(1000);
  }
  
  secureClient.setInsecure(); 
  Serial.println(F("🔒 WiFiClientSecure configured for insecure connections."));

  // 2. Initialize SD Card
  Serial.println(F("💾 Initializing SD Card..."));
  if (!SD.begin(SD_CARD_CS_PIN)) { 
    Serial.println(F("❌ SD Card Mount Failed! Check wiring, card presence, and formatting (FAT32). Halting."));
    while (1) delay(1000);
  }
  Serial.println(F("✅ SD Card Initialized."));

  // 3. STT Phase: Record audio, send to Google STT, get destination
  Serial.println(F("\n--- Starting Speech-to-Text Phase ---"));
  initMicI2S(); 
  
  #if defined(AMP_SD_PIN) 
    pinMode(AMP_SD_PIN, OUTPUT);
    digitalWrite(AMP_SD_PIN, LOW); 
    Serial.printf("🎤 Amplifier SD Pin (%d) set LOW (disabled for mic recording).\n", AMP_SD_PIN);
  #endif

  recordToRaw_STT();
  buildJsonOnSD_STT();
  String destinationFromSTT = sendJsonFile_STT();
  
  i2s_driver_uninstall(I2S_MIC_PORT_NUM); 
  Serial.println(F("🎤 I2S Mic driver uninstalled."));
  Serial.println(F("--- Speech-to-Text Phase Complete ---"));


  // 4. Navigation and TTS Phase
  if (destinationFromSTT.length() > 0) {
    Serial.println(F("\n--- Starting Navigation & Text-to-Speech Phase ---"));
    initSpeakerI2S(); 

    #if defined(AMP_SD_PIN) 
      pinMode(AMP_SD_PIN, OUTPUT); 
      digitalWrite(AMP_SD_PIN, HIGH); 
      Serial.printf("🔊 Amplifier SD Pin (%d) set HIGH (enabled for TTS playback).\n", AMP_SD_PIN);
    #else
      Serial.println(F("⚠️ Amplifier SD Pin control not defined. Ensure MAX98357A SD pin is managed correctly for sound."));
    #endif
    
    processNavigationAndTTS(destinationFromSTT);

    Serial.println(F("Playing personal audio.raw file..."));
    playRawAudioFile_TTS("/Arrived.raw");

    i2s_driver_uninstall(I2S_SPEAKER_PORT_NUM); 
    Serial.println(F("🔊 I2S Speaker driver uninstalled."));
    Serial.println(F("--- Navigation & Text-to-Speech Phase Complete ---"));

  } else {
    Serial.println(F("\n⚠️ No valid destination obtained from STT. Skipping navigation and TTS."));
  }
  
  Serial.println(F("\n Program finished. Entering idle loop."));
}

void loop() {
  delay(10000); 
}
