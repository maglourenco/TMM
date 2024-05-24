#include "MLX90640_API.h"
#include "MLX90640_I2C_Driver.h"
#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h> // Hardware-specific library
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define TFT_CS   15 
#define TFT_DC    2
#define TFT_MOSI 13
#define TFT_CLK  14
#define TFT_RST  26
#define TFT_MISO 12
#define TFT_LED  27
#define SERVICE_UUID "ab0828b1-198e-4351-b779-901fa0e0371e"
#define HOTSPOT_CHARACTERISTIC_UUID "4ac8a682-9736-4e5d-932b-e9b31405049c"
#define TA_SHIFT 8 //Default shift for MLX90640 in open air

BLECharacteristic *hotspotCharacteristic; // This BLE Characteristic will receive data if YES or NO hotspot
bool deviceConnected = false; // If the BLE module has an active connection
boolean pressed = false;

TFT_eSPI tft = TFT_eSPI(); //(changed)

const byte MLX90640_address = 0x33; //Default 7-bit unshifted address of the MLX90640
static float mlx90640To[768];
paramsMLX90640 mlx90640;

int xPos, yPos;                                
int R_colour, G_colour, B_colour;              
int i, j;                                      
float T_max, T_min, T_medium;                  
float T_center;
float stDev, meanTemperature;                                

class ServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    }; 

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

void sendDataBLEInterrupt() {
  xTaskCreate(
               sendDataBLE,      /* Task function. */
               "SendDataBLE",    /* String with name of task. */
               5000,             /* Stack size in bytes. */
               NULL,             /* Parameter passed as input of the task */
               2,                /* Priority of the task. */
               NULL);            /* Task handle. */ 
}

// ***************************************
// **************** SETUP ****************
// ***************************************
void setup()
   {
    Serial.begin(115200);
    Wire.begin();
    Wire.setClock(5000000); //speed at 5Mhz (max)

    pinMode(17, INPUT);
    pinMode(0, OUTPUT);
    attachInterrupt(digitalPinToInterrupt(17), sendDataBLEInterrupt, CHANGE);
  
    if (isConnected() == false)
       {
        Serial.println("MLX90640 not detected at default I2C address. Please check wiring. Freezing.");
        while (1);
       }
       
    Serial.println("MLX90640 online!");

    //Get device parameters - We only have to do this once
    int status;
    uint16_t eeMLX90640[832];
    
    status = MLX90640_DumpEE(MLX90640_address, eeMLX90640);
  
    if (status != 0)
       Serial.println("Failed to load system parameters");

    status = MLX90640_ExtractParameters(eeMLX90640, &mlx90640);
  
    if (status != 0)
       {
        Serial.println("Parameter extraction failed");
        Serial.print(" status = ");
        Serial.println(status);
       }

    //Once params are extracted, we can release eeMLX90640 array
    MLX90640_I2CWrite(0x33, 0x800D, 6401);    // writes the value 1901 (HEX) = 6401 (DEC) in the register at position 0x800D to enable reading out the temperatures
    MLX90640_SetRefreshRate(MLX90640_address, 0x05); //setting to 16Hz
       
    pinMode(TFT_LED, OUTPUT);
    digitalWrite(TFT_LED, HIGH);

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(ILI9341_BLACK);
    tft.fillRect(0, 0, 319, 13, TFT_RED);
    tft.setCursor(100, 3);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_YELLOW);
    tft.print("Camara Termica foRESTER");    
    tft.drawLine(250, 210 - 0, 258, 210 - 0, tft.color565(255, 255, 255));
    tft.drawLine(250, 210 - 90, 258, 210 - 90, tft.color565(255, 255, 255));
    tft.drawLine(250, 210 - 180, 258, 210 - 180, tft.color565(255, 255, 255));
    tft.setCursor(80, 220);
    tft.setTextColor(ILI9341_WHITE, tft.color565(0, 0, 0));
    tft.print("T(+) = ");    

    // drawing the colour-scale
    // ========================

    for (i = 0; i < 181; i++)
       {     
        getColour(i);
        tft.drawLine(240, 210 - i, 250, 210 - i, tft.color565(R_colour, G_colour, B_colour));
       } 


  ///////BLE SETUP//////
  BLEDevice::init("Thermal Mapping"); // nome do dispositivo bluetooth
  // Create the BLE Server
  BLEServer *server = BLEDevice::createServer(); //cria um BLE server
  server->setCallbacks(new ServerCallbacks()); //seta o callback do server
  // Create the BLE Service
  BLEService *service = server->createService(SERVICE_UUID);
  // Create a BLE Characteristic to show the detection (or not) of hotspot(s)
  hotspotCharacteristic = service->createCharacteristic(HOTSPOT_CHARACTERISTIC_UUID,BLECharacteristic::PROPERTY_NOTIFY);
  hotspotCharacteristic->addDescriptor(new BLE2902());

  service->start();
  // Start advertising (descoberta do ESP32)
  server->getAdvertising()->start();
  } 
  
// **********************************
// ************** LOOP **************
// **********************************
void loop()
   { 
    for (byte x = 0 ; x < 2 ; x++) //Read both subpages
       {
        uint16_t mlx90640Frame[834];
        int status = MLX90640_GetFrameData(MLX90640_address, mlx90640Frame);
    
        if (status < 0)
           {
            Serial.print("GetFrame Error: ");
            Serial.println(status);
           }

        float vdd = MLX90640_GetVdd(mlx90640Frame, &mlx90640);
        float Ta = MLX90640_GetTa(mlx90640Frame, &mlx90640);

        float tr = Ta - TA_SHIFT; //Reflected temperature based on the sensor ambient temperature
        float emissivity = 0.95;

        MLX90640_CalculateTo(mlx90640Frame, &mlx90640, emissivity, tr, mlx90640To);
       }

       
    // determine T_min and T_max and eliminate error pixels
    // ====================================================
    mlx90640To[1*32 + 21] = 0.5 * (mlx90640To[1*32 + 20] + mlx90640To[1*32 + 22]);    // eliminate the error-pixels
    mlx90640To[4*32 + 30] = 0.5 * (mlx90640To[4*32 + 29] + mlx90640To[4*32 + 31]);    // eliminate the error-pixels
    
    T_min = mlx90640To[0];
    T_max = mlx90640To[0];

    for (i = 1; i < 768; i++)
       {
        if((mlx90640To[i] > -41) && (mlx90640To[i] < 301))
           {
            if(mlx90640To[i] < T_min)
               {
                T_min = mlx90640To[i];
               }

            if(mlx90640To[i] > T_max)
               {
                T_max = mlx90640To[i];
               }
           }
        else if(i > 0)   // temperature out of range
           {
            mlx90640To[i] = mlx90640To[i-1];
           }
        else
           {
            mlx90640To[i] = mlx90640To[i+1];
           }
       }

    // determine T_center
    // ==================
    T_center = mlx90640To[11* 32 + 15];    
    T_medium = ((T_max + T_min)/2);

    float temperatureSum = 0.0;
    
    // STEP 1a, FIND THE MEAN: SUM ALL TEMPERATURES
    for (i = 1; i < 768; i++)
    {
      temperatureSum += mlx90640To[i];
    }

    // STEP 1b, FIND THE MEAN: DIVIDE BY THE # OF TEMPERATURE VALUES
    meanTemperature = temperatureSum / float(768);
    float sqDevSum = 0.0;

    // STEP 2, sum the squares of the differences from the mean
    for (i = 1; i < 768; i++)
    {
      sqDevSum += pow((meanTemperature - float(mlx90640To[i])), 2);
    }

    // STEP 3, TAKE THE SQUARE ROOT OF THAT TO CALCULATE THE STANDARD DEVIATION
    stDev = sqrt(sqDevSum/float(768));
  
    // drawing the picture
    // ===================
    for (i = 0 ; i < 24 ; i++)
    {
      for (j = 0; j < 32; j++)
      {
        mlx90640To[i*32 + j] = 180.0 * (mlx90640To[i*32 + j] - T_min) / (T_max - T_min);          
        getColour(mlx90640To[i*32 + j]);
        tft.fillRect(217 - j * 7, 35 + i * 7, 7, 7, tft.color565(R_colour, G_colour, B_colour));
      }
    }

    tft.drawLine(217 - 15*7 + 3.5 - 5, 11*7 + 35 + 3.5, 217 - 15*7 + 3.5 + 5, 11*7 + 35 + 3.5, tft.color565(255, 255, 255));
    tft.drawLine(217 - 15*7 + 3.5, 11*7 + 35 + 3.5 - 5, 217 - 15*7 + 3.5, 11*7 + 35 + 3.5 + 5,  tft.color565(255, 255, 255));
    tft.fillRect(260, 25, 37, 10, tft.color565(0, 0, 0));
    tft.fillRect(260, 205, 37, 10, tft.color565(0, 0, 0));    
    tft.fillRect(115, 220, 37, 10, tft.color565(0, 0, 0));    
    tft.setTextColor(ILI9341_WHITE, tft.color565(0, 0, 0));
    tft.setCursor(265, 25);
    tft.print(T_max, 1);
    tft.setCursor(265, 205);
    tft.print(T_min, 1);
    tft.setCursor(120, 220);
    tft.print(T_center, 1);
    tft.setCursor(265, 115);
    tft.print(T_medium, 1);
    tft.setCursor(300, 25);
    tft.print("C");
    tft.setCursor(300, 205);
    tft.print("C");
    tft.setCursor(300, 115);
    tft.print("C");
    tft.setCursor(155, 220);
    tft.print("C");

    delay(10);
}

// ===============================
// ===== determine the colour ====
// ===============================
void getColour(int j)
   {
    if (j >= 0 && j < 30)
       {
        R_colour = 0;
        G_colour = 0;
        B_colour = 20 + (120.0/30.0) * j;
       }
    
    if (j >= 30 && j < 60)
       {
        R_colour = (120.0 / 30) * (j - 30.0);
        G_colour = 0;
        B_colour = 140 - (60.0/30.0) * (j - 30.0);
       }

    if (j >= 60 && j < 90)
       {
        R_colour = 120 + (135.0/30.0) * (j - 60.0);
        G_colour = 0;
        B_colour = 80 - (70.0/30.0) * (j - 60.0);
       }

    if (j >= 90 && j < 120)
       {
        R_colour = 255;
        G_colour = 0 + (60.0/30.0) * (j - 90.0);
        B_colour = 10 - (10.0/30.0) * (j - 90.0);
       }

    if (j >= 120 && j < 150)
       {
        R_colour = 255;
        G_colour = 60 + (175.0/30.0) * (j - 120.0);
        B_colour = 0;
       }

    if (j >= 150 && j <= 180)
       {
        R_colour = 255;
        G_colour = 235 + (20.0/30.0) * (j - 150.0);
        B_colour = 0 + 255.0/30.0 * (j - 150.0);
       }

   }
   
// Returns true if the MLX90640 is detected on the I2C bus
boolean isConnected()
   {
    Wire.beginTransmission((uint8_t)MLX90640_address);
    Serial.print("looking for i2c");
    if (Wire.endTransmission() != 0)
       return (false); //Sensor did not ACK
    
    return (true);
   }   

// Sends hotspot data to client through Bluetooth BLE
void sendDataBLE(void * pvParameters ){  
  // Debounce the button by reading its state multiple times
  int buttonState = digitalRead(17);
  // Handle voltage bouncing
  delay(10);
  if (buttonState == digitalRead(17)) {
    // The button state hasn't changed, so it's a valid press
    // Logic to fire once and not while the button is pressed
    if (!pressed && buttonState == HIGH) {
      pressed = true;
      String isHotspot = "-1";
      int str_len = isHotspot.length() + 3;      
      char char_array[str_len];
      
      // To detect an hotspot - check the T_max and the standard deviation, if both have abnormal values - hotspot detected
      if (T_max > 40.0 && stDev > 3.0) {
        Serial.println("HOTSPOT DETECTED!");
        isHotspot = "1";
      } else {
        Serial.println("NO HOTSPOT DETECTED!");
        isHotspot = "0";
      }
      
      // Send the data through Bluetooth BLE 
      isHotspot.toCharArray(char_array, str_len);
      hotspotCharacteristic->setValue(char_array); //seta o valor que a caracteristica notificará (enviar)
      hotspotCharacteristic->notify(); // Envia o valor para o smartphone 

      // Blink LED to signal BLE data send
      digitalWrite(0, HIGH);   // turn the LED on
      delay(200);
      digitalWrite(0, LOW);    // turn the LED off
    } else if (pressed){
      digitalWrite(0, LOW);    // turn the LED off
      pressed = false;
    }
  } 

  vTaskDelete(NULL);
}
