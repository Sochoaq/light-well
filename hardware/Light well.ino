/************  ESP32 + AS7262 + BH1750 + Supabase + Tuya (color según mEDI)  ************/

#include <Arduino.h>
#include <Wire.h>
#include "Adafruit_AS726x.h"
#include <BH1750.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <math.h>

#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include <esp_system.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// ===================== WIFI =====================
const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;

// ===================== SUPABASE =====================
const char* SUPABASE_URL = SECRET_SUPABASE_URL;
const char* SUPABASE_TABLE_ENDPOINT = "/rest/v1/mediciones";
const char* SUPABASE_ANON_KEY = SECRET_SUPABASE_ANON_KEY;

// ===================== TUYA CLOUD =====================
const char* TUYA_API_BASE = SECRET_TUYA_API_BASE;
const char* ACCESS_ID     = SECRET_TUYA_ACCESS_ID;
const char* ACCESS_SECRET = SECRET_TUYA_ACCESS_SECRET;
const char* DEVICE_ID     = SECRET_TUYA_DEVICE_ID;

String accessToken;
time_t tokenIssuedAt = 0;
int tokenExpireSec = 0;
bool tuyaReady = false;

// ===================== COLORES / RANGOS mEDI =====================
// Ajusta estos rangos a tu gusto:
const double EDI_PINK_MIN = 0.0;
const double EDI_PINK_MAX = 10.0;

const double EDI_RED_MIN  = 10.0;
const double EDI_RED_MAX  = 20.0;

// Escalas Tuya (tu bombillo puede ser 0..255 o 0..1000)
const int TUYA_SV_MAX = 1000;
const int TUYA_S_ON   = 1000;
const int TUYA_V_ON   = 1000;

// Estado previo para no spamear
int lastColorState = -1; // -1 = ninguno, 0 = OFF, 1 = ROJO, 2 = ROSADO

// ===================== Sensores =====================
Adafruit_AS726x ams;
BH1750 lightMeter;
uint16_t sensorValues[AS726x_NUM_CHANNELS];

// ===================== CCT / EDI =====================
const int LAMBDA_START = 400;
const int LAMBDA_END   = 700;
const int LAMBDA_STEP  = 1;
const int NUM_LAMBDA   = (LAMBDA_END - LAMBDA_START) / LAMBDA_STEP + 1;

// AS7262
const double wl[6] = {450, 500, 550, 570, 600, 650};
const double sigma_default = 17;
const double sigma_violet  = 0.65 * sigma_default;
const double sigmas[6] = {sigma_violet, sigma_default, sigma_default, sigma_default, sigma_default, sigma_default};
const double a_n[6] = {1.24, 4.3, 0.95, 0.95, 0.415, 1.2};

// Extras
const double wl_extra[2] = {525, 626};
const double a_extra[2]  = {0.7, 0.35};
const double sigma_extra[2] = {sigma_default, sigma_default};

// CIE 1931 2° (380-780 cada 5 nm)
const int NUM_CIE = 81;
const int wavelengths_cie[NUM_CIE] = {
  380, 385, 390, 395, 400, 405, 410, 415, 420, 425,
  430, 435, 440, 445, 450, 455, 460, 465, 470, 475,
  480, 485, 490, 495, 500, 505, 510, 515, 520, 525,
  530, 535, 540, 545, 550, 555, 560, 565, 570, 575,
  580, 585, 590, 595, 600, 605, 610, 615, 620, 625,
  630, 635, 640, 645, 650, 655, 660, 665, 670, 675,
  680, 685, 690, 695, 700, 705, 710, 715, 720, 725,
  730, 735, 740, 745, 750, 755, 760, 765, 770, 775,
  780
};

const double x_bar[NUM_CIE] = {
  0.001368, 0.002236, 0.004243, 0.00765, 0.01431, 0.02319, 0.04351, 0.07763, 0.13438, 0.21477,
  0.2839, 0.3285, 0.34828, 0.34806, 0.3362, 0.3187, 0.2908, 0.2511, 0.19536, 0.1421,
  0.09564, 0.05795001, 0.03201, 0.0147, 0.0049, 0.0024, 0.0093, 0.0291, 0.06327, 0.1096,
  0.1655, 0.2257499, 0.2904, 0.3597, 0.4334499, 0.5120501, 0.5945, 0.6784, 0.7621, 0.8425,
  0.9163, 0.9786, 1.0263, 1.0567, 1.0622, 1.0456, 1.0026, 0.9384, 0.8544499, 0.7514,
  0.6424, 0.5419, 0.4479, 0.3608, 0.2835, 0.2187, 0.1649, 0.1212, 0.0874, 0.0636,
  0.04677, 0.0329, 0.0227, 0.01584, 0.01135916, 0.008110916, 0.005790346, 0.004109457, 0.002899327, 0.00204919,
  0.001439971, 0.000999949, 0.000690079, 0.000476021, 0.000332301, 0.000234826, 0.000166151, 0.000117413, 8.30753E-05, 5.87065E-05,
  4.15099E-05
};

const double y_bar[NUM_CIE] = {
  0.000039, 0.000064, 0.00012, 0.000217, 0.000396, 0.00064, 0.00121, 0.00218, 0.004, 0.0073,
  0.0116, 0.01684, 0.023, 0.0298, 0.038, 0.048, 0.06, 0.0739, 0.09098, 0.1126,
  0.13902, 0.1693, 0.20802, 0.2586, 0.323, 0.4073, 0.503, 0.6082, 0.71, 0.7932,
  0.862, 0.9148501, 0.954, 0.9803, 0.9949501, 1, 0.995, 0.9786, 0.952, 0.9154,
  0.87, 0.8163, 0.757, 0.6949, 0.631, 0.5668, 0.503, 0.4412, 0.381, 0.321,
  0.265, 0.217, 0.175, 0.1382, 0.107, 0.0816, 0.061, 0.04458, 0.032, 0.0232,
  0.017, 0.01192, 0.00821, 0.005723, 0.004102, 0.002929, 0.002091, 0.001484, 0.001047, 0.00074,
  0.00052, 0.0003611, 0.0002492, 0.0001719, 0.00012, 0.0000848, 0.00006, 0.0000424, 0.00003, 0.0000212,
  0.00001499
};

const double z_bar[NUM_CIE] = {
  0.006450001, 0.01054999, 0.02005001, 0.03621, 0.06785001, 0.1102, 0.2074, 0.3713, 0.6456, 1.0390501,
  1.3856, 1.62296, 1.74706, 1.7826, 1.77211, 1.7441, 1.6692, 1.5281, 1.28764, 1.0419,
  0.8129501, 0.6162, 0.46518, 0.3533, 0.272, 0.2123, 0.1582, 0.1117, 0.07824999, 0.05725001,
  0.04216, 0.02984, 0.0203, 0.0134, 0.008749999, 0.005749999, 0.0039, 0.002749999, 0.0021, 0.0018,
  0.001650001, 0.0014, 0.0011, 0.001, 0.0008, 0.0006, 0.00034, 0.00024, 0.00019, 0.0001,
  5E-05, 0.00003, 0.00002, 0.00001, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0
};

const double CCT_MIN = 2000.0;
const double CCT_MAX = 10000.0;

double calculate_mEDI(double lux, double cct) {
  if (cct < CCT_MIN) cct = CCT_MIN;
  if (cct > CCT_MAX) cct = CCT_MAX;
  double mDER = 4.324e-12 * pow(cct, 3) - 8.325e-8 * pow(cct, 2) + 6.147e-4 * cct - 0.750;
  return lux * mDER;
}

double gaussian(double x, double mu, double sigma, double amp) {
  return amp * exp(-pow(x - mu, 2) / (2 * pow(sigma, 2))) / (sigma * sqrt(2 * M_PI));
}

double linear_interp(double x, const int* wl_table, const double* val_table, int table_size) {
  if (x < wl_table[0]) return 0.0;
  if (x > wl_table[table_size - 1]) return 0.0;
  for (int i = 0; i < table_size - 1; i++) {
    if (x >= wl_table[i] && x <= wl_table[i + 1]) {
      double frac = (x - wl_table[i]) / (wl_table[i + 1] - wl_table[i]);
      return val_table[i] + frac * (val_table[i + 1] - val_table[i]);
    }
  }
  return 0.0;
}

double computeCCT_fromAS7262(int violet, int blue, int green, int yellow, int orange, int red) {
  double M_n[6] = {(double)violet, (double)blue, (double)green, (double)yellow, (double)orange, (double)red};
  double M_extra[2] = {(M_n[1] + M_n[2]) / 2.0, (M_n[4] + M_n[5]) / 2.0};

  double amps[6];
  for (int i = 0; i < 6; i++) amps[i] = M_n[i] * a_n[i];

  double amps_extra[2];
  for (int i = 0; i < 2; i++) amps_extra[i] = M_extra[i] * a_extra[i];

  double X = 0.0, Y = 0.0, Z = 0.0;

  for (int j = 0; j < NUM_LAMBDA; j++) {
    double lam = LAMBDA_START + j * LAMBDA_STEP;

    double S_here = 0.0;
    for (int i = 0; i < 6; i++) S_here += gaussian(lam, wl[i], sigmas[i], amps[i]);
    for (int i = 0; i < 2; i++) S_here += gaussian(lam, wl_extra[i], sigma_extra[i], amps_extra[i]);

    X += S_here * linear_interp(lam, wavelengths_cie, x_bar, NUM_CIE);
    Y += S_here * linear_interp(lam, wavelengths_cie, y_bar, NUM_CIE);
    Z += S_here * linear_interp(lam, wavelengths_cie, z_bar, NUM_CIE);
  }

  double total = X + Y + Z;
  double x = (total != 0) ? X / total : 0.0;
  double y = (total != 0) ? Y / total : 0.0;

  double denom = (y - 0.1858);
  if (fabs(denom) < 1e-9) denom = (denom >= 0 ? 1e-9 : -1e-9);

  double n = (x - 0.332) / denom;
  double cct = -449 * pow(n, 3) + 3525 * pow(n, 2) - 6823.3 * n + 5520.33;

  if (cct < CCT_MIN) cct = CCT_MIN;
  if (cct > CCT_MAX) cct = CCT_MAX;
  return cct;
}

// ===================== WiFi Setup =====================
bool conectarWiFi(uint32_t timeoutMs = 30000) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  WiFi.disconnect(true, true);
  delay(250);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > timeoutMs) {
      Serial.println();
      Serial.println("WiFi connection timeout");
      Serial.print("WiFi.status(): ");
      Serial.println((int)WiFi.status());
      return false;
    }
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

// ===================== NTP =====================
bool syncTimeNTP() {
  const char* ntpServer = "pool.ntp.org";
  configTime(0, 0, ntpServer);

  Serial.print("Sincronizando tiempo con NTP");
  for (int i = 0; i < 50; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        Serial.println("\nTiempo sincronizado: " + String(asctime(&timeinfo)));
        return true;
      }
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nERROR: No se pudo sincronizar el tiempo");
  return false;
}

// ===================== TUYA: Hash / Firma v2 =====================
String sha256Hex(const String& s) {
  unsigned char hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char*)s.c_str(), s.length());
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  char out[65];
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", hash[i]);
  out[64] = 0;
  return String(out);
}

String hmacSha256Upper(const String& key, const String& msg) {
  unsigned char hmac[32];
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, md, 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);

  char out[65];
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02X", hmac[i]);
  out[64] = 0;
  return String(out);
}

String buildSignV2(const String& method, const String& pathWithQuery,
                   const String& body, const String& tMs, const String& nonce,
                   const String& clientId, const String& secret, const String& accessTokOpt) {
  String stringToSign = method + "\n" + sha256Hex(body) + "\n" + "" + "\n" + pathWithQuery;
  String signStr = clientId + (accessTokOpt.length() ? accessTokOpt : "") + tMs + nonce + stringToSign;
  return hmacSha256Upper(secret, signStr);
}

bool tuyaRequest(const String& method,
                 const String& pathWithQuery,
                 const String& body,
                 String& out,
                 bool* successFlag = nullptr,
                 bool withTokenHeader = true) {

  time_t now = time(nullptr);
  if (now < 1700000000) {
    Serial.println("Tuya: tiempo no sincronizado, reintentando NTP...");
    if (!syncTimeNTP()) return false;
    now = time(nullptr);
    if (now < 1700000000) return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(TUYA_API_BASE) + pathWithQuery;

  if (!http.begin(client, url)) {
    Serial.println("ERROR: No se pudo conectar a Tuya");
    return false;
  }

  String tMs = String((uint64_t)now * 1000ULL);
  String nonce = String((uint32_t)esp_random(), HEX);

  String tokenForSign = (withTokenHeader && accessToken.length()) ? accessToken : String("");
  String sign = buildSignV2(method, pathWithQuery, body, tMs, nonce, ACCESS_ID, ACCESS_SECRET, tokenForSign);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("client_id", ACCESS_ID);
  http.addHeader("t", tMs);
  http.addHeader("sign", sign);
  http.addHeader("sign_method", "HMAC-SHA256");
  http.addHeader("nonce", nonce);
  if (withTokenHeader && accessToken.length()) {
    http.addHeader("access_token", accessToken);
  }

  int code = (method == "GET") ? http.GET() : http.POST(body);
  out = (code > 0) ? http.getString() : "";

  Serial.printf("[TUYA HTTP %s] %s -> %d\n", method.c_str(), pathWithQuery.c_str(), code);
  if (out.length()) Serial.println("TUYA Resp: " + out);

  http.end();

  if (successFlag) {
    int p = out.indexOf("\"success\":");
    if (p >= 0) *successFlag = (out.indexOf("true", p) == p + 10);
    else        *successFlag = (code >= 200 && code < 300);
  }

  return (code >= 200 && code < 300);
}

bool tuyaGetToken() {
  String old = accessToken;
  accessToken = "";

  String resp;
  bool okFlag = false;

  bool ok = tuyaRequest("GET", "/v1.0/token?grant_type=1", "", resp, &okFlag, false);
  if (!ok || !okFlag) {
    accessToken = old;
    Serial.println("ERROR obteniendo token Tuya");
    return false;
  }

  int pos = resp.indexOf("\"access_token\":\"");
  if (pos < 0) {
    accessToken = old;
    Serial.println("ERROR: Token no encontrado");
    return false;
  }

  pos += 16;
  int end = resp.indexOf("\"", pos);
  if (end < 0) {
    accessToken = old;
    return false;
  }

  accessToken = resp.substring(pos, end);
  tokenIssuedAt = time(nullptr);

  tokenExpireSec = 0;
  pos = resp.indexOf("\"expire_time\":");
  if (pos >= 0) {
    int start = pos + 14;
    int i = start;
    while (i < (int)resp.length() && isDigit(resp[i])) i++;
    tokenExpireSec = resp.substring(start, i).toInt();
  }

  Serial.println("Token Tuya OK: " + accessToken.substring(0, 10) + "...");
  if (tokenExpireSec > 0) Serial.println("Expira en: " + String(tokenExpireSec) + " s");
  return true;
}

bool tuyaTokenValid() {
  if (!accessToken.length()) return false;
  if (tokenExpireSec <= 0) return true;
  time_t now = time(nullptr);
  return (now - tokenIssuedAt) < (tokenExpireSec - 60);
}

bool tuyaSendCommands(const String& bodyJson) {
  String path = String("/v1.0/devices/") + DEVICE_ID + "/commands";
  String resp;
  bool okFlag = false;

  if (!tuyaRequest("POST", path, bodyJson, resp, &okFlag, true)) return false;

  if (!okFlag && (resp.indexOf("token") >= 0 || resp.indexOf("Token") >= 0)) {
    Serial.println("Token posiblemente inválido, renovando...");
    if (!tuyaGetToken()) return false;

    resp = "";
    okFlag = false;
    if (!tuyaRequest("POST", path, bodyJson, resp, &okFlag, true)) return false;
  }

  return okFlag;
}

// OFF (con fallback switch)
bool tuyaSwitchLed(bool on) {
  String body = String("{\"commands\":[{\"code\":\"switch_led\",\"value\":") + (on ? "true" : "false") + "}]}";
  if (tuyaSendCommands(body)) return true;

  String body2 = String("{\"commands\":[{\"code\":\"switch\",\"value\":") + (on ? "true" : "false") + "}]}";
  return tuyaSendCommands(body2);
}

/* ===================== CORREGIDO AQUÍ ===================== */
// Set color HSV usando colour_data_v2 (con fallback a colour_data)
bool tuyaSetColorHSV(int h, int s, int v) {
  if (h < 0) h = 0; if (h > 360) h = 360;
  if (s < 0) s = 0; if (s > TUYA_SV_MAX) s = TUYA_SV_MAX;
  if (v < 0) v = 0; if (v > TUYA_SV_MAX) v = TUYA_SV_MAX;

  char buf[320];

  // 1) colour_data_v2
  snprintf(buf, sizeof(buf),
    "{\"commands\":["
      "{\"code\":\"switch_led\",\"value\":true},"
      "{\"code\":\"work_mode\",\"value\":\"colour\"},"
      "{\"code\":\"colour_data_v2\",\"value\":{\"h\":%d,\"s\":%d,\"v\":%d}}"
    "]}",
    h, s, v
  );

  if (tuyaSendCommands(String(buf))) return true;

  // 2) fallback colour_data
  Serial.println("Fallback: probando 'colour_data'...");
  snprintf(buf, sizeof(buf),
    "{\"commands\":["
      "{\"code\":\"switch_led\",\"value\":true},"
      "{\"code\":\"work_mode\",\"value\":\"colour\"},"
      "{\"code\":\"colour_data\",\"value\":{\"h\":%d,\"s\":%d,\"v\":%d}}"
    "]}",
    h, s, v
  );

  return tuyaSendCommands(String(buf));
}

// ===================== SUPABASE =====================
void sendToSupabase(int violet, int blue, int green, int yellow,
                    int orange, int red, float lux, float edi) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado, no se envía a Supabase.");
    return;
  }

  WiFiClientSecure net;
  net.setInsecure();

  HTTPClient https;
  https.setReuse(false);
  https.setTimeout(15000);

  String url = String(SUPABASE_URL) + SUPABASE_TABLE_ENDPOINT;
  Serial.print("URL Supabase: ");
  Serial.println(url);

  if (!https.begin(net, url)) {
    Serial.println("Error iniciando HTTPS (https.begin falló).");
    return;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("apikey", SUPABASE_ANON_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  https.addHeader("Prefer", "return=minimal");

  String body = "{";
  body += "\"violet\":" + String(violet) + ",";
  body += "\"blue\":"   + String(blue)   + ",";
  body += "\"green\":"  + String(green)  + ",";
  body += "\"yellow\":" + String(yellow) + ",";
  body += "\"orange\":" + String(orange) + ",";
  body += "\"red\":"    + String(red)    + ",";
  body += "\"lux\":"    + String(lux, 2) + ",";
  body += "\"edi\":"    + String(edi, 6);
  body += "}";

  Serial.print("Enviando a Supabase: ");
  Serial.println(body);

  int httpCode = https.POST(body);
  Serial.print("HTTP Code: ");
  Serial.println(httpCode);

  Serial.print("Respuesta Supabase: ");
  Serial.println(https.getString());

  https.end();
}

// ===================== REGLA: estado por EDI =====================
int colorStateFromEDI(double edi) {
  if (edi >= EDI_PINK_MIN && edi < EDI_PINK_MAX) return 2; // ROSADO
  if (edi >= EDI_RED_MIN  && edi < EDI_RED_MAX)  return 1; // ROJO
  return 0; // OFF
}

bool applyColorState(int state) {
  if (state == 0) return tuyaSwitchLed(false);
  if (state == 1) return tuyaSetColorHSV(0,   TUYA_S_ON, TUYA_V_ON);   // ROJO
  if (state == 2) return tuyaSetColorHSV(330, TUYA_S_ON, TUYA_V_ON);   // ROSADO/MAGENTA
  return false;
}

// ===================== setup / loop =====================
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\nIniciando...");

  Wire.begin();

  if (!conectarWiFi(30000)) {
    Serial.println("ERROR: No hay WiFi.");
    return;
  }

  // Sensores
  if (!ams.begin()) {
    Serial.println("Error: no se pudo encontrar el AS7262.");
    while (1) delay(1000);
  }

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("Error: no se pudo iniciar el BH1750.");
    while (1) delay(1000);
  }

  Serial.println("AS7262 y BH1750 inicializados.");

  // Hora (Tuya necesita esto)
  if (!syncTimeNTP()) {
    Serial.println("ERROR: No se pudo sincronizar el tiempo. Tuya no funcionará hasta tener NTP.");
    tuyaReady = false;
  } else {
    if (!tuyaGetToken()) {
      Serial.println("ERROR: No se pudo obtener token Tuya.");
      tuyaReady = false;
    } else {
      tuyaReady = true;
    }
  }
}

void loop() {
  // WiFi
  if (!conectarWiFi(15000)) {
    Serial.println("Sin WiFi: se omite ciclo.\n");
    delay(2000);
    return;
  }

  // Lectura sensores
  uint8_t temp = ams.readTemperature();
  ams.startMeasurement();
  while (!ams.dataReady()) delay(5);
  ams.readRawValues(sensorValues);

  int violet = sensorValues[AS726x_VIOLET];
  int blue   = sensorValues[AS726x_BLUE];
  int green  = sensorValues[AS726x_GREEN];
  int yellow = sensorValues[AS726x_YELLOW];
  int orange = sensorValues[AS726x_ORANGE];
  int red    = sensorValues[AS726x_RED];

  float lux = lightMeter.readLightLevel();
  if (!isfinite(lux) || lux < 0) lux = 0;

  double cct_calculated = computeCCT_fromAS7262(violet, blue, green, yellow, orange, red);
  double edi = calculate_mEDI(lux, cct_calculated);

  Serial.println("==== Lectura ====");
  Serial.print("Temp AS7262: "); Serial.print(temp); Serial.println(" °C");
  Serial.printf("V:%d B:%d G:%d Y:%d O:%d R:%d\n", violet, blue, green, yellow, orange, red);
  Serial.print("Lux: "); Serial.print(lux); Serial.println(" lx");
  Serial.print("CCT: "); Serial.print((long)llround(cct_calculated)); Serial.println(" K");
  Serial.print("mEDI: "); Serial.print(edi, 6); Serial.println(" melanopic lux");
  Serial.println();

  // Supabase
  sendToSupabase(violet, blue, green, yellow, orange, red, lux, (float)edi);

  // ====== Tuya: control por EDI ======
  if (!tuyaReady) {
    if (time(nullptr) > 1700000000) {
      if (tuyaGetToken()) tuyaReady = true;
    }
  }

  if (tuyaReady) {
    if (!tuyaTokenValid()) {
      Serial.println("Tuya: token expirando/expirado, renovando...");
      if (!tuyaGetToken()) {
        Serial.println("Tuya: no se pudo renovar token.");
        tuyaReady = false;
      }
    }
  }

  int desiredState = colorStateFromEDI(edi);

  if (tuyaReady && desiredState != lastColorState) {
    lastColorState = desiredState;

    if (desiredState == 2) Serial.println("Tuya: RANGO => ROSADO");
    else if (desiredState == 1) Serial.println("Tuya: RANGO => ROJO");
    else Serial.println("Tuya: FUERA DE RANGO => OFF");

    if (!applyColorState(desiredState)) {
      Serial.println("Tuya: fallo aplicando estado (revisa DP codes o token/NTP).");
    }
  }

  delay(5000);
}

