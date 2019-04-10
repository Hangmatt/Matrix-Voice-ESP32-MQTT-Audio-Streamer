/* ************************************************************************* *
 * Matrix Voice Audio Streamer
 * 
 * This program is written to be a streaming audio server running on the Matrix Voice.
 This is typically used for Snips.AI, it will then be able to replace
 * the Snips Audio Server, by publishing small wave messages to the hermes protocol
 * See https://snips.ai/ for more information
 * 
 * Author:  Paul Romkes
 * Date:    September 2018
 * Version: 3.3
 * 
 * Changelog:
 * ==========
 * v1:
 *  - first code release. It needs a lot of improvement, no hardcoding stuff
 * v2:
 *  - Change to Arduino IDE
 * v2.1:
 *  - Changed to pubsubclient and fixed other stability issues
 * v3:
 *  - Add OTA
 * v3.1:
 *  - Only listen to SITEID to toggle hotword
 *  - Got rid of String, leads to Heap Fragmentation
 *  - Add dynamic brihtness, post {"brightness": 50 } to SITEID/everloop
 *  - Fix stability, using semaphores
 * v3.2:
 *  - Add dynamic colors, see readme for documentation
 *  - Restart the device by publishing hashed password to SITEID/restart
 *  - Adjustable framerate, more info at https://snips.gitbook.io/documentation/advanced-configuration/platform-configuration 
 *  - Rotating animation possible, not finished or used yet
 * v3.3:
 *  - Added support for Rhasspy https://github.com/synesthesiam/rhasspy
 *  - Started implementing playBytes, not finished
 * v3.4:
 *  - Implemented playBytes, basics done but sometimes audio garbage out
 * ************************************************************************ */
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>
#include <AsyncMqttClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <string>
#include "wishbone_bus.h"
#include "everloop.h"
#include "everloop_image.h"
#include "microphone_array.h"
#include "microphone_core.h"
#include "voice_memory_map.h"
#include <thread>
#include <chrono>

extern "C" {
  #include "freertos/FreeRTOS.h"
  #include "freertos/timers.h"
  #include "freertos/event_groups.h"
}
#include "config.h"
#include "webpages.h"
#include "RingBuf.h"

/* ************************************************************************* *
 *    DEFINES AND GLOBALS
 * ************************************************************************ */
#define RATE 16000
#define SITEID "matrixvoice"
//Change to your own IP
#define MQTT_IP IPAddress(192, 168, 0, 148)
#define MQTT_HOST "192.168.0.148"
#define MQTT_PORT 1883
#define WIDTH 2
#define CHANNELS 1
#define DATA_CHUNK_ID 0x61746164
#define FMT_CHUNK_ID 0x20746d66
// Convert 4 byte little-endian to a long, 
#define longword(bfr, ofs) (bfr[ofs+3] << 24 | bfr[ofs+2] << 16 |bfr[ofs+1] << 8 |bfr[ofs+0])

//Matrix Voice
namespace hal = matrix_hal;
static hal::WishboneBus wb;
static hal::Everloop everloop;
static hal::MicrophoneArray mics;
static hal::EverloopImage image1d;
WiFiClient net;
AsyncMqttClient asyncClient; //ASYNCH client to be able to handle huge messages like WAV files
PubSubClient audioServer(net); //We also need a sync client, asynch leads to errors on the audio thread
WebServer server(80);
//Timers
TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;
TaskHandle_t audioTaskHandle;
SemaphoreHandle_t wbSemaphore;
//Globals
const int kMaxWriteLength = 1024;
uint8_t data[kMaxWriteLength];
RingBuf<uint8_t, kMaxWriteLength*4> audioData;
struct wavfile_header {
  char  riff_tag[4];    //4
  int   riff_length;    //4
  char  wave_tag[4];    //4
  char  fmt_tag[4];     //4
  int   fmt_length;     //4
  short audio_format;   //2
  short num_channels;   //2
  int   sample_rate;    //4
  int   byte_rate;      //4
  short block_align;    //2
  short bits_per_sample;//2
  char  data_tag[4];    //4
  int   data_length;    //4
};
static struct wavfile_header header;
const int EVERLOOP = BIT0;
const int ANIMATE = BIT1;
const int PLAY = BIT2;
int hotword_colors[4] = {0,255,0,0};
int idle_colors[4] = {0,0,255,0};
int wifi_disc_colors[4] = {255,0,0,0};
int update_colors[4] = {0,0,0,255};
int brightness = 15;
bool DEBUG = false; //If set to true, code will post several messages to topics in case of events
bool wifi_connected = false;
bool hotword_detected = false;
bool isUpdateInProgess =  false;
//Change to your own password hash at https://www.md5hashgenerator.com/
const char* passwordhash = "4b8d34978fafde81a85a1b91a09a881b";
const char* host = "matrixvoice";
std::string finishedMsg = "";
int message_count;
int samplerate;
int CHUNK = 256; //set to multiplications of 256, voice return a set of 256
int chunkValues[] = {32,64,128,256,512,1024};
static EventGroupHandle_t everloopGroup;
static EventGroupHandle_t audioGroup;
//This is used to be able to change brightness, while keeping the colors appear the same
//Called gamma correction, check this https://learn.adafruit.com/led-tricks-gamma-correction/the-issue
const uint8_t PROGMEM gamma8[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  5,  5,  5,
    5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10,
   10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
   17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
   25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
   37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
   51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
   69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
   90, 92, 93, 95, 96, 98, 99,101,102,104,105,107,109,110,112,114,
  115,117,119,120,122,124,126,127,129,131,133,135,137,138,140,142,
  144,146,148,150,152,154,156,158,160,162,164,167,169,171,173,175,
  177,180,182,184,186,189,191,193,196,198,200,203,205,208,210,213,
  215,218,220,223,225,228,231,233,236,239,241,244,247,249,252,255 };


/* ************************************************************************* *
 *    MQTT TOPICS
 * ************************************************************************ */
//Dynamic topics for MQTT
std::string audioFrameTopic = std::string("hermes/audioServer/") + SITEID + std::string("/audioFrame");
std::string playFinishedTopic = std::string("hermes/audioServer/") + SITEID + std::string("/playFinished");
std::string playBytesTopic = std::string("hermes/audioServer/") + SITEID + std::string("/playBytes/#");
std::string rhasspyWakeTopic = std::string("rhasspy/nl/transition/HermesWakeListener");
std::string toggleOffTopic = "hermes/hotword/toggleOff";
std::string toggleOnTopic = "hermes/hotword/toggleOn";
std::string everloopTopic = SITEID + std::string("/everloop");
std::string debugTopic = SITEID + std::string("/debug");
std::string audioTopic = SITEID + std::string("/audio");
std::string restartTopic = SITEID + std::string("/restart");

/* ************************************************************************* *
 *    HELPER CLASS FOR WAVE HEADER, taken from https://www.xtronical.com/
 *    Changed to fit my needs
 * ************************************************************************ */
class XT_Wav_Class
{
  public:      
  uint16_t SampleRate;  
  uint32_t DataStart;     // offset of the actual data.
  // constructors
  XT_Wav_Class(const unsigned char *WavData);
};

XT_Wav_Class::XT_Wav_Class(const unsigned char *WavData)
{
   unsigned long ofs, siz;
   ofs = 12;
   siz = longword(WavData, 4);
   SampleRate = DataStart = 0;
   while (ofs < siz) {
      if (longword(WavData, ofs) == DATA_CHUNK_ID) {
        DataStart = ofs +8;
      }
      if (longword(WavData, ofs) == FMT_CHUNK_ID) {
        SampleRate = longword(WavData, ofs+12);
      }
      ofs += longword(WavData, ofs+4) + 8;  
   }
}

/* ************************************************************************* *
 *    NETWORK FUNCTIONS AND MQTT 
 * ************************************************************************ */
void connectToWifi() {
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(SSID, PASSWORD);
}

void connectToMqtt() {
  Serial.println("Connecting to asynch MQTT...");
  asyncClient.connect();
}

void connectAudio() {
 Serial.println("Connecting to synch MQTT...");
 audioServer.connect("MatrixVoiceAudio");
}

// ---------------------------------------------------------------------------
// WIFI event
// Kicks off various stuff in case of connect/disconnect
// ---------------------------------------------------------------------------
void WiFiEvent(WiFiEvent_t event) {
  switch(event) {
  case SYSTEM_EVENT_STA_GOT_IP:
    wifi_connected = true;
    xEventGroupSetBits(everloopGroup, EVERLOOP); //Set the bit so the everloop gets updated
    connectToMqtt();
    connectAudio();
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED:
    wifi_connected = false;
    xEventGroupSetBits(everloopGroup, EVERLOOP);
    xTimerStop(mqttReconnectTimer, 0); //Do not reconnect to MQTT while reconnecting to network
    xTimerStart(wifiReconnectTimer, 0); //Start the reconnect timer
    break;
  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// MQTT Connect event
// ---------------------------------------------------------------------------
void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
  asyncClient.subscribe(playBytesTopic.c_str(), 0);
  asyncClient.subscribe(toggleOffTopic.c_str(),0);
  asyncClient.subscribe(toggleOnTopic.c_str(),0);
  asyncClient.subscribe(rhasspyWakeTopic.c_str(),0);
  asyncClient.subscribe(everloopTopic.c_str(),0);
  asyncClient.subscribe(restartTopic.c_str(),0);
  asyncClient.subscribe(audioTopic.c_str(),0);
  asyncClient.subscribe(debugTopic.c_str(),0);
  xEventGroupClearBits(everloopGroup,ANIMATE);
}

// ---------------------------------------------------------------------------
// MQTT Disonnect event
// ---------------------------------------------------------------------------
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.println("Disconnected from MQTT.");
  if (!isUpdateInProgess) {
     xEventGroupSetBits(everloopGroup, ANIMATE);
     if (WiFi.isConnected()) {
      xTimerStart(mqttReconnectTimer, 0);
     }
  }
}

// ---------------------------------------------------------------------------
// MQTT Callback
// Sets the HOTWORD bits to toggle the leds and published a message on playFinished
// to simulate the playFinished. Without it, Snips will not start listening when the 
// feedback sound is toggled on
// ---------------------------------------------------------------------------
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  std::string topicstr(topic);
  if (len + index == total) {
    //when len + index is total, we have reached the end of the message.
    //We can then do work on it
    if (topicstr.find("toggleOff") != std::string::npos) {
      std::string payloadstr(payload);
      //Check if this is for us
      if (payloadstr.find(SITEID) != std::string::npos) {
        hotword_detected = true;
        xEventGroupSetBits(everloopGroup, EVERLOOP); //Set the bit so the everloop gets updated
      }
    } else if (topicstr.find("toggleOn") != std::string::npos) {
      //Check if this is for us
      std::string payloadstr(payload);
      if (payloadstr.find(SITEID) != std::string::npos) {
        hotword_detected = false;
        xEventGroupSetBits(everloopGroup, EVERLOOP); //Set the bit so the everloop gets updated
      }
    } else if (topicstr.find("HermesWakeListener") != std::string::npos) {
      std::string payloadstr(payload);
      if (payloadstr.find("loaded") != std::string::npos) {
        hotword_detected = true;
        xEventGroupSetBits(everloopGroup, EVERLOOP); //Set the bit so the everloop gets updated
      } 
      if (payloadstr.find("listening") != std::string::npos) {
        hotword_detected = false;
        xEventGroupSetBits(everloopGroup, EVERLOOP); //Set the bit so the everloop gets updated
      }    
    } else if (topicstr.find("playBytes") != std::string::npos) {
      //Get the ID from the topic
      int pos = 19 + strlen(SITEID) + 11;
      finishedMsg = "{\"id\":\"" + topicstr.substr(pos,37) + "\",\"siteId\":\"" + SITEID + "\",\"sessionId\":null}";
      //fill the ringbuffer
      for (uint32_t s = 0; s < len; s++) {
          if (audioData.isFull()) {
            //start playing is the buffer is full
            if( xEventGroupGetBits( audioGroup ) != PLAY  ) {
              xEventGroupSetBits(audioGroup, PLAY);
            }
          }
          while (audioData.isFull()) {
            delay(1);
          }
          audioData.push(payload[s]);
      }        
    } else if (topicstr.find(everloopTopic.c_str()) != std::string::npos) {
      StaticJsonBuffer<300> jsonBuffer;
      JsonObject& root = jsonBuffer.parseObject((char *) payload);
      if (root.success()) {
        if (root.containsKey("brightness")) {
          brightness = root["brightness"];          
        }
        if (root.containsKey("hotword")) {
          hotword_colors[0] = root["hotword"][0];          
          hotword_colors[1] = root["hotword"][1];          
          hotword_colors[2] = root["hotword"][2];          
          hotword_colors[3] = root["hotword"][3];          
        }
        if (root.containsKey("idle")) {
          idle_colors[0] = root["idle"][0];          
          idle_colors[1] = root["idle"][1];          
          idle_colors[2] = root["idle"][2];          
          idle_colors[3] = root["idle"][3];          
        }
        if (root.containsKey("wifi_disconnect")) {
          wifi_disc_colors[0] = root["wifi_disconnect"][0];          
          wifi_disc_colors[1] = root["wifi_disconnect"][1];          
          wifi_disc_colors[2] = root["wifi_disconnect"][2];          
          wifi_disc_colors[3] = root["wifi_disconnect"][3];          
        }
        if (root.containsKey("update")) {
          update_colors[0] = root["update"][0];          
          update_colors[1] = root["update"][1];          
          update_colors[2] = root["update"][2];          
          update_colors[3] = root["update"][3];          
        }
        xEventGroupSetBits(everloopGroup, EVERLOOP);
      }
    } else if (topicstr.find(audioTopic.c_str()) != std::string::npos) {
      StaticJsonBuffer<300> jsonBuffer;
      JsonObject& root = jsonBuffer.parseObject((char *) payload);
      if (root.success()) {
        if (root.containsKey("framerate")) {
          bool found = false;
          for (int i=0; i<6; i++){
            if (chunkValues[i] == root["framerate"]){
              CHUNK = root["framerate"];
              message_count = (int)round(mics.NumberOfSamples() / CHUNK);
              header.riff_length = (uint32_t)sizeof(header) + (CHUNK * WIDTH);
              header.data_length = CHUNK * WIDTH;
              found = true;
              break;
            }
          }
          if (!found) {
             asyncClient.publish(debugTopic.c_str(),0, false, "Framerate should be 32,64,128,256,512 or 1024");
          }
        }
      }
    } else if (topicstr.find(restartTopic.c_str()) != std::string::npos) {
      StaticJsonBuffer<300> jsonBuffer;
      JsonObject& root = jsonBuffer.parseObject((char *) payload);
      if (root.success()) {
        if (root.containsKey("passwordhash")) {
          if (root["passwordhash"] == passwordhash) {
            ESP.restart();          
          }
        }
      }
    }   
  } else {
    //len + index < total ==> partial message   
    if (topicstr.find("playBytes") != std::string::npos) {
      if (index == 0) {
        //when the index is 0, we have the start of a message
        XT_Wav_Class Message((const uint8_t *)payload); 
        samplerate = Message.SampleRate;
        
        //Clear RingBuffer
        uint8_t b = 0;
        for (int i = 0; i < kMaxWriteLength* 4;i++) {
          audioData.push(b);
        }
        audioData.clear();
        
        for (uint32_t s = Message.DataStart; s < len; s++) {
            //skip first 44 bytes of the wav header
            if (audioData.isFull()) {
              if( xEventGroupGetBits( audioGroup ) != PLAY  ) {
                xEventGroupSetBits(audioGroup, PLAY);
              }
            }
            while (audioData.isFull()) {
              delay(1);
            }
            audioData.push(payload[s]);
        }        
      }  else {
        for (uint32_t s = 0; s < len; s++) {
            if (audioData.isFull()) {
              if( xEventGroupGetBits( audioGroup ) != PLAY  ) {
                xEventGroupSetBits(audioGroup, PLAY);
              }
            }
            while (audioData.isFull()) {
              delay(1);
            }
            audioData.push(payload[s]);
        }        
      }
    }
  }  
}

/* ************************************************************************* *
 *    AUDIOSTREAM TASK, USES SYNCED MQTT CLIENT
 * ************************************************************************ */
void Audiostream( void * p ) {
   for(;;){
     // See if we can obtain or "Take" the Serial Semaphore.
     // If the semaphore is not available, wait 5 ticks of the Scheduler to see if it becomes free.
    if (audioServer.connected() && (xSemaphoreTake( wbSemaphore, ( TickType_t ) 5000 ) == pdTRUE) ) {
      //We are connected! 
      mics.Read();
      
      //Sound buffers
      uint16_t voicebuffer[CHUNK];
      uint8_t voicemapped[CHUNK*WIDTH];
      uint8_t payload[sizeof(header)+(CHUNK*WIDTH)];
      
      //Message count is the Mattix NumberOfSamples divided by the framerate of Snips.
      //This defaults to 512 / 256 = 2. If you lower the framerate, the AudioServer has to send more wavefile
      //because the NumOfSamples is a fixed number
      for (int i=0;i<message_count;i++) {
        for (uint32_t s = CHUNK*i; s < CHUNK * (i+1); s++) {
            voicebuffer[s-(CHUNK*i)] = mics.Beam(s);
        }      
        //voicebuffer will hold 256 samples of 2 bytes, but we need it as 1 byte
        //We do a memcpy, because I need to add the wave header as well
        memcpy(voicemapped,voicebuffer,CHUNK*WIDTH);
        
       //Add the wave header
        memcpy(payload,&header,sizeof(header));
        memcpy(&payload[sizeof(header)], voicemapped, sizeof(voicemapped));
        audioServer.publish(audioFrameTopic.c_str(), (uint8_t *)payload, sizeof(payload));
        
      }
      xSemaphoreGive( wbSemaphore ); // Now free or "Give" the Serial Port for others.
    }
    vTaskDelay(1);
  }
  vTaskDelete(NULL);
}

/* ************************************************************************ *
 *    LED ANIMATION TASK
 * ************************************************************************ */
void everloopAnimation(void * p) {
    int position = 0;
    int red;
    int green;
    int blue;
    int white;
    while(1){          
      xEventGroupWaitBits(everloopGroup,ANIMATE,true,true,portMAX_DELAY); //Wait for the bit before updating
      if ( xSemaphoreTake( wbSemaphore, ( TickType_t ) 5000 ) == pdTRUE )
      {
         for (int i = 0; i < image1d.leds.size(); i++) {
          red = ((i+1) * brightness / image1d.leds.size()) * idle_colors[0] / 100;
          green = ((i+1) * brightness / image1d.leds.size()) * idle_colors[1] / 100;
          blue = ((i+1) * brightness / image1d.leds.size()) * idle_colors[2] / 100;
          white = ((i+1) * brightness / image1d.leds.size()) * idle_colors[3] / 100;
          image1d.leds[(i + position) % image1d.leds.size()].red = pgm_read_byte(&gamma8[red]);
          image1d.leds[(i + position) % image1d.leds.size()].green = pgm_read_byte(&gamma8[green]);
          image1d.leds[(i + position) % image1d.leds.size()].blue = pgm_read_byte(&gamma8[blue]);
          image1d.leds[(i + position) % image1d.leds.size()].white = pgm_read_byte(&gamma8[white]);
        }       
        position++;
        position %= image1d.leds.size();
        everloop.Write(&image1d);
        delay(50);
        xSemaphoreGive( wbSemaphore ); //Free for all
      }
    }
    vTaskDelete(NULL);  
}

/* ************************************************************************ *
 *    LED RING TASK
 * ************************************************************************ */
void everloopTask(void * p){
    while(1){
      xEventGroupWaitBits(everloopGroup,EVERLOOP,true,true,portMAX_DELAY); //Wait for the bit before updating
      Serial.println("Updating everloop");
      //Implementation of Semaphore, otherwise the ESP will crash due to read of the mics
      //Wait a really long time to make sure we get access (5000 ticks)
      if ( xSemaphoreTake( wbSemaphore, ( TickType_t ) 5000 ) == pdTRUE )
      {
        //Yeah got it, see what colors we need
        int r = 0;
        int g = 0;
        int b = 0;
        int w = 0;
        if (isUpdateInProgess) {
          r = update_colors[0];
          g = update_colors[1];
          b = update_colors[2];
          w = update_colors[3];
        } else if (hotword_detected) {
          r = hotword_colors[0];
          g = hotword_colors[1];
          b = hotword_colors[2];
          w = hotword_colors[3];
        } else if (!wifi_connected) {
          r = wifi_disc_colors[0];
          g = wifi_disc_colors[1];
          b = wifi_disc_colors[2];
          w = wifi_disc_colors[3];
        } else {
          r = idle_colors[0];
          g = idle_colors[1];
          b = idle_colors[2];
          w = idle_colors[3];
        }
        r = floor(brightness * r / 100);
        r = pgm_read_byte(&gamma8[r]);
        g = floor(brightness * g / 100);
        g = pgm_read_byte(&gamma8[g]);
        b = floor(brightness * b / 100);
        b = pgm_read_byte(&gamma8[b]);
        w = floor(brightness * w / 100);
        w = pgm_read_byte(&gamma8[w]);
        for (hal::LedValue& led : image1d.leds) {
          led.red = r;
          led.green = g;
          led.blue = b;
          led.white = w;
        }
        everloop.Write(&image1d);
        xSemaphoreGive( wbSemaphore ); //Free for all
        xEventGroupClearBits(everloopGroup,EVERLOOP); //Clear the everloop bit
        Serial.println("Updating done");
      }
      vTaskDelay(1); //Delay a tick, for better stability 
    }
    vTaskDelete(NULL);
}

/* ************************************************************************ *
 *    AUDIO OUTPUT TASK
 * ************************************************************************ */
void AudioPlayTask(void * p){
  while(1) {
    xEventGroupWaitBits(audioGroup,PLAY,true,true,portMAX_DELAY); //Wait for the bit before updating
      if ( xSemaphoreTake( wbSemaphore, ( TickType_t ) 10000 ) == pdTRUE )
      {
        Serial.println("Play Audio");
        float sleep = ((1000000 / samplerate) * (kMaxWriteLength/2)) / 3;
        while(!audioData.isEmpty())
        {
            for (int i = 0; i < kMaxWriteLength; i++) 
            {
              if (!audioData.isEmpty()) {
                audioData.pop(data[i]);
              }
            }
            wb.SpiWrite(hal::kDACBaseAddress,(const uint8_t *)data, sizeof(uint16_t) * kMaxWriteLength/2);
            std::this_thread::sleep_for(std::chrono::microseconds((int)sleep));
        }
        if (asyncClient.connected()) {
          asyncClient.publish(playFinishedTopic.c_str(), 0, false, finishedMsg.c_str());
        }      
        uint8_t b = 0;
        for (int i = 0; i < kMaxWriteLength* 4;i++) {
          audioData.push(b);
        }
        audioData.clear();
        xEventGroupClearBits(audioGroup, PLAY);
        xSemaphoreGive( wbSemaphore ); //Free for all
      }
  }
  vTaskDelete(NULL);
}

/* ************************************************************************ *
 *    SETUP
 * ************************************************************************ */
void setup() {  
  //Implementation of Semaphore, otherwise the ESP will crash due to read of the mics
  if ( wbSemaphore == NULL )  //Not yet been created?
  {
    wbSemaphore = xSemaphoreCreateMutex();  //Create a mutex semaphore
    if ( ( wbSemaphore ) != NULL )
      xSemaphoreGive( ( wbSemaphore ) ); //Free for all 
  }
    
  //Reconnect timers
  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToWifi));
 
  WiFi.onEvent(WiFiEvent);
  asyncClient.setClientId("MatrixVoice");
  asyncClient.onConnect(onMqttConnect);
  asyncClient.onDisconnect(onMqttDisconnect);
  asyncClient.onMessage(onMqttMessage);
  asyncClient.setServer(MQTT_IP, MQTT_PORT);
  audioServer.setServer(MQTT_IP, 1883);  

  everloopGroup = xEventGroupCreate();
  audioGroup = xEventGroupCreate();
 
  strncpy(header.riff_tag,"RIFF",4);
  strncpy(header.wave_tag,"WAVE",4);
  strncpy(header.fmt_tag,"fmt ",4);
  strncpy(header.data_tag,"data",4);

  header.riff_length = (uint32_t)sizeof(header) + (CHUNK * WIDTH);
  header.fmt_length = 16;
  header.audio_format = 1;
  header.num_channels = 1;
  header.sample_rate = RATE;
  header.byte_rate = RATE * WIDTH;
  header.block_align = WIDTH;
  header.bits_per_sample = WIDTH * 8;
  header.data_length = CHUNK * WIDTH;
  
  wb.Init();
  everloop.Setup(&wb);
  
  //setup mics
  mics.Setup(&wb);
  mics.SetSamplingRate(RATE);
  //mics.SetGain(5);

   // Microphone Core Init
  hal::MicrophoneCore mic_core(mics);
  mic_core.Setup(&wb);

  //NumberOfSamples() = kMicarrayBufferSize / kMicrophoneChannels = 4069 / 8 = 512
  //Depending on the CHUNK, we need to calculate how many message we need to send
  message_count = (int)round(mics.NumberOfSamples() / CHUNK);

  xTaskCreatePinnedToCore(everloopTask,"everloopTask",4096,NULL,5,NULL,1);
  xEventGroupSetBits(everloopGroup, EVERLOOP);

  Serial.begin(115200);
  Serial.println("Booting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  //Create the runnings tasks, AudioStream is on 1 core, the rest on the other core
  xTaskCreatePinnedToCore(Audiostream,"Audiostream",10000,NULL,3,&audioTaskHandle,0);
  //xTaskCreatePinnedToCore(everloopAnimation,"everloopAnimation",4096,NULL,5,NULL,1);
  xTaskCreatePinnedToCore(AudioPlayTask,"AudioPlayTask",4096,NULL,3,NULL,1);

  if (!MDNS.begin(host)) { 
    Serial.println("Error setting up MDNS responder!");
    while (1) {
      delay(1000);
    }
  }

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", loginIndex);
  });
  server.on("/serverIndex", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex);
  });
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      isUpdateInProgess = true;
      vTaskSuspend(audioTaskHandle);
      xTimerStop(mqttReconnectTimer, 0);
      xTimerStop(wifiReconnectTimer, 0);
      audioServer.disconnect();
      asyncClient.disconnect();
      xEventGroupSetBits(everloopGroup, EVERLOOP);
      
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  server.begin();
  
  // ---------------------------------------------------------------------------
  // ArduinoOTA stuff
  // ---------------------------------------------------------------------------
  ArduinoOTA.setHostname(host);  
  ArduinoOTA.setPasswordHash(passwordhash);
  ArduinoOTA
    .onStart([]() {
      isUpdateInProgess = true;
      vTaskSuspend(audioTaskHandle);
      xTimerStop(mqttReconnectTimer, 0);
      xTimerStop(wifiReconnectTimer, 0);
      audioServer.disconnect();
      asyncClient.disconnect();
      xEventGroupSetBits(everloopGroup, EVERLOOP);
    })
    .onEnd([]() {
      isUpdateInProgess = false;
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
}

/* ************************************************************************ *
 *    MAIN LOOP
 * ************************************************************************ */
void loop() {
  ArduinoOTA.handle();
  delay(1000);  
  server.handleClient();
  delay(1000);  
  if (!audioServer.connected() && !isUpdateInProgess) {
    connectAudio();
  }
}
