#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h> // Utilizado somente em update OTA
#include <ArduinoOTA.h>
#include "credentials.h" // somente armazena SSID e PASS. rede e senha respectivamente.
#include <MCP492X.h>     // biblioteca dos DACs

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
#define CSA 22  // ADC
#define Dummy 5 // pino para usar como chip select na biblioteca do MCP

int CS_SPI[]{CSA, CS1, CS2, CS3, CS4, CS5, CS6, CS7, CS8};

// Pino de latch. Utilizado para alteração simultanea dos dacs. ativa as saídas quando low
#define LDAC 15

// Setup do DAC
MCP492X myDac(Dummy);

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//   SETUP DE COMUNICAÇÃO
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// buffers
#define BUFFERLEN 42 // tamanho em bytes do buffer que armazena a mensagem recebida

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
bool echo = true;           // a cada comando recebido devolve o comando
bool use_LDAC = false;      // utiliza o LDAC para sincronizar as saidas

char mensagemTcpIn[BUFFERLEN] = "";  // variavel global com a mensagem recebiada via TCP
char mensagemTcpOut[BUFFERLEN] = ""; // ultima mensagem enviada via TCP
int valorRecebido = 1;               // armazena o valor recebido via TCP em um int

// tasks
TaskHandle_t taskTcp, taskCheckConn, taskDacs;
void taskTcpCode(void *parameter);        // faz a comunicação via socket
void taskCheckConnCode(void *parameters); // checa periodicamente o wifi e verifica se tem atualização
void taskUpdateDacs(void *parameters);    // task que faz a alteração nos dacs

// funcoes
void setupPins();                     // inicialização das saidas digitais e do SPI
void setupWireless();                 // inicialização do wireless e do update OTA
void setupOTA();                      // inicializa o serviço de upload OTA do codigo
void launchTasks();                   // dispara as tasks.
void connectWiFi();                   // conecta o wifi. é repetida via tasks.
void changeDacs();                    // atualiza os valores dos dacs
void report();                        // devolve o valor do ADC
void stageChanges();                  // verifica se a mensagem é consistente com o protocolo adotado e agenda atualizações nos dacs
void printChanges();                  //
void evaluate();                      // identifica o comando, checa se houve mudança na string que armazena a entrada com relação ao estado atual
void dacUpdate(int canal, int valor); // ajusta os dacs individualmente

char estado_DACs[] = "WA0000B0000C0000D0000E0000F0000G0000H0000"; // valor inicial só para referência e leitura do código
char estado_ADC[] = "0000,0000,0000,0000,0000,0000,0000,0000,,";  // valor inicial só para referência e leitura do código
int estado_Update[3][9] =
    {
        {0, 1, 2, 3, 4, 5, 6, 7, 8},
        {1, 1, 1, 1, 1, 1, 1, 1, 1},
        {0, 0, 0, 0, 0, 0, 0, 0, 0}};
// canal, update status (1 = update), valor

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// SETUP e LOOP (arduino default)
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void setup()
{
  // Serial.begin(9600); //debug
  setupPins();     // Seta os pinos
  myDac.begin();   // inicializa os dacs
  changeDacs();    // Zera os dacs
  setupWireless(); // Seta o WIreless
  setupOTA();      // Inicia os scripts para programar o esp32 via rede
  launchTasks();   // Inicia tudo que roda via task (checagem de coxexão, recebimento de menwsagem, atuação dos DACs e ADC)
}

void loop()
{
  vTaskDelete(NULL); // não utiliza o void loop. As tasks lançadas no launchTasks.
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TASKS
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Checa a conecção, mantem o serviço de upload por wifi. periodo definido em PERIODO
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
    digitalWrite(LED_BUILTIN, LOW);
    connectWiFi();
  }
}

// Recebe a mensagem via TCP, delimita o tamanho maximo de mensagem e elimina o resto. dentro de um socket aberto podem ser feitas varias leituras/envios
// contudo se o socket for fechado pode ser necessário ajustar o vtaskdelay no final da função
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
          if (i < BUFFERLEN - 1)
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
          else
          {
            bufferEntrada[BUFFERLEN] = '\0';
            while (cl.available() > 0)
            {
              char z = cl.read();
            }
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

void taskUpdateDacs(void *parameters)
{
  if (use_LDAC)
  {
    digitalWrite(LDAC, HIGH);
  }
  else
  {
    digitalWrite(LDAC, LOW);
  }
  for (int canal = 1; canal < 9; canal++)
  {
    if (estado_Update[1][canal] == 1)
    {
      dacUpdate(canal, estado_Update[2][canal]);
      estado_Update[1][canal] = 0;
    }
  }
  if (use_LDAC)
  {
    digitalWrite(LDAC, LOW);
  };
  vTaskDelete(NULL);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Funções
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// inicializa os pinos do microcontrolador
void setupPins()
{
  pinMode(LDAC, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  for (int i = 0; i < 9; i++)
  {
    pinMode(CS_SPI[i], OUTPUT);
  }
  if (use_LDAC)
  {
    digitalWrite(LDAC, HIGH);
  }
  else
  {
    digitalWrite(LDAC, LOW);
  }
  for (int i = 0; i < 9; i++)
  {
    digitalWrite(CS_SPI[i], HIGH);
  }
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

// esta função de atualização OTA provavelmente foi obtida e explicada no video do Andreas Spiess.
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

// Inicia as tasks. As tasks de comunicação (Wifi) devem rodar no core que roda o arduino (CONFIG_ARDUINO_RUNNING_CORE)
void launchTasks()
{
  // delay(2000);
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
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
  }
}

// verifica se a mensagem é para atualizar os dacs ou fazer a leitura do adc.
void evaluate()
{
  if (strncmp(mensagemTcpIn, "W", 1) == 0)
  {
    if (strncmp(mensagemTcpIn, estado_DACs, BUFFERLEN) != 0)
    {
      stageChanges();
    }
  }
  else if (strncmp(mensagemTcpIn, "R", 1) == 0)
  {
    report();
  }
  else
  {
    cl.print("\ncomando não reconhecido\nA mensagem deve começar com W para variar a corrente e R para leitura"); //
  }
}

// leitura do ADC ***falta implementar***
void report()
{
  cl.print(estado_ADC);
}

// distribui os valores de entrada na matriz de estado_Update para que posteriormente os dacs sejam ajustados
void stageChanges()
{
  bool erro = false;
  char letras[] = "ABCDEFGH";
  char valorSTR[] = "A0000";
  int valorInt = 0;

  for (int canal = 0; canal <= 7; canal++)
  {
    if (strncmp(mensagemTcpIn + (5 * canal + 1), letras + (canal), 1) != 0)
    {
      erro = true;
      cl.print("\nE2:mensagem fora do padrão. Erro nas letras\nRecebido: ");
      cl.print(mensagemTcpIn);
      cl.print("\nFormato esperado: WA0000B0000C0000D0000E0000F0000G0000H0000\nAs letras devem ser de A a H e nessa ordem. as unicas variáveis são os números ");
      break;
    }
    strncpy(valorSTR, mensagemTcpIn + (5 * canal + 1), 5);

    valorInt = 0;
    for (int digito = 0; digito < 4; digito++)
    {
      char valorSTRBuffer[] = "0";
      strncpy(valorSTRBuffer, valorSTR + 1 + digito, 1);
      if (isalpha(valorSTRBuffer[0]))
      {
        erro = true;
        cl.print("\nE3:mensagem fora do padrão. valores de ajuste dos dacs precisam ser numeros\nRcebido: ");
        cl.print(mensagemTcpIn);
        cl.print("\nErro na parte: ");
        cl.print(valorSTR);
        break;
      }
      valorInt += atoi(valorSTRBuffer) * 1000 / pow(10, digito);
    }
    if (erro)
    {
      break;
    }

    if (valorInt < 0 || valorInt > 4095)
    {
      erro = true;
      cl.print("\nE4:mensagem fora do padrão. valores precisam estar entre 0 e 4095\nRcebido: ");
      cl.print(mensagemTcpIn);
      cl.print("\nErro na parte: ");
      cl.print(valorSTR);
      break;
    }

    if (estado_Update[2][canal + 1] != valorInt)
    {
      estado_Update[2][canal + 1] = valorInt;
      estado_Update[1][canal + 1] = 1;
    }
  }
  if (!erro)
  {
    strncpy(estado_DACs, mensagemTcpIn, BUFFERLEN);
    // printChanges();
    changeDacs();
  }
}

// devolve os valores da matriz de estado dos dacs via tcp
void printChanges()
{
  for (int i = 1; i < 9; i++)
  {
    char itoabuff[] = "0000";
    cl.print("\nCanal: ");
    cl.print(itoa(estado_Update[0][i], itoabuff, 10));
    cl.print("     estado: ");
    cl.print(itoa(estado_Update[1][i], itoabuff, 10));
    cl.print("     Valor: ");
    cl.print(itoa(estado_Update[2][i], itoabuff, 10));
    int valor = estado_Update[2][i];
    Serial.println("canal: ");
    Serial.println(i);
    Serial.println("valor: ");
    Serial.println(valor);
  }
}

// gera a task que atualiza os dacs.
void changeDacs()
{
  xTaskCreatePinnedToCore(taskUpdateDacs, "taskDacs", 1000, NULL, 1, &taskDacs, coreTask);
}

// função que recebe o canal e valor para atualizar um dac individual.
void dacUpdate(int canal, int valor)
{
  digitalWrite(CS_SPI[canal], LOW);
  // delay(100);
  myDac.analogWrite(valor);
  digitalWrite(CS_SPI[canal], HIGH);
}