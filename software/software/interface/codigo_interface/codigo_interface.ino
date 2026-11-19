/*
  Arduino Uno + shield LCD 3.5" touch (ILI9486, 8 bits) -- interface "Libras e Fala"
  Seguindo o diagrama de telas e baud rate 115200.
*/
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>

// ------------------------------------------------------------------ 1. VELOCIDADE SERIAL (MUDADO PARA 115200 CONFORME DIAGRAMA)
#define BAUD_ESP32        9600
#define ROTACAO           1      // 1 = Paisagem (480x320)
#define INVERTER_CORES    false
#define ID_PADRAO_TELA    0x9486

// ------------------------------------------------------------------ 2. AJUSTE DEFINITIVO DO TOUCH
// Se ao tocar na tela NAO aparecer "RAW X:... Y:..." no topo ou um eixo ficar travado,
// mude TIPO_PINOS_TOUCH de 1 para 2!
#define TIPO_PINOS_TOUCH  1      // 1 = (XP=8, XM=A2, YP=A3, YM=9) | 2 = (XP=9, XM=A3, YP=A2, YM=8)

#if TIPO_PINOS_TOUCH == 1
  const int XP = 8, XM = A2, YP = A3, YM = 9;
#else
  const int XP = 9, XM = A3, YP = A2, YM = 8;
#endif

// Mude estas 3 chaves observando para onde a bolinha vermelha vai quando voce arrasta o dedo:
#define MODO_CALIBRACAO   false   // Mostra valores no topo e bolinha vermelha (mude para false quando ficar bom)
#define TOUCH_TROCAR_XY   true   // Se mover na horizontal e a bolinha for na vertical, mude para false
#define TOUCH_INVERTER_X  true   // Se tocar na esquerda e a bolinha sair na direita, mude (true <-> false)
#define TOUCH_INVERTER_Y  true  // Se tocar em cima e a bolinha sair embaixo, mude (true <-> false)

// Limites padrao do sensor resistivo (voce pode afinar olhando os numeros RAW X e RAW Y no topo da tela)
const int TS_MINX = 150, TS_MAXX = 900;
const int TS_MINY = 150, TS_MAXY = 920;

#define MINPRESSURE 150
#define MAXPRESSURE 1000

// ------------------------------------------------------------------ CORES (RGB565)
#define PRETO     0x0000
#define BRANCO    0xFFFF
#define CINZA     0x8410
#define CINZA_BTN 0x4208
#define AZUL      0x24BE
#define VERDE     0x4D6A
#define VERMELHO  0xF206
#define LARANJA   0xFD20

// ------------------------------------------------------------------ TAMANHOS
#define LINHA_MAX    130
#define TEXTO_MAX    120
#define PALAVRA_MAX   80
#define MSG_MAX       48
#define MAX_BOTOES     5

enum { T_INICIO, T_MODO, T_PRONTO, T_GRAVANDO, T_GRAVADO, T_TRANSCREVENDO,
       T_RESULTADO, T_LIBRAS, T_FALANDO, T_ERRO };

struct Botao {
  int16_t x, y, w, h;
  const __FlashStringHelper* nome;
  const __FlashStringHelper* rot;
  uint16_t cor;
};

MCUFRIEND_kbv tft;
TouchScreen ts(XP, YP, XM, YM, 300);

int16_t W, H;
const int16_t M = 12, BH = 50, GAP = 8;

char linha[LINHA_MAX];
uint8_t nLinha = 0;
bool descartando = false;

char bufTexto[TEXTO_MAX] = "";
char bufPalavra[PALAVRA_MAX] = "";
char bufMsg[MSG_MAX] = "Wi-Fi, PC ou API";

Botao botoes[MAX_BOTOES];
uint8_t nBotoes = 0;

uint8_t tela = T_INICIO;
uint8_t geracao = 0;
uint32_t telaDesde = 0, ultimaAnim = 0, bloqueioToque = 0;
bool piscar = false;

// ------------------------------------------------------------------ prototipos
void mostraTela(uint8_t t);
void desenhaBotao(uint8_t i, bool pressionado);
void lerSerial();

// ------------------------------------------------------------------ desenho
int16_t yFila(uint8_t k) { return H - M - BH - k * (BH + GAP); }

void titulo(const __FlashStringHelper* t, uint16_t cor) {
  tft.fillRect(0, 0, W, 44, cor);
  tft.setTextSize(2);
  tft.setTextColor(BRANCO);
  int16_t w = strlen_P((PGM_P)t) * 12;
  tft.setCursor((W - w) / 2, 14);
  tft.print(t);
}

uint8_t escreveQuebrado(const char* s, int16_t x, int16_t y, int16_t w,
                        uint8_t tam, uint16_t cor, uint8_t maxLinhas) {
  tft.setTextSize(tam);
  tft.setTextColor(cor);
  uint8_t cpl = w / (6 * tam);
  if (cpl < 1) cpl = 1;
  uint8_t linhas = 0;
  uint16_t n = strlen(s), i = 0;
  while (i < n && linhas < maxLinhas) {
    while (i < n && s[i] == ' ') i++;
    if (i >= n) break;
    uint16_t fim = i + cpl;
    if (fim >= n) {
      fim = n;
    } else {
      uint16_t k = fim;
      while (k > i && s[k] != ' ') k--;
      if (k > i) fim = k;
    }
    tft.setCursor(x, y + linhas * (8 * tam + 4));
    for (uint16_t j = i; j < fim; j++) tft.write(s[j]);
    i = fim;
    linhas++;
  }
  return linhas;
}

uint8_t info(const __FlashStringHelper* f, int16_t y, uint16_t cor) {
  char tmp[72];
  strncpy_P(tmp, (PGM_P)f, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  return escreveQuebrado(tmp, M, y, W - 2 * M, 2, cor, 6);
}

void desenhaBotao(uint8_t i, bool pressionado) {
  Botao &b = botoes[i];
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 8, pressionado ? BRANCO : b.cor);
  tft.setTextSize(2);
  tft.setTextColor(pressionado ? b.cor : BRANCO);
  int16_t n = strlen_P((PGM_P)b.rot);
  tft.setCursor(b.x + (b.w - n * 12) / 2, b.y + (b.h - 16) / 2);
  tft.print(b.rot);
}

void botao(const __FlashStringHelper* nome, const __FlashStringHelper* rot,
           int16_t x, int16_t y, int16_t w, int16_t h, uint16_t cor) {
  if (nBotoes >= MAX_BOTOES) return;
  Botao &b = botoes[nBotoes];
  b.x = x; b.y = y; b.w = w; b.h = h;
  b.nome = nome; b.rot = rot; b.cor = cor;
  desenhaBotao(nBotoes, false);
  nBotoes++;
}

void desenhaTextoArea(const char* s, int16_t topo, int16_t base, uint16_t cor) {
  tft.fillRect(0, topo, W, base - topo, PRETO);
  uint8_t maxL = (base - topo - 4) / 20;
  escreveQuebrado(s, M, topo + 4, W - 2 * M, 2, cor, maxL);
}

void desenhaResultado() { 
  if (bufTexto[0] == 0) desenhaTextoArea("(texto transcrito aparece aqui)", 60, yFila(1) - GAP, CINZA);
  else desenhaTextoArea(bufTexto, 60, yFila(1) - GAP, BRANCO); 
}

int16_t topoFrase = 100;

void desenhaFrase() {
  int16_t topo = topoFrase + 22, base = yFila(1) - GAP;
  tft.fillRect(0, topo, W, base - topo, PRETO);
  uint8_t maxL = (base - topo - 4) / 20;
  if (bufPalavra[0] == 0) escreveQuebrado("____", M, topo + 4, W - 2 * M, 2, CINZA, 1);
  else escreveQuebrado(bufPalavra, M, topo + 4, W - 2 * M, 2, BRANCO, maxL);
}

void desenhaMsg() { desenhaTextoArea(bufMsg, 70, yFila(1) - GAP, BRANCO); }

void desenhaGravando(bool ponto) {
  tft.fillCircle(W / 2, 120, 34, ponto ? VERMELHO : PRETO);
  tft.fillRect(M, 170, W - 2 * M, 30, PRETO);
  char b[8];
  utoa((millis() - telaDesde) / 1000, b, 10);
  tft.setTextSize(3);
  tft.setTextColor(BRANCO);
  int16_t w = (strlen(b) + 2) * 18;
  tft.setCursor((W - w) / 2, 174);
  tft.print(b);
  tft.print(F(" s"));
}

void desenhaPontos() {
  uint8_t n = 1 + (millis() / 400) % 3;
  tft.fillRect(M, 130, W - 2 * M, 30, PRETO);
  tft.setTextSize(3);
  tft.setTextColor(BRANCO);
  tft.setCursor((W - n * 18) / 2, 132);
  for (uint8_t i = 0; i < n; i++) tft.write('.');
}

// Telas desenhadas fielmente conforme o diagrama_telas.drawio
void mostraTela(uint8_t t) {
  tela = t;
  telaDesde = millis();
  ultimaAnim = 0;
  piscar = true;
  geracao++;
  nBotoes = 0;
  tft.fillScreen(PRETO);
  const int16_t L = W - 2 * M;

  switch (t) {
    case T_INICIO:
      titulo(F("Libras e Fala"), AZUL);
      info(F("Se comunique com Libras e fala!"), 90, BRANCO);
      botao(F("INICIAR"), F("Iniciar Conversa"), M, yFila(0), L, BH, AZUL);
      break;

    case T_MODO:
      titulo(F("Modo de comunicacao"), AZUL);
      info(F("Escolha um modo de comunicacao"), 70, BRANCO);
      botao(F("MICROFONE"), F("Microfone"), M, yFila(1), L, BH, VERDE);
      botao(F("LIBRAS"), F("Libras"), M, yFila(0), L, BH, LARANJA);
      break;

    case T_PRONTO:
      titulo(F("Pronto para ouvir"), VERDE);
      info(F("Toque em Comecar para falar."), 90, BRANCO);
      botao(F("COMECAR"), F("Comecar"), M, yFila(0), L, BH, VERDE);
      break;

    case T_GRAVANDO:
      titulo(F("Gravando audio..."), VERMELHO);
      desenhaGravando(true);
      botao(F("PARAR"), F("Parar"), M, yFila(0), L, BH, VERMELHO);
      break;

    case T_GRAVADO:
      titulo(F("Audio gravado"), AZUL);
      info(F("O que deseja fazer com o audio?"), 70, BRANCO);
      botao(F("TRANSCREVER"), F("Transcrever"), M, yFila(1), L, BH, VERDE);
      botao(F("REGRAVAR"), F("Gravar de novo"), M, yFila(0), L, BH, CINZA_BTN);
      break;

    case T_TRANSCREVENDO:
      titulo(F("Transcrevendo..."), AZUL);
      info(F("aguardando resposta da API"), 70, BRANCO);
      desenhaPontos();
      botao(F("CANCELAR"), F("Cancelar"), M, yFila(0), L, BH, CINZA_BTN);
      break;

    case T_RESULTADO:
      titulo(F("Resultado:"), VERDE);
      desenhaResultado();
      botao(F("FALAR"), F("Falar em voz alta"), M, yFila(1), L, BH, AZUL);
      botao(F("NOVA"), F("Nova conversa"), M, yFila(0), L, BH, CINZA_BTN);
      break;

    case T_LIBRAS: {
      titulo(F("Libras"), LARANJA);
      uint8_t n = info(F("Libras: mostre as letras para a camera do notebook"), 52, CINZA);
      topoFrase = 52 + n * 20 + 2;
      tft.setTextSize(2);
      tft.setTextColor(BRANCO);
      tft.setCursor(M, topoFrase);
      tft.print(F("Palavra:"));
      desenhaFrase();
      botao(F("FALAR"), F("Falar"), M, yFila(1), L, BH, VERDE);
      botao(F("VOLTAR"), F("Voltar"), M, yFila(0), L, BH, CINZA_BTN);
      break;
    }

    case T_FALANDO:
      titulo(F("Falando..."), LARANJA);
      info(F("Reproduzindo o audio no alto-falante"), 70, BRANCO);
      desenhaPontos();
      break;

    case T_ERRO:
      titulo(F("Algo deu errado"), VERMELHO);
      desenhaMsg();
      botao(F("TENTAR"), F("Tentar de novo"), M, yFila(1), L, BH, AZUL);
      botao(F("INICIO"), F("Inicio"), M, yFila(0), L, BH, CINZA_BTN);
      break;
  }
  bloqueioToque = millis() + 250;
}

// ------------------------------------------------------------------ serial
uint8_t indiceTela(const char* n) {
  if (!strcmp_P(n, PSTR("INICIO")))        return T_INICIO;
  if (!strcmp_P(n, PSTR("MODO")))          return T_MODO;
  if (!strcmp_P(n, PSTR("PRONTO")))        return T_PRONTO;
  if (!strcmp_P(n, PSTR("GRAVANDO")))      return T_GRAVANDO;
  if (!strcmp_P(n, PSTR("GRAVADO")))       return T_GRAVADO;
  if (!strcmp_P(n, PSTR("TRANSCREVENDO"))) return T_TRANSCREVENDO;
  if (!strcmp_P(n, PSTR("RESULTADO")))     return T_RESULTADO;
  if (!strcmp_P(n, PSTR("LIBRAS")))        return T_LIBRAS;
  if (!strcmp_P(n, PSTR("FALANDO")))       return T_FALANDO;
  if (!strcmp_P(n, PSTR("ERRO")))          return T_ERRO;
  return 255;
}

void copia(char* dst, uint8_t tam, const char* src) {
  strncpy(dst, src, tam - 1);
  dst[tam - 1] = 0;
}

void processaLinha() {
  if (!strncmp_P(linha, PSTR("TELA:"), 5)) {
    uint8_t t = indiceTela(linha + 5);
    if (t != 255) mostraTela(t);
  } else if (!strncmp_P(linha, PSTR("TEXTO:"), 6)) {
    copia(bufTexto, TEXTO_MAX, linha + 6);
    if (tela == T_RESULTADO) desenhaResultado();
  } else if (!strncmp_P(linha, PSTR("PALAVRA:"), 8)) {
    copia(bufPalavra, PALAVRA_MAX, linha + 8);
    if (tela == T_LIBRAS) desenhaFrase();
  } else if (!strncmp_P(linha, PSTR("FRASE:"), 6)) {
    copia(bufPalavra, PALAVRA_MAX, linha + 6);
    if (tela == T_LIBRAS) desenhaFrase();
  } else if (!strncmp_P(linha, PSTR("MSG:"), 4)) {
    copia(bufMsg, MSG_MAX, linha + 4);
    if (tela == T_ERRO) desenhaMsg();
  }
}

void lerSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!descartando && nLinha > 0) {
        linha[nLinha] = 0;
        processaLinha();
      }
      nLinha = 0;
      descartando = false;
    } else if (!descartando) {
      if (nLinha < LINHA_MAX - 1) linha[nLinha++] = c;
      else descartando = true;
    }
  }
}

// ------------------------------------------------------------------ toque
bool lerToque(int16_t &x, int16_t &y) {
  TSPoint p = ts.getPoint();
  pinMode(XM, OUTPUT);
  pinMode(YP, OUTPUT);
  pinMode(XP, OUTPUT);
  pinMode(YM, OUTPUT);
  
  if (p.z < MINPRESSURE || p.z > MAXPRESSURE) return false;

  int16_t rawX = TOUCH_TROCAR_XY ? p.y : p.x;
  int16_t rawY = TOUCH_TROCAR_XY ? p.x : p.y;

  if (TOUCH_INVERTER_X) {
    x = map(rawX, TS_MAXX, TS_MINX, 0, W);
  } else {
    x = map(rawX, TS_MINX, TS_MAXX, 0, W);
  }

  if (TOUCH_INVERTER_Y) {
    y = map(rawY, TS_MAXY, TS_MINY, 0, H);
  } else {
    y = map(rawY, TS_MINY, TS_MAXY, 0, H);
  }

  x = constrain(x, 0, W - 1);
  y = constrain(y, 0, H - 1);

#if MODO_CALIBRACAO
  // Mostra no topo os valores crus e mapeados para voce ver o que esta acontecendo
  tft.fillRect(0, 0, 260, 14, PRETO);
  tft.setTextSize(1);
  tft.setTextColor(VERDE);
  tft.setCursor(2, 3);
  tft.print(F("RAW:")); tft.print(p.x); tft.print(F(",")); tft.print(p.y);
  tft.print(F(" -> XY:")); tft.print(x); tft.print(F(",")); tft.print(y);
  tft.fillCircle(x, y, 4, VERMELHO);
#endif

  return true;
}

// Transicoes locais fieis ao diagrama_telas.drawio
void transicaoLocal(const __FlashStringHelper* nome) {
  PGM_P n = (PGM_P)nome;
  if      (!strcmp_P("INICIAR", n))     mostraTela(T_MODO);
  else if (!strcmp_P("MICROFONE", n))   mostraTela(T_PRONTO);
  else if (!strcmp_P("LIBRAS", n))      mostraTela(T_LIBRAS);
  else if (!strcmp_P("VOLTAR", n))      mostraTela(T_MODO);
  else if (!strcmp_P("COMECAR", n))     mostraTela(T_GRAVANDO);
  else if (!strcmp_P("PARAR", n))       mostraTela(T_GRAVADO);
  else if (!strcmp_P("REGRAVAR", n))    mostraTela(T_PRONTO);
  else if (!strcmp_P("TRANSCREVER", n)) mostraTela(T_TRANSCREVENDO);
  else if (!strcmp_P("CANCELAR", n))    mostraTela(T_INICIO);
  else if (!strcmp_P("NOVA", n))        mostraTela(T_INICIO);
  else if (!strcmp_P("INICIO", n))      mostraTela(T_INICIO);
  else if (!strcmp_P("TENTAR", n))      mostraTela(T_MODO);
  //else if (!strcmp_P("FALAR", n))       mostraTela(T_FALANDO);
}

void aoTocar(int16_t x, int16_t y) {
  for (uint8_t i = 0; i < nBotoes; i++) {
    Botao &b = botoes[i];
    if (x >= b.x - 6 && x <= b.x + b.w + 6 && y >= b.y - 6 && y <= b.y + b.h + 6) {
      uint8_t g = geracao;
      desenhaBotao(i, true);
      
      // Envia para o ESP32
      Serial.print(F("BTN:"));
      Serial.println(b.nome);
      delay(60);
      
      // Muda a tela imediatamente conforme o diagrama
      transicaoLocal(b.nome);
      
      // Le a serial caso o ESP32 mande algo em seguida
      lerSerial();
      if (g == geracao) desenhaBotao(i, false);
      return;
    }
  }
}

void tratarToque() {
  static bool tocando = false;
  static uint32_t visto = 0;
  int16_t x, y;
  if (lerToque(x, y)) {
    visto = millis();
    if (!tocando) {
      tocando = true;
      if ((int32_t)(millis() - bloqueioToque) >= 0) aoTocar(x, y);
    }
  } else if (tocando && millis() - visto > 60) {
    tocando = false;
  }
}

// ------------------------------------------------------------------ animacoes
void animacoes() {
  uint32_t agora = millis();
  if (tela == T_GRAVANDO) {
    if (agora - ultimaAnim >= 500) {
      ultimaAnim = agora;
      piscar = !piscar;
      desenhaGravando(piscar);
    }
  } else if (tela == T_TRANSCREVENDO || tela == T_FALANDO) {
    if (agora - ultimaAnim >= 400) {
      ultimaAnim = agora;
      desenhaPontos();
    }
  }
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(BAUD_ESP32);
  tft.reset();
  uint16_t id = tft.readID();
  if (id == 0xD3D3 || id == 0x0000 || id == 0xFFFF) id = ID_PADRAO_TELA;
  tft.begin(id);
  tft.setRotation(ROTACAO);
  
  if (tft.width() < 480 && tft.height() < 480) {
    tft.begin(ID_PADRAO_TELA);
    tft.setRotation(ROTACAO);
  }

  tft.invertDisplay(INVERTER_CORES);
  tft.setTextWrap(false);
  W = tft.width();
  H = tft.height();

  mostraTela(T_INICIO);
  Serial.println(F("HELLO"));
}

void loop() {
  lerSerial();
  tratarToque();
  animacoes();
}