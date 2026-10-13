/*
 * CP3 - Hardware Hacking | Atualização remota de firmware (OTA) segura no ESP32
 *
 * Fluxo:
 *   1. Conecta ao Wi-Fi (Wokwi-GUEST)
 *   2. Baixa o manifesto version.json (HTTPS)
 *   3. Compara a versão instalada com a anunciada (bloqueia downgrade)
 *   4. Baixa o firmware e calcula o SHA-256 durante o download
 *   5. Só ativa o novo firmware se o hash bater com o do manifesto
 *   6. Reinicia e passa a rodar a nova versão
 *
 * Prova visual: a v1.0 pisca o LED devagar (1 s); a v2.0 pisca rápido (150 ms).
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include "mbedtls/sha256.h"

// ===================== CONFIGURAÇÃO =====================
// Para gerar o firmware v2.0: troque para "2.0" e BLINK_MS para 150.
#define FW_VERSION "1.0"
#define BLINK_MS   1000

// Repositório que funciona como "servidor de atualização".
#define SERVIDOR "https://raw.githubusercontent.com/malencarcastro7-cell/esp32-ota-seguro/main/servidor/"

// Manifesto usado em cada teste (troque para rodar os testes 3, 4 e 5):
//   "version.json"              -> T1: atualização normal
//   "teste_hash_adulterado.json" -> T3: firmware com hash errado
//   "teste_url_invalida.json"    -> T4: firmware inexistente
//   "teste_downgrade.json"       -> T5: tentativa de downgrade
#define MANIFESTO "version.json"

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const int   LED_PIN   = 2;
// ========================================================

// Compatibilidade entre o core 2.x (mbedTLS 2) e o core 3.x (mbedTLS 3) do ESP32
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define SHA_START(c)        mbedtls_sha256_starts(c, 0)
  #define SHA_UPDATE(c, b, n) mbedtls_sha256_update(c, b, n)
  #define SHA_FINISH(c, o)    mbedtls_sha256_finish(c, o)
#else
  #define SHA_START(c)        mbedtls_sha256_starts_ret(c, 0)
  #define SHA_UPDATE(c, b, n) mbedtls_sha256_update_ret(c, b, n)
  #define SHA_FINISH(c, o)    mbedtls_sha256_finish_ret(c, o)
#endif

// "1.10" -> 1010 ; "2.0" -> 2000. Permite comparar versões corretamente.
long versaoNumerica(const String& v) {
  int ponto = v.indexOf('.');
  long maior = v.substring(0, ponto < 0 ? v.length() : ponto).toInt();
  long menor = ponto < 0 ? 0 : v.substring(ponto + 1).toInt();
  return maior * 1000 + menor;
}

void conectarWiFi() {
  Serial.print("[WIFI] Conectando a ");
  Serial.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[WIFI] Conectado! IP: ");
  Serial.println(WiFi.localIP());
}

// Baixa o firmware, calcula o SHA-256 em tempo real e só ativa se o hash conferir.
bool atualizarFirmware(const String& url, const String& hashEsperado) {
  WiFiClientSecure cliente;
  // LAB: certificado não validado para simplificar. Em produção usar setCACert(<CA raiz>).
  cliente.setInsecure();

  HTTPClient http;
  Serial.println("[OTA] Baixando: " + url);
  if (!http.begin(cliente, url)) {
    Serial.println("[OTA] ERRO: nao foi possivel abrir a URL do firmware.");
    return false;
  }

  int codigo = http.GET();
  if (codigo != HTTP_CODE_OK) {
    Serial.printf("[OTA] ERRO: servidor respondeu HTTP %d. Atualizacao cancelada.\n", codigo);
    http.end();
    return false;
  }

  int tamanho = http.getSize();
  Serial.printf("[OTA] Tamanho do firmware: %d bytes\n", tamanho);
  if (tamanho <= 0 || !Update.begin(tamanho)) {
    Serial.println("[OTA] ERRO: sem espaco na particao OTA.");
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  SHA_START(&sha);

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[1024];
  int recebido = 0, ultimoPct = -1;

  while (http.connected() && recebido < tamanho) {
    size_t disponivel = stream->available();
    if (disponivel) {
      int lidos = stream->readBytes(buffer, min(disponivel, sizeof(buffer)));
      SHA_UPDATE(&sha, buffer, lidos);
      if (Update.write(buffer, lidos) != (size_t)lidos) {
        Serial.println("[OTA] ERRO ao gravar na flash.");
        break;
      }
      recebido += lidos;
      int pct = (recebido * 100L) / tamanho;
      if (pct / 20 != ultimoPct / 20) {           // log a cada 20%
        Serial.printf("[OTA] Progresso: %d%%\n", pct);
        ultimoPct = pct;
      }
    }
    delay(1);
  }
  http.end();

  uint8_t digest[32];
  SHA_FINISH(&sha, digest);
  mbedtls_sha256_free(&sha);

  char hashCalculado[65];
  for (int i = 0; i < 32; i++) sprintf(hashCalculado + i * 2, "%02x", digest[i]);

  Serial.printf("[SEG] Bytes recebidos : %d de %d\n", recebido, tamanho);
  Serial.println("[SEG] SHA-256 esperado : " + hashEsperado);
  Serial.println("[SEG] SHA-256 calculado: " + String(hashCalculado));

  if (recebido != tamanho || !hashEsperado.equalsIgnoreCase(hashCalculado)) {
    Update.abort();   // descarta a particao: o firmware suspeito nunca sera executado
    Serial.println("[SEG] REJEITADO: hash diferente. Firmware NAO instalado.");
    Serial.println("[SEG] Dispositivo continua seguro na versao " FW_VERSION ".");
    return false;
  }

  Serial.println("[SEG] Integridade confirmada (hash confere).");
  if (!Update.end(true)) {
    Serial.printf("[OTA] ERRO ao finalizar: %s\n", Update.errorString());
    return false;
  }
  Serial.println("[OTA] Firmware instalado. Reiniciando em 3 s...");
  delay(3000);
  ESP.restart();
  return true;
}

void verificarAtualizacao() {
  String urlManifesto = String(SERVIDOR) + MANIFESTO;
  Serial.println("[OTA] Consultando manifesto: " + urlManifesto);

  WiFiClientSecure cliente;
  cliente.setInsecure();
  HTTPClient http;
  http.begin(cliente, urlManifesto);
  int codigo = http.GET();
  if (codigo != HTTP_CODE_OK) {
    Serial.printf("[OTA] ERRO ao consultar manifesto (HTTP %d).\n", codigo);
    http.end();
    return;
  }
  String resposta = http.getString();
  http.end();

  Serial.println("[OTA] Manifesto recebido:");
  Serial.println(resposta);

  JsonDocument doc;
  if (deserializeJson(doc, resposta)) {
    Serial.println("[SEG] REJEITADO: manifesto com formato invalido.");
    return;
  }
  String versaoNova = doc["version"] | "";
  String url        = doc["url"]     | "";
  String sha256     = doc["sha256"]  | "";

  if (versaoNova == "" || url == "" || sha256.length() != 64) {
    Serial.println("[SEG] REJEITADO: manifesto incompleto (version, url ou sha256).");
    return;
  }
  if (!url.startsWith("https://")) {
    Serial.println("[SEG] REJEITADO: firmware so pode ser baixado via HTTPS.");
    return;
  }

  Serial.println("[OTA] Versao instalada: " FW_VERSION " | Versao no servidor: " + versaoNova);
  long atual = versaoNumerica(FW_VERSION), nova = versaoNumerica(versaoNova);

  if (nova == atual) {
    Serial.println("[OTA] Firmware ja esta atualizado. Nenhuma acao necessaria.");
  } else if (nova < atual) {
    Serial.println("[SEG] REJEITADO: tentativa de DOWNGRADE bloqueada (anti-rollback).");
  } else {
    Serial.println("[OTA] Nova versao disponivel! Iniciando atualizacao...");
    atualizarFirmware(url, sha256);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  delay(500);

  const esp_partition_t* particao = esp_ota_get_running_partition();
  Serial.println();
  Serial.println("==========================================");
  Serial.println("[BOOT] Firmware versao " FW_VERSION);
  Serial.printf("[BOOT] Rodando na particao: %s\n", particao->label);
  Serial.printf("[BOOT] LED piscando a cada %d ms\n", BLINK_MS);
  Serial.println("==========================================");

  conectarWiFi();
  verificarAtualizacao();
}

void loop() {
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  delay(BLINK_MS);
}
