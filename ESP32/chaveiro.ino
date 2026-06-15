#include <Wire.h>
#include <MPU6050_light.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h>

/* ============================================================
   PROJETO: CHAVEIRO / HEALTH PLUS

   Este código controla um chaveiro inteligente com ESP32,
   acelerômetro MPU6050, display OLED, botões e buzzer.

   O sistema conta passos a partir dos movimentos detectados
   pelo acelerômetro e usa essa contagem para evoluir uma flor
   virtual exibida no display.

   Além do funcionamento local, o dispositivo tenta enviar os
   dados de passos e estágio da flor por MQTT. A conexão com
   Wi-Fi e MQTT foi implementada de forma não bloqueante, para
   que o chaveiro continue funcionando mesmo sem internet ou
   sem conexão com a VM/broker.
   ============================================================ */


/* ============================================================
   CONFIGURAÇÃO
   ============================================================ */

// Dados da rede Wi-Fi usada pelo ESP32.
// Caso a rede esteja indisponível, o sistema continua funcionando localmente.
const char* WIFI_SSID      = "";
const char* WIFI_PASSWORD  = "";

// Configurações do broker MQTT responsável por receber os dados do chaveiro.
const char* MQTT_BROKER    = "";
const int   MQTT_PORT      = 1883;
const char* MQTT_TOPIC     = "/TEF/chaveiro001/attrs";
const char* MQTT_CLIENT_ID = "chaveiro001";

// Quantidade de passos necessária para cada estágio da flor.
const int LIMITE_SPROUT = 10;
const int LIMITE_BUD    = 30;
const int LIMITE_BLOOM  = 60;
const int LIMITE_FULL   = 100;

// Tempo mínimo entre dois passos detectados.
// Esse valor evita que um mesmo movimento seja contado várias vezes.
const unsigned long PASSO_DEBOUNCE_MS = 300;

// Valor base da gravidade em "g".
// A biblioteca MPU6050_light retorna a aceleração em unidades de gravidade.
float baseGravidade = 1.0;

// Controle usado para garantir que apenas um passo seja contado por pico.
bool picoArmado = true;

// Limiares usados para detectar e rearmar a contagem de passos.
const float PASSO_SUBIDA  = 0.35;
const float PASSO_DESCIDA = 0.15;

// A cada 5 passos novos, o estado é enviado via MQTT.
const int PASSOS_POR_ENVIO = 5;

// Mesmo sem atingir 5 passos, os dados podem ser enviados após esse tempo.
const unsigned long ENVIO_TIMEOUT_MS = 15000;

// Tempo sem passos necessário para ativar o alerta de inatividade.
const unsigned long INATIVIDADE_MS = 30000;

// Intervalo mínimo entre alertas de inatividade.
const unsigned long REPETE_ALERTA_MS = 30000;

// Intervalos entre tentativas de reconexão.
// O objetivo é evitar travamentos no funcionamento local.
const unsigned long RETRY_WIFI_MS = 5000;
const unsigned long RETRY_MQTT_MS = 5000;

// Pinos dos botões e do buzzer.
const int PIN_BTN_NEXT   = 14;
const int PIN_BTN_SELECT = 27;
const int PIN_BUZZER     = 13;

// Dimensões do display OLED.
const int OLED_LARGURA = 128;
const int OLED_ALTURA  = 64;


/* ============================================================
   OBJETOS GLOBAIS
   ============================================================ */

// Objeto do sensor MPU6050 usando comunicação I2C.
MPU6050 mpu(Wire);

// Objeto do display OLED SSD1306.
Adafruit_SSD1306 oled(OLED_LARGURA, OLED_ALTURA, &Wire, -1);

// Objetos usados para conexão Wi-Fi e comunicação MQTT.
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);


/* ============================================================
   ESTADO DO SISTEMA
   ============================================================ */

// Quantidade total de passos contados desde o início ou último reset.
int passos = 0;

// Estágio atual da flor.
int estagio = 0;

// Guarda quantos passos já foram enviados via MQTT.
int passosUltimoEnvio = 0;

// Variáveis de tempo usadas para controlar envios, passos e alertas.
unsigned long ultimoEnvioMs   = 0;
unsigned long ultimoPassoMs   = 0;
unsigned long ultimoAlertaMs  = 0;
unsigned long ultimoPicoMs    = 0;

// Controle das tentativas de reconexão.
unsigned long ultimaTentativaWiFiMs = 0;
unsigned long ultimaTentativaMQTTMs = 0;

// Indica se o Wi-Fi estava conectado no ciclo anterior.
bool wifiConectado = false;

// Telas disponíveis no display.
enum TelaAtual {
  TELA_FLOR,
  TELA_MENU,
  TELA_STATUS
};

TelaAtual telaAtual = TELA_FLOR;

// Controle do menu.
int menuSelecionado = 0;
const char* opcoesMenu[] = {
  "Voltar",
  "Status",
  "Resetar passos"
};

const int NUM_OPCOES_MENU = 3;

// Tempo até a tela de status voltar automaticamente para a tela principal.
unsigned long telaStatusAteMs = 0;

// Estados anteriores dos botões, usados para detectar apenas o clique.
bool ultimoEstadoNext   = HIGH;
bool ultimoEstadoSelect = HIGH;

// Controle de debounce dos botões.
unsigned long ultimoToqueNextMs   = 0;
unsigned long ultimoToqueSelectMs = 0;

const unsigned long DEBOUNCE_BOTAO_MS = 200;


/* ============================================================
   SETUP
   ============================================================ */

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n=== Tamagotchi Flora ===");

  // Configuração dos botões com pull-up interno.
  // Quando pressionados, os botões passam a ler LOW.
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_BTN_SELECT, INPUT_PULLUP);

  // Configuração do buzzer.
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // Inicialização da comunicação I2C.
  // No ESP32, os pinos usados são SDA = 21 e SCL = 22.
  Wire.begin(21, 22);

  // Inicialização do display OLED.
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Erro: OLED nao encontrado");

    while (true) {
      delay(100);
    }
  }

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(10, 28);
  oled.println("Iniciando...");
  oled.display();

  // Inicialização do acelerômetro MPU6050.
  byte status = mpu.begin();

  if (status != 0) {
    Serial.println("Erro: MPU6050 nao encontrado");

    oled.clearDisplay();
    oled.setCursor(0, 28);
    oled.println("Erro MPU!");
    oled.display();

    while (true) {
      delay(100);
    }
  }

  Serial.println("MPU-6050 ok");
  Serial.println("Calibrando MPU, nao mova o dispositivo...");

  // Calibração inicial do sensor.
  // Durante esse processo, o dispositivo deve ficar parado.
  mpu.calcOffsets();

  Serial.println("Calibracao ok");

  // Configuração do servidor MQTT.
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);

  // Reduz o tempo de espera do MQTT para evitar travamentos longos
  // caso o broker esteja fora do ar.
  mqtt.setSocketTimeout(2);

  // Inicia a tentativa de conexão Wi-Fi sem bloquear o funcionamento.
  iniciarWiFi();

  // Inicialização dos marcadores de tempo.
  ultimoPassoMs  = millis();
  ultimoEnvioMs  = millis();
  ultimoAlertaMs = millis();

  // Exibe a animação inicial.
  mostrarSplash();

  Serial.println("Setup completo - rede em segundo plano");
}


/* ============================================================
   SPLASH SCREEN
   ============================================================ */

void mostrarSplash() {
  // Pequena animação da flor evoluindo.
  for (int est = 0; est <= 4; est++) {
    oled.clearDisplay();
    desenharFlor(64, 32, est);
    oled.display();
    delay(250);
  }

  delay(500);

  // Tela intermediária com o nome do projeto.
  oled.clearDisplay();
  desenharFlor(64, 24, 4);
  oled.setTextSize(2);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(40, 48);
  oled.print("HEALTH");
  oled.display();
  delay(400);

  // Tela final do splash.
  oled.clearDisplay();
  oled.setTextSize(2);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(28, 16);
  oled.print("HEALTH");
  oled.setCursor(40, 36);
  oled.print("PLUS");
  oled.drawLine(20, 58, 108, 58, SSD1306_WHITE);
  oled.display();
  delay(700);

  oled.clearDisplay();
  oled.display();
  delay(150);
}


/* ============================================================
   LOOP PRINCIPAL
   ============================================================ */

void loop() {
  // Cuida das conexões sem interromper as funções principais.
  cuidarConexoes();

  // Mantém o cliente MQTT ativo somente quando conectado.
  if (mqtt.connected()) {
    mqtt.loop();
  }

  // Funções principais do chaveiro.
  detectarPasso();
  lerBotoes();
  verificarEnvioMQTT();
  verificarInatividade();
  desenharTela();

  // Pequeno intervalo para estabilizar o loop.
  delay(20);
}


/* ============================================================
   FLOR
   ============================================================ */

// Define o estágio da flor com base na quantidade de passos.
int calcularEstagio(int p) {
  if (p < LIMITE_SPROUT) return 0;
  if (p < LIMITE_BUD)    return 1;
  if (p < LIMITE_BLOOM)  return 2;
  if (p < LIMITE_FULL)   return 3;

  return 4;
}

// Retorna o nome textual do estágio da flor.
const char* nomeEstagio(int e) {
  switch (e) {
    case 0: return "Semente";
    case 1: return "Broto";
    case 2: return "Folhas";
    case 3: return "Botao";
    case 4: return "Florida!";
    default: return "?";
  }
}


/* ============================================================
   DETECTOR DE PASSO
   ============================================================ */

void detectarPasso() {
  // Atualiza as leituras do MPU6050.
  mpu.update();

  // Calcula o módulo da aceleração somando os três eixos.
  float modulo = sqrt(
    mpu.getAccX() * mpu.getAccX() +
    mpu.getAccY() * mpu.getAccY() +
    mpu.getAccZ() * mpu.getAccZ()
  );

  // Atualiza a referência da gravidade de forma lenta.
  // Isso ajuda a separar a gravidade do movimento real.
  baseGravidade = baseGravidade * 0.95 + modulo * 0.05;

  // Remove a gravidade aproximada, deixando apenas a parte dinâmica.
  float dinamico = modulo - baseGravidade;

  unsigned long agora = millis();

  // Quando o movimento passa do limite de subida,
  // o sistema considera que um passo foi detectado.
  if (picoArmado &&
      dinamico > PASSO_SUBIDA &&
      (agora - ultimoPicoMs) > PASSO_DEBOUNCE_MS) {

    passos++;
    estagio = calcularEstagio(passos);

    ultimoPicoMs  = agora;
    ultimoPassoMs = agora;

    // Desarma o detector para evitar múltiplas contagens no mesmo pico.
    picoArmado = false;

    Serial.printf(
      "Passo %d (estagio %d) | din=%.2f\n",
      passos,
      estagio,
      dinamico
    );
  }

  // Quando o movimento volta a cair, o detector é armado novamente.
  if (dinamico < PASSO_DESCIDA) {
    picoArmado = true;
  }
}


/* ============================================================
   BOTÕES E MENU
   ============================================================ */

void lerBotoes() {
  unsigned long agora = millis();

  // Leitura do botão NEXT.
  bool estadoNext = digitalRead(PIN_BTN_NEXT);

  // Detecta apenas a transição de solto para pressionado.
  if (estadoNext == LOW &&
      ultimoEstadoNext == HIGH &&
      (agora - ultimoToqueNextMs) > DEBOUNCE_BOTAO_MS) {

    ultimoToqueNextMs = agora;
    onBotaoNext();
  }

  ultimoEstadoNext = estadoNext;

  // Leitura do botão SELECT.
  bool estadoSelect = digitalRead(PIN_BTN_SELECT);

  if (estadoSelect == LOW &&
      ultimoEstadoSelect == HIGH &&
      (agora - ultimoToqueSelectMs) > DEBOUNCE_BOTAO_MS) {

    ultimoToqueSelectMs = agora;
    onBotaoSelect();
  }

  ultimoEstadoSelect = estadoSelect;
}

// Ação do botão NEXT.
// Na tela principal, abre o menu.
// Dentro do menu, muda a opção selecionada.
void onBotaoNext() {
  if (telaAtual == TELA_FLOR) {
    telaAtual = TELA_MENU;
    menuSelecionado = 0;
  } else if (telaAtual == TELA_MENU) {
    menuSelecionado = (menuSelecionado + 1) % NUM_OPCOES_MENU;
  }
}

// Ação do botão SELECT.
// Executa a opção escolhida no menu.
void onBotaoSelect() {
  if (telaAtual == TELA_MENU) {
    switch (menuSelecionado) {
      case 0:
        telaAtual = TELA_FLOR;
        break;

      case 1:
        telaAtual = TELA_STATUS;
        telaStatusAteMs = millis() + 3000;
        break;

      case 2:
        passos = 0;
        estagio = 0;
        passosUltimoEnvio = 0;

        // Tenta publicar o reset caso exista conexão.
        // Se estiver offline, apenas mantém o reset local.
        publicarEstado();

        telaAtual = TELA_FLOR;
        break;
    }
  }
}


/* ============================================================
   DESENHO NO OLED
   ============================================================ */

void desenharTela() {
  // A tela de status aparece por 3 segundos e depois volta para a flor.
  if (telaAtual == TELA_STATUS && millis() > telaStatusAteMs) {
    telaAtual = TELA_FLOR;
  }

  oled.clearDisplay();

  if (telaAtual == TELA_FLOR) {
    desenharTelaFlor();
  } else if (telaAtual == TELA_MENU) {
    desenharMenu();
  } else if (telaAtual == TELA_STATUS) {
    desenharStatus();
  }

  oled.display();
}

// Tela principal com passos, estágio e desenho da flor.
void desenharTelaFlor() {
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("Passos: %d/%d", passos, LIMITE_FULL);

  // Indicador simples de ausência de conexão MQTT.
  // A letra "x" mostra que a parte online está indisponível,
  // mas a contagem e a flor continuam funcionando.
  if (!mqtt.connected()) {
    oled.setCursor(122, 0);
    oled.print("x");
  }

  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  desenharFlor(64, 38, estagio);

  oled.setCursor(0, 56);
  oled.print("Estagio: ");
  oled.print(nomeEstagio(estagio));
}

// Desenho da flor em diferentes fases.
// Cada estágio acrescenta novos elementos visuais.
void desenharFlor(int cx, int cy, int est) {
  switch (est) {
    case 0:
      // Semente.
      oled.fillCircle(cx, cy + 8, 3, SSD1306_WHITE);
      oled.drawLine(cx - 10, cy + 12, cx + 10, cy + 12, SSD1306_WHITE);
      break;

    case 1:
      // Broto inicial.
      oled.drawLine(cx, cy + 12, cx, cy + 4, SSD1306_WHITE);
      oled.fillCircle(cx, cy + 2, 2, SSD1306_WHITE);
      oled.drawLine(cx - 10, cy + 12, cx + 10, cy + 12, SSD1306_WHITE);
      break;

    case 2:
      // Planta com folhas.
      oled.drawLine(cx, cy + 12, cx, cy - 4, SSD1306_WHITE);
      oled.fillTriangle(cx, cy + 4, cx - 6, cy + 2, cx, cy + 8, SSD1306_WHITE);
      oled.fillTriangle(cx, cy, cx + 6, cy - 2, cx, cy + 4, SSD1306_WHITE);
      oled.drawLine(cx - 12, cy + 12, cx + 12, cy + 12, SSD1306_WHITE);
      break;

    case 3:
      // Botão da flor antes de abrir.
      oled.drawLine(cx, cy + 12, cx, cy - 8, SSD1306_WHITE);
      oled.fillTriangle(cx, cy + 2, cx - 6, cy, cx, cy + 6, SSD1306_WHITE);
      oled.fillTriangle(cx, cy - 2, cx + 6, cy - 4, cx, cy + 2, SSD1306_WHITE);
      oled.fillCircle(cx, cy - 10, 4, SSD1306_WHITE);
      oled.drawLine(cx - 14, cy + 12, cx + 14, cy + 12, SSD1306_WHITE);
      break;

    case 4:
      // Flor completa.
      oled.drawLine(cx, cy + 12, cx, cy - 6, SSD1306_WHITE);
      oled.fillTriangle(cx, cy + 4, cx - 7, cy + 2, cx, cy + 8, SSD1306_WHITE);
      oled.fillTriangle(cx, cy, cx + 7, cy - 2, cx, cy + 4, SSD1306_WHITE);
      oled.fillCircle(cx - 5, cy - 10, 3, SSD1306_WHITE);
      oled.fillCircle(cx + 5, cy - 10, 3, SSD1306_WHITE);
      oled.fillCircle(cx, cy - 14, 3, SSD1306_WHITE);
      oled.fillCircle(cx, cy - 6, 3, SSD1306_WHITE);
      oled.fillCircle(cx, cy - 10, 2, SSD1306_BLACK);
      oled.drawLine(cx - 14, cy + 12, cx + 14, cy + 12, SSD1306_WHITE);
      break;
  }
}

// Tela de menu com destaque na opção selecionada.
void desenharMenu() {
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("== MENU ==");
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  for (int i = 0; i < NUM_OPCOES_MENU; i++) {
    int y = 14 + i * 11;

    if (i == menuSelecionado) {
      oled.fillRect(0, y - 1, 127, 10, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else {
      oled.setTextColor(SSD1306_WHITE);
    }

    oled.setCursor(2, y);
    oled.print(opcoesMenu[i]);
  }

  oled.setTextColor(SSD1306_WHITE);
}

// Tela de status com informações de funcionamento.
void desenharStatus() {
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("== STATUS ==");
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  oled.setCursor(0, 16);
  oled.printf("Passos: %d", passos);

  oled.setCursor(0, 28);
  oled.printf("Estagio: %s", nomeEstagio(estagio));

  oled.setCursor(0, 40);
  oled.printf("WiFi: %s", WiFi.status() == WL_CONNECTED ? "OK" : "X");

  oled.setCursor(0, 52);
  oled.printf("MQTT: %s", mqtt.connected() ? "OK" : "X");
}


/* ============================================================
   WI-FI E MQTT
   ============================================================ */

// Inicia a conexão Wi-Fi sem esperar a conexão terminar.
void iniciarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  ultimaTentativaWiFiMs = millis();

  Serial.printf(
    "WiFi: iniciando conexao a %s em segundo plano\n",
    WIFI_SSID
  );
}

// Cuida da reconexão Wi-Fi e MQTT sem travar o loop principal.
void cuidarConexoes() {
  unsigned long agora = millis();

  bool wifiOk = (WiFi.status() == WL_CONNECTED);

  // Detecta quando o Wi-Fi acabou de conectar.
  if (wifiOk && !wifiConectado) {
    wifiConectado = true;

    Serial.printf(
      "WiFi conectado! IP: %s\n",
      WiFi.localIP().toString().c_str()
    );

    // Força uma tentativa de MQTT assim que o Wi-Fi conecta.
    ultimaTentativaMQTTMs = 0;

  } else if (!wifiOk && wifiConectado) {
    wifiConectado = false;

    Serial.println("WiFi caiu. Seguindo offline.");
  }

  // Enquanto não houver Wi-Fi, tenta reconectar apenas de tempos em tempos.
  if (!wifiOk) {
    if (agora - ultimaTentativaWiFiMs > RETRY_WIFI_MS) {
      ultimaTentativaWiFiMs = agora;

      Serial.println("WiFi: tentando reconectar...");
      WiFi.reconnect();
    }

    return;
  }

  // O MQTT só é tentado quando o Wi-Fi está conectado.
  if (!mqtt.connected()) {
    if (agora - ultimaTentativaMQTTMs > RETRY_MQTT_MS) {
      ultimaTentativaMQTTMs = agora;

      Serial.printf(
        "MQTT: conectando a %s:%d ... ",
        MQTT_BROKER,
        MQTT_PORT
      );

      if (mqtt.connect(MQTT_CLIENT_ID)) {
        Serial.println("ok");

        // Ao reconectar, envia o estado atual da flor.
        publicarEstado();

      } else {
        Serial.printf("falhou rc=%d\n", mqtt.state());
      }
    }
  }
}

// Verifica se já é hora de enviar os dados por MQTT.
void verificarEnvioMQTT() {
  unsigned long agora = millis();

  int passosNovos = passos - passosUltimoEnvio;

  bool porContagem = passosNovos >= PASSOS_POR_ENVIO;
  bool porTempo = (agora - ultimoEnvioMs) >= ENVIO_TIMEOUT_MS && passosNovos > 0;

  // O envio só acontece se houver conexão MQTT.
  if ((porContagem || porTempo) && mqtt.connected()) {
    publicarEstado();
  }
}

// Publica passos, estágio e timestamp no tópico MQTT.
void publicarEstado() {
  // Se estiver offline, a função termina sem travar o sistema.
  if (!mqtt.connected()) {
    return;
  }

  char timestamp[32];

  unsigned long s = millis() / 1000;

  // Timestamp simplificado usado para testes.
  snprintf(
    timestamp,
    sizeof(timestamp),
    "2026-04-28T00:00:%02luZ",
    s % 60
  );

  char payload[96];

  // Formato esperado pelo agente MQTT/FIWARE:
  // atributo|valor|atributo|valor...
  snprintf(
    payload,
    sizeof(payload),
    "s|%d|fs|%d|ts|%s",
    passos,
    estagio,
    timestamp
  );

  bool ok = mqtt.publish(MQTT_TOPIC, payload);

  Serial.printf(
    "Publicou [%s]: %s -> %s\n",
    MQTT_TOPIC,
    payload,
    ok ? "ok" : "FALHOU"
  );

  passosUltimoEnvio = passos;
  ultimoEnvioMs = millis();
}


/* ============================================================
   ALERTA DE INATIVIDADE
   ============================================================ */

// Verifica se o usuário ficou parado por tempo suficiente
// para acionar o alerta.
void verificarInatividade() {
  if (passos == 0) {
    return;
  }

  unsigned long agora = millis();
  unsigned long inativo = agora - ultimoPassoMs;

  if (inativo > INATIVIDADE_MS &&
      (agora - ultimoAlertaMs) > REPETE_ALERTA_MS) {

    tocarAlerta();
    ultimoAlertaMs = agora;
  }
}

// Emite um alerta sonoro simples pelo buzzer.
void tocarAlerta() {
  Serial.println("Alerta: hora de se mexer!");

  for (int i = 0; i < 4; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(150);

    digitalWrite(PIN_BUZZER, LOW);
    delay(120);
  }
}