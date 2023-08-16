#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "credentials.h"

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//   SETUP DE HARDWARE
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Lista de Chip Select do protocolo SPI, 8 para os DACs e 1 para o ADC
#define CS1 13
#define CS2 12
#define CS3 14
#define CS4 27
#define CS5 26
#define CS6 25
#define CS7 33
#define CS8 32
#define CSA 22 // ADC

int CS_SPI[]{CSA, CS1, CS2, CS3, CS4, CS5, CS6, CS7, CS8};

// Pino de latch. Utilizado para alteração simultanea dos dacs. ativa as saídas quando low
#define LDAC 15

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//   SETUP DE COMUNICAÇÃO
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// buffers
#define BUFFERLEN 35 // tamanho em bytes do buffer que armazena a mensagem recebida

// rede e socket. credenciais do wifi devem ser mantidas no arquivo credentials.h
#define HOSTNAME "controlador_FID"    // wireless
#define PORTA 6969                    // socket
#define PERIODO 1000                  // periodo de reconexao e update em ms
IPAddress local_IP(192, 168, 0, 170); // wireless
IPAddress gateway(192, 168, 0, 1);    // wireless
IPAddress subnet(255, 255, 0, 0);     // wireless
WiFiServer sv(PORTA);                 // socket
WiFiClient cl;                        // socket

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// GERAL
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Configurações e modos
int const coreTask = 0;     // core onde rodarão as tasks nao relacionadas a comunicação (DACs e ADCs)
bool closeAfterRec = false; // o host fecha o socket apos receber a mensagem
bool echo = false;          // a cada comando recebido devolve o comando
bool use_LDAC = true;       // utiliza o LDAC para sincronizar as saidas

char mensagemTcpIn[BUFFERLEN] = "";   // variavel global com a mensagem recebiada via TCP
char mensagemTcpOut[BUFFERLEN] = "0"; // ultima mensagem enviada via TCP
int valorRecebido = 1;                // armazena o valor recebido via TCP em um int

// tasks
TaskHandle_t taskTcp, taskCheckConn, taskRPM;
void taskTcpCode(void *parameter);        // faz a comunicação via socket
void taskCheckConnCode(void *parameters); // checa periodicamente o wifi e verifica se tem atualização

// funcoes
void setupPins();     // inicialização das saidas digitais e do SPI
void setupWireless(); // inicialização do wireless e do update OTA
void setupOTA();      // inicializa o serviço de upload OTA do codigo
void launchTasks();   // dispara as tasks.
void connectWiFi();   // conecta o wifi. é repetida via tasks.
void evaluate();      // identifica o comando, checa se houve mudança na string que armazena a entrada com relação ao estado atual
void changedac();     // atualiza os valores dos dacs
void report();        // devolve o valor do ADC
void erro();          // função de teste

// Variaveis

char estado_DACs[] = "WA000B000C000D000E000F000G000H000\0";
char estado_ADC[] = "RA000B000C000D000E000F000G000H000\0";

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SETUP e LOOP (arduino default)
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void setup()
{
  setupPins();
  setupWireless();
  setupOTA();
  launchTasks();
}

void loop()
{
  vTaskDelete(NULL); // não utiliza o void loop. As tasks lançadas no launchTasks.
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TASKS
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void taskCheckConnCode(void *parameters)
{
  for (;;)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      vTaskDelay(PERIODO / portTICK_PERIOD_MS);
      ArduinoOTA.handle();
      continue;
    }
    connectWiFi();
  }
}

void taskTcpCode(void *parameters)
{
  for (;;)
  {
    if (cl.connected())
    {
      if (cl.available() > 0)
      {
        int i = 0;
        char bufferEntrada[BUFFERLEN] = "";
        while (cl.available() > 0)
        {
          char z = cl.read();
          bufferEntrada[i] = z;
          i++;
          if (z == '\r')
          {
            bufferEntrada[i] = '\0';
            i++;
          }
        }
        strncpy(mensagemTcpIn, bufferEntrada, i);
        if (closeAfterRec)
        {
          cl.stop();
        }
        evaluate();
      }
    }
    else
    {
      cl = sv.available(); // Disponabiliza o servidor para o cliente se conectar.
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Funções
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void setupPins()
{
  pinMode(LDAC, OUTPUT);
  for (int i = 0; i < 9; i++)
  {
    pinMode(CS_SPI[i], OUTPUT);
  }
  digitalWrite(LDAC, HIGH);
  for (int i = 0; i < 9; i++)
  {
    digitalWrite(CS_SPI[i], HIGH);
  }
  SPI.begin(); // inicializa o SPI
}

void setupWireless()
{
  if (!WiFi.config(local_IP, gateway, subnet))
  { // configura o ip estatico
  }
  connectWiFi();
  delay(100);
  sv.begin(); // inicia o server para o socket
}

void setupOTA()
{
  ArduinoOTA.setHostname(HOSTNAME);
  // No authentication by default
  // ArduinoOTA.setPassword("admin");
  ArduinoOTA.onStart([]()
                     {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type); });
  ArduinoOTA.onEnd([]()
                   { Serial.println("\nEnd"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed"); });
  ArduinoOTA.begin();
}

void launchTasks()
{
  delay(2000);
  xTaskCreatePinnedToCore(taskCheckConnCode, "conexao wifi", 5000, NULL, 1, &taskCheckConn, CONFIG_ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(taskTcpCode, "task TCP", 2000, NULL, 1, &taskTcp, CONFIG_ARDUINO_RUNNING_CORE);
}

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
  }
}

void evaluate()
{
  if (strncmp(mensagemTcpIn, "W", 1) == 0)
  {
    if (strncmp(mensagemTcpIn, estado_DACs, BUFFERLEN) != 0)
    {
      strncpy(estado_DACs, mensagemTcpIn, BUFFERLEN);
      changedac();
    }
  }
  else if (strncmp(mensagemTcpIn, "R", 1) == 0)
  {
    report();
  }
  else
  {
    erro();
  }
}

void changedac()
{
  cl.print(estado_DACs);
}

void report()
{
  cl.print(estado_ADC);
}

void erro()
{
  cl.print("comando não reconhecido");
}
// void checkValue()
// {
//   if (strcmp(mensagemTcpIn, "a\r") == 0)
//   {
//     mensagemTcpOut[0] = '2';
//     cl.print(mensagemTcpOut);
//     mode = 'a';
//   }
//   else if (strcmp(mensagemTcpIn, "d\r") == 0)
//   {
//     mensagemTcpOut[0] = '2';
//     cl.print(mensagemTcpOut);
//     mode = 'd';
//   }
//   else if (strcmp(mensagemTcpIn, "s\r") == 0)
//   {
//     mensagemTcpOut[0] = '4';
//     cl.print(mensagemTcpOut);
//     RPMStop();
//   }
//   else if (strcmp(mensagemTcpIn, "r\r") == 0)
//   {
//     mensagemTcpOut[0] = '5';
//     cl.print(mensagemTcpOut);
//     RPMStart();
//   }
//   else if (atoi(mensagemTcpIn) < 9 && atoi(mensagemTcpIn) > 0 && mode == 'd')
//   {
//     valorRecebido = atoi(mensagemTcpIn);
//     mensagemTcpOut[0] = '0';
//     cl.print(mensagemTcpOut);
//     xTaskCreatePinnedToCore(taskRPMCode, "task RPM", 1000, NULL, 1, &taskRPM, coreTask);
//   }
//   else if (atoi(mensagemTcpIn) < 257 && atoi(mensagemTcpIn) > 0 && mode == 'a')
//   {
//     valorRecebido = atoi(mensagemTcpIn);
//     mensagemTcpOut[0] = '0';
//     cl.print(mensagemTcpOut);
//     xTaskCreatePinnedToCore(taskRPMCode, "task RPM", 1000, NULL, 1, &taskRPM, coreTask);
//   }
//   else
//   {
//     mensagemTcpOut[0] = '1';
//     cl.print(mensagemTcpOut);
//   }
// }