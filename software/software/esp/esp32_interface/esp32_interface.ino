/*
  ESP32: microfone (INMP441) + alto-falante (MAX98357A) + interface touch (Arduino Uno)
  falando com o PC por WebSocket.

  Biblioteca necessaria: "WebSockets" de Markus Sattler (Links2004)

  Ligacao com o Uno (serial 115200 baud):
    ESP32 GPIO17 (TX2) -> D0 (RX) do Uno
    ESP32 GPIO16 (RX2) <- D1 (TX) do Uno, com divisor de tensao (1k em serie + 2k para o GND)
    GND com GND
*/
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>

// ---- PREENCHA COM OS DADOS DA SUA REDE ----
const char* WIFI_SSID = "EVERTON 7550";
const char* WIFI_PASS = "<L119n69";
const char* SERVER_IP = "192.168.137.1";   // IP do notebook no hotspot
const uint16_t SERVER_PORT = 8765;

// ---- Serial com o Arduino Uno (115200 igual ao Uno e ao diagrama) ----
#define ARDUINO_RX    16      // recebe do TX (D1) do Uno
#define ARDUINO_TX    17      // envia para o RX (D0) do Uno
#define ARDUINO_BAUD  9600

// ---- Microfone INMP441 (pino L/R do mic no GND) ----
#define I2S_MIC_PORT  I2S_NUM_0
#define I2S_MIC_SCK   GPIO_NUM_21   // SCK
#define I2S_MIC_WS    GPIO_NUM_19   // WS
#define I2S_MIC_SD    GPIO_NUM_18   // SD

// ---- Amplificador MAX98357A ----
#define I2S_SPK_PORT  I2S_NUM_1
#define I2S_SPK_BCLK  GPIO_NUM_33
#define I2S_SPK_LRC   GPIO_NUM_25
#define I2S_SPK_DIN   GPIO_NUM_32

#define SAMPLE_RATE   16000
#define CHUNK_SAMPLES 512
#define MIC_SHIFT     16

#define MAX_SEG_GRAVACAO       30
#define TIMEOUT_TRANSCREVER_MS 30000
#define TIMEOUT_FALAR_MS       60000

WebSocketsClient webSocket;
HardwareSerial ArduinoSerial(2);
float volume = 0.6;

uint32_t ttsInicio = 0;
uint32_t bytesRecebidos = 0;

bool gravando = false;
uint32_t gravarAte = 0;
uint32_t bytesEnviados = 0;

// ---------------------------------------------------------------- telas
enum {
  T_INICIO, T_MODO, T_PRONTO, T_GRAVANDO, T_GRAVADO, T_TRANSCREVENDO,
  T_RESULTADO, T_LIBRAS, T_FALANDO, T_ERRO
};

const char* NOMES_TELA[] = {
  "INICIO", "MODO", "PRONTO", "GRAVANDO", "GRAVADO", "TRANSCREVENDO",
  "RESULTADO", "LIBRAS", "FALANDO", "ERRO"
};

uint8_t tela = T_INICIO;
uint8_t origemFala = T_RESULTADO;
uint8_t retryTela = T_MODO;
uint32_t telaDesde = 0;

String textoResultado = "";
String fraseLibras = "";
String mensagemErro = "";

// ---------------------------------------------------------------- fila para o Uno
#define FILA_MAX 10
String fila[FILA_MAX];
uint8_t filaIni = 0, filaFim = 0;
uint32_t filaLiberaEm = 0;
String linhaArduino = "";

void enviarArduino(const String& linha) {
  if (linha.startsWith("PALAVRA:")) {
    for (uint8_t i = filaIni; i != filaFim; i = (i + 1) % FILA_MAX) {
      if (fila[i].startsWith("PALAVRA:")) { fila[i] = linha; return; }
    }
  }
  uint8_t prox = (filaFim + 1) % FILA_MAX;
  if (prox == filaIni) return;
  fila[filaFim] = linha;
  filaFim = prox;
}

void processarFila() {
  if (filaIni == filaFim) return;
  if ((int32_t)(millis() - filaLiberaEm) < 0) return;
  const String& l = fila[filaIni];
  Serial.println("[->UNO] " + l); // Mostra no monitor do ESP32 o que foi enviado ao Uno
  ArduinoSerial.println(l);
  filaLiberaEm = millis() + (l.startsWith("TELA:") ? 300 : 50 + l.length() * 2);
  filaIni = (filaIni + 1) % FILA_MAX;
}

// ---------------------------------------------------------------- PC
void enviarPC(const String& msg) {
  if (webSocket.isConnected()) webSocket.sendTXT(msg.c_str());
}

bool pcOk() {
  return webSocket.isConnected();
}

// ---------------------------------------------------------------- estados
String ultimos(const String& s, unsigned int n) {
  return s.length() > n ? s.substring(s.length() - n) : s;
}

String cortar(const String& s, unsigned int n) {
  return s.length() > n ? s.substring(0, n - 3) + "..." : s;
}

// Nota: No codigo novo do Uno, enviar TEXTO: ou MSG: ja abre a tela correspondente!
void enviarTelaAtual() {
  if (tela == T_RESULTADO) enviarArduino("TEXTO:" + cortar(textoResultado, 110));
  if (tela == T_LIBRAS)    enviarArduino("PALAVRA:" + ultimos(fraseLibras, 70));
  if (tela == T_ERRO)      enviarArduino("MSG:" + cortar(mensagemErro, 46));
  enviarArduino(String("TELA:") + NOMES_TELA[tela]);
}

// avisarUno = true quando o ESP32 forca a mudanca (ex: erro, fim de fala, resultado)
// avisarUno = false quando o proprio Uno ja mudou a tela localmente ao clicar no botao
void irParaTela(uint8_t t, bool avisarUno = true) {
  bool eraLibras = (tela == T_LIBRAS);
  tela = t;
  telaDesde = millis();
  Serial.printf("[TELA] %s (avisarUno=%d)\n", NOMES_TELA[t], avisarUno);
  
  if (avisarUno) {
    enviarTelaAtual();
  }
  
  if ((t == T_LIBRAS) != eraLibras) {
    enviarPC(String("modo:") + (t == T_LIBRAS ? "libras" : "outro"));
  }
}

void mostrarErro(const String& msg, uint8_t retry) {
  mensagemErro = msg;
  retryTela = retry;
  Serial.println("[ERRO] " + msg);
  irParaTela(T_ERRO, true); // Sempre forca o Uno a mostrar a tela de erro
}

// ---------------------------------------------------------------- I2S
void setupMic() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false
  };
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_MIC_SCK;
  pins.ws_io_num = I2S_MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = I2S_MIC_SD;

  i2s_driver_install(I2S_MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pins);
  i2s_stop(I2S_MIC_PORT);
}

void setupSpeaker() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_SPK_BCLK;
  pins.ws_io_num = I2S_SPK_LRC;
  pins.data_out_num = I2S_SPK_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &pins);
}

// ---------------------------------------------------------------- alto-falante
void tocarPcm(const uint8_t* data, size_t len) {
  static int16_t estereo[CHUNK_SAMPLES * 2];
  const int16_t* mono = (const int16_t*)data;
  size_t total = len / 2;
  size_t pos = 0;

  while (pos < total) {
    size_t n = min((size_t)CHUNK_SAMPLES, total - pos);
    for (size_t i = 0; i < n; i++) {
      int32_t v = (int32_t)(mono[pos + i] * volume);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      estereo[2 * i]     = (int16_t)v;
      estereo[2 * i + 1] = (int16_t)v;
    }
    size_t escritos;
    i2s_write(I2S_SPK_PORT, estereo, n * 4, &escritos, portMAX_DELAY);
    pos += n;
  }
}

void silencioFinal() {
  static int16_t zeros[512] = {0};
  size_t escritos;
  for (int k = 0; k < 4; k++) {
    i2s_write(I2S_SPK_PORT, zeros, sizeof(zeros), &escritos, portMAX_DELAY);
  }
}

// ---------------------------------------------------------------- microfone
void iniciarGravacao(int segundos) {
  if (gravando) return;
  segundos = constrain(segundos, 1, 30);
  i2s_zero_dma_buffer(I2S_MIC_PORT);
  i2s_start(I2S_MIC_PORT);
  bytesEnviados = 0;
  gravarAte = millis() + (uint32_t)segundos * 1000;
  gravando = true;
  webSocket.sendTXT(("mic_start:" + String(SAMPLE_RATE)).c_str());
  Serial.printf("[MIC] gravando %d s\n", segundos);
}

void pararGravacao(bool veioDoBotao) {
  if (!gravando) return;
  i2s_stop(I2S_MIC_PORT);
  gravando = false;
  webSocket.sendTXT(("mic_end:" + String(bytesEnviados)).c_str());
  Serial.printf("[MIC] fim, %u bytes enviados\n", bytesEnviados);
  if (tela == T_GRAVANDO) {
    // Se parou por tempo esgotado (nao veio do botao), avisa o Uno para mudar para GRAVADO
    irParaTela(T_GRAVADO, !veioDoBotao);
  }
}

void processarGravacao() {
  if (!gravando) return;

  static int32_t bruto[CHUNK_SAMPLES];
  static int16_t pcm[CHUNK_SAMPLES];
  size_t lidos = 0;

  i2s_read(I2S_MIC_PORT, bruto, sizeof(bruto), &lidos, 100 / portTICK_PERIOD_MS);
  size_t n = lidos / sizeof(int32_t);

  for (size_t i = 0; i < n; i++) {
    int32_t v = bruto[i] >> MIC_SHIFT;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    pcm[i] = (int16_t)v;
  }

  if (n > 0) {
    webSocket.sendBIN((uint8_t*)pcm, n * sizeof(int16_t));
    bytesEnviados += n * sizeof(int16_t);
  }

  if ((int32_t)(millis() - gravarAte) >= 0) pararGravacao(false);
}

// ---------------------------------------------------------------- botoes do Uno
// ---------------------------------------------------------------- botoes do Uno
void tratarBotao(const String& b) {
  Serial.println("[UNO] botao " + b + " (tela era " + NOMES_TELA[tela] + ")");

  if (b == "INICIAR" || b == "VOLTAR") {
    irParaTela(T_MODO, false);
  } 
  else if (b == "MICROFONE" || b == "REGRAVAR") {
    irParaTela(T_PRONTO, false);
  } 
  else if (b == "LIBRAS") {
    irParaTela(T_LIBRAS, false);
  } 
  else if (b == "COMECAR") {
    if (!pcOk()) { mostrarErro("Sem conexao com o PC", T_PRONTO); return; }
    iniciarGravacao(MAX_SEG_GRAVACAO);
    irParaTela(T_GRAVANDO, false);
  } 
  else if (b == "PARAR") {
    pararGravacao(true);
    irParaTela(T_GRAVADO, false);
  } 
  else if (b == "TRANSCREVER") {
    if (!pcOk()) { mostrarErro("Sem conexao com o PC", T_GRAVADO); return; }
    enviarPC("transcrever");
    irParaTela(T_TRANSCREVENDO, false);
  } 
  else if (b == "CANCELAR" || b == "NOVA" || b == "INICIO") {
    if (gravando) pararGravacao(true);
    irParaTela(T_INICIO, false);
  } 
  else if (b == "FALAR") {
    if (!pcOk()) { mostrarErro("Sem conexao com o PC", tela); return; }
    if (tela == T_LIBRAS) {
      origemFala = T_LIBRAS;
      enviarPC("libras:falar");
    } else {
      origemFala = T_RESULTADO;
      enviarPC("falar_ultimo");
    }
  } 
  else if (b == "TENTAR") {
    irParaTela(retryTela, true);
  }
}

void tratarLinhaArduino(const String& l) {
  if (l == "HELLO") {
    enviarTelaAtual();
  } else if (l.startsWith("BTN:")) {
    tratarBotao(l.substring(4));
  }
}

void lerArduino() {
  while (ArduinoSerial.available()) {
    char c = (char)ArduinoSerial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (linhaArduino.length() > 0) tratarLinhaArduino(linhaArduino);
      linhaArduino = "";
    } else if (linhaArduino.length() < 80) {
      linhaArduino += c;
    }
  }
}

void verificarTimeouts() {
  uint32_t dt = millis() - telaDesde;
  if (tela == T_TRANSCREVENDO && dt > TIMEOUT_TRANSCREVER_MS) {
    mostrarErro("A API demorou demais", T_GRAVADO);
  } else if (tela == T_FALANDO && dt > TIMEOUT_FALAR_MS) {
    mostrarErro("A voz demorou demais", origemFala);
  }
}

// ---------------------------------------------------------------- WebSocket
uint8_t retryParaErro() {
  switch (tela) {
    case T_TRANSCREVENDO: return T_GRAVADO;
    case T_FALANDO:       return origemFala;
    default:              return tela;
  }
}

void aoReceberEventoWs(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] conectado ao servidor");
      webSocket.sendTXT(("ready:" + WiFi.localIP().toString()).c_str());
      enviarPC(String("modo:") + (tela == T_LIBRAS ? "libras" : "outro"));
      break;

    case WStype_DISCONNECTED:
      Serial.println("[WS] desconectado");
      if (gravando) {
        i2s_stop(I2S_MIC_PORT);
        gravando = false;
      }
      if (tela == T_GRAVANDO || tela == T_TRANSCREVENDO || tela == T_FALANDO) {
        mostrarErro("Conexao com o PC perdida", retryParaErro());
      }
      break;

    case WStype_TEXT: {
      String msg = (char*)payload;
      Serial.println("[WS] texto: " + msg);

      if (msg.startsWith("tts_start:")) {
        int taxa = msg.substring(10).toInt();
        if (taxa > 0) i2s_set_sample_rates(I2S_SPK_PORT, taxa);
        ttsInicio = millis();
        bytesRecebidos = 0;
      } else if (msg == "tts_end") {
        silencioFinal();
        webSocket.sendTXT(("done:" + String(millis() - ttsInicio) + ":" + String(bytesRecebidos)).c_str());
        if (tela == T_FALANDO) irParaTela(T_MODO, true); // Manda o Uno voltar para T_MODO
      } else if (msg.startsWith("rec:")) {
        iniciarGravacao(msg.substring(4).toInt());
      } else if (msg.startsWith("ping:")) {
        webSocket.sendTXT(("pong:" + msg.substring(5)).c_str());
      } else if (msg.startsWith("vol:")) {
        volume = constrain(msg.substring(4).toFloat(), 0.0f, 4.0f);
        Serial.printf("[WS] volume = %.2f\n", volume);
      } else if (msg.startsWith("texto:")) {
        if (tela == T_TRANSCREVENDO) {
          textoResultado = msg.substring(6);
          irParaTela(T_RESULTADO, true); // Envia TEXTO:... e muda a tela do Uno
        }
      } else if (msg.startsWith("erro:")) {
        if (tela == T_TRANSCREVENDO || tela == T_FALANDO || tela == T_LIBRAS || tela == T_RESULTADO) {
          mostrarErro(msg.substring(5), retryParaErro());
        }
      } else if (msg.startsWith("palavra:") || msg.startsWith("frase:")) {
        int idx = msg.indexOf(':') + 1;
        fraseLibras = msg.substring(idx);
        enviarArduino("PALAVRA:" + ultimos(fraseLibras, 75));
      } else if (msg == "falando") {
        if (tela != T_FALANDO) irParaTela(T_FALANDO, true);
      }
      break;
    }

    case WStype_BIN:
      if (!gravando) {
        bytesRecebidos += length;
        tocarPcm(payload, length);
      }
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  ArduinoSerial.begin(ARDUINO_BAUD, SERIAL_8N1, ARDUINO_RX, ARDUINO_TX);
  delay(500);
  setupMic();
  setupSpeaker();
  irParaTela(T_INICIO, true);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Conectando em %s", WIFI_SSID);
  
  // Aguarda Wi-Fi ate 10 segundos na inicializacao, mas ja escuta o Arduino!
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    lerArduino();
    processarFila();
    delay(50);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi ok, IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWi-Fi ainda conectando em segundo plano...");
  }

  webSocket.begin(SERVER_IP, SERVER_PORT, "/");
  webSocket.onEvent(aoReceberEventoWs);
  webSocket.setReconnectInterval(3000);
}

void loop() {
  webSocket.loop();
  processarGravacao();
  lerArduino();
  processarFila();
  verificarTimeouts();
}