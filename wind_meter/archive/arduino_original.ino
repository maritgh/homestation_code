#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include "HX711.h"
#include <HTTPClient.h>
#include <Preferences.h>

// Création de l'objet Preferences
Preferences preferences;


const char *serverName = "http://igrand38.local/csv"; /* <------------------------------External personnal weather Station, give data with this CSV Format:

temperature,dewp,humidity,pressure,wind,gust,winddir,rain1h,rain24h,tMin,tMax,wMax,gMax,pMin,pMax,
30.17,26.20,80.15,1016.48,10.89,13.32,326.00,0.00,0.00,25.00,30.17,10.91,17.64,1013.00,1016.77,

*/

// HX711 Setup
HX711 scaleNS;
HX711 scaleEW;

uint8_t dataPinNS = 26;      // <-------- data pin N-S loadcell
uint8_t clockPinNS = 23;     // <-------- clock pin N-S loadcell
uint8_t dataPinEW = 19;      // <-------- data pin E-W loadcell
uint8_t clockPinEW = 18;     // <-------- clock pin E-W loadcell

float pressure = 1017.25;     // <------------------------------EDIT IF NO External weather Station-------------------------------
float temperature = 28.50;     // <------------------------------EDIT IF NO External weather Station-------------------------------
int humidity = 75;     // <------------------------------EDIT IF NO External weather Station-------------------------------

int plate = 80;
float scale_calibNS = -708.900330;     // <-------- Calibration N-S loadcell, Get with the HX_calibration exemple sketch from the HX711 library
float scale_calibEW = 699.668091;      // <-------- Calibration E-W loadcell, Get with the HX_calibration exemple sketch from the HX711 library
float scale_divider = 1;

int NSOffset = 563636;     // <-------- Inithial N-S loadcel Offset, don't needed to tset anymore becausi il will be mesured at startup and every 15 min
int EWOffset = -41792;     // <-------- Inithial E-W loadcel Offset, don't needed to tset anymore becausi il will be mesured at startup and every 15 min

int NSOffsetValue[7];
int EWOffsetValue[7];
int Offsetindex = 0;

const float Rd = 287.05;   //	gas constant for dry air
const float Rv = 461.5;    // gas constant for water vapor
const float g = 9.80665;   // Gravitational acceleration constant

float forceNS, forceEW, wind_speed, wind_direction;
String wind_cardinal;

// Wi-Fi Setup
const char *ssid = "WIFI_SSID";     // <------------------------------EDIT-------------------------------
const char *password = "WIFI_PASSWORD";     // <------------------------------EDIT-------------------------------

// OTA Setup
const char *otaPassword = "OTA_PASSWORD";     // <------------------------------EDIT-------------------------------
const char *otaHostname = "WindSpeedLoadCell";

// Web Server
WebServer server(80);

// Timers et buffers
const int TIMER_Wifi = 900000;   // 15 minutes en ms
const int TIMER_Tare = 60000*15;   
const int TIMER_INTERVAL = 200; // Intervalle de collecte en ms
const int TIMER_60S = 60000;   // 60 secondes en ms
const int TIMER_2_5S = 2500;   // 2.5 secondes en ms
const int TIMER_5M = 300000;   // 5 minutes en ms

float windSpeeds60s[300];     // Stockage des vitesses sur 60s 
float windDir60s[300];     // Stockage des directions sur 60s 
float windSpeeds2_5s[13];      // Stockage des vitesses sur 2.5s 
int index60s = 0, index60sD = 0, index2_5s = 0;
bool bufferFull60s = false, bufferFull2_5s = false;
bool bufferDirFull60s = false;

// Tableau pour conserver les médianes 2.5s sur 5 minutes
float median2_5sValues[120];  // 120 médianes sur 5 minutes (2.5s * 120 = 5 minutes)
int indexMedian2_5s = 0;      // Index pour le tableau des médianes 2.5s
bool bufferFullMedian2_5s = false;

float median60s = 0;          // Médiane sur 60 secondes
float maxMedian2_5s = 0;      // Rafales maximales sur 5 minutes
float medianDir60s = 0;          // Médiane sur 60 secondes
unsigned long lastCollectTime = 0;
unsigned long last60sTime = 0;
unsigned long lastWifiTime = 0;
unsigned long lastTareTime = 0;
unsigned long last2_5sTime = 0;

// Fonction pour calculer la médiane
float getMedian(float* data, int size) {
  float temp[size];
  memcpy(temp, data, size * sizeof(float));
  std::sort(temp, temp + size);
  return (size % 2 == 0) ? (temp[size / 2 - 1] + temp[size / 2]) / 2.0 : temp[size / 2];
}

float getMedianGust(float* data, int size) {
    // Vérifier que les données sont suffisantes
    if (size < 8) return NAN; // Pas assez de données pour calculer des fractions comme 1/8ème

    // Trier les données
    std::vector<float> sortedData(data, data + size);
    std::sort(sortedData.begin(), sortedData.end());

    // Indices pour les intervalles
    int halfIndex = size / 2;          // Début des 50 % supérieurs
    int excludeIndex = size - (size / 8); // Exclusion du 1/8ème supérieur

    // Vérification de la validité de l'intervalle
    if (excludeIndex <= halfIndex) {
        return NAN; // Si l'exclusion dépasse la moitié haute, renvoyer NaN
    }

    // Calculer la médiane dans la plage ajustée
    int adjustedSize = excludeIndex - halfIndex;
    if (adjustedSize % 2 == 0) {
        return (sortedData[halfIndex + adjustedSize / 2 - 1] +
                sortedData[halfIndex + adjustedSize / 2]) / 2.0;
    } else {
        return sortedData[halfIndex + adjustedSize / 2];
    }
}


// Fonction pour tarer les capteurs
void tareSensors() {
  tareSensor();
  Serial.println("Capteurs tarés !");
  server.send(200, "text/plain", "Les capteurs ont été tarés avec succès !");
}

void updateWindDirectionAndForce() {
  // Air density
  float T = temperature + 273.15;
  float p_sat = 610.94 * exp((17.625 * temperature) / (temperature + 243.04));
  float p_v = humidity / 100.0 * p_sat;
  float p_d = (pressure * 100) - p_v;
  float rho = (p_d / (Rd * T)) + (p_v / (Rv * T));

  // Plate area
  float radius = (plate / 2.0) / 1000.0;
  float surface = 3.14159 * radius * radius;

  // Forces
  float fNS = scaleNS.get_units(5);
  float fEW = scaleEW.get_units(5);
  forceNS = (fNS / 1000.0) * g;
  forceEW = (fEW / 1000.0) * g;

  float Fx = forceEW / surface;
  float Fy = forceNS / surface;
  float resultant_pressure = sqrt(Fx * Fx + Fy * Fy);
  wind_speed = sqrt((2 * resultant_pressure) / rho) * 3.6;

  // Wind direction
  wind_direction = atan2(Fx, Fy) * (180.0 / 3.14159);
  if (wind_direction < 0) wind_direction += 360.0;

  // Cardinal direction
  if ((wind_direction >= 337.5) || (wind_direction < 22.5)) wind_cardinal = "N";
  else if (wind_direction < 67.5) wind_cardinal = "NE";
  else if (wind_direction < 112.5) wind_cardinal = "E";
  else if (wind_direction < 157.5) wind_cardinal = "SE";
  else if (wind_direction < 202.5) wind_cardinal = "S";
  else if (wind_direction < 247.5) wind_cardinal = "SW";
  else if (wind_direction < 292.5) wind_cardinal = "W";
  else wind_cardinal = "NW";

}

void wificonnect() {
  // Tentative de connexion WiFi
  Serial.println("Connexion au WiFi...");
  //WiFi.disconnect(true);
  WiFi.begin(ssid, password);
  
  int retryCount = 0; // Compteur de tentatives
  const int maxRetries = 60; // Nombre maximum de tentatives

  while (WiFi.status() != WL_CONNECTED && retryCount < maxRetries) {
    delay(1000); // Attendre 500ms
    retryCount++;
    Serial.print(".");
  }

  // Vérifie si la connexion est réussie
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnecté au WiFi!");
    Serial.print("Adresse IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nÉchec de la connexion après 30 tentatives.");
    // Ajoutez ici le code pour passer à l'étape suivante
  }
}

void setup() {
  Serial.begin(115200);

  wificonnect();
  
  scaleNS.begin(dataPinNS, clockPinNS);
  scaleEW.begin(dataPinEW, clockPinEW);
  scaleNS.set_scale(scale_calibNS / scale_divider);
  scaleEW.set_scale(scale_calibEW / scale_divider);


  preferences.begin("sensorOffsets", true);  // "true" = mode lecture seule

    NSOffset = preferences.getFloat("NSOffset", 563636);  // 0.0 = valeur par défaut si non trouvé
    EWOffset = preferences.getFloat("EWOffset", -41792);

    preferences.end();

    Serial.print("NSOffset chargé: ");
    Serial.println(NSOffset);
    Serial.print("EWOffset chargé: ");
    Serial.println(EWOffset);

  scaleNS.set_offset(NSOffset);
  scaleEW.set_offset(EWOffset);

  for (byte i = 0; i < 7; i = i + 1) {
    NSOffsetValue[i] = NSOffset;
    EWOffsetValue[i] = EWOffset;
  }

  ArduinoOTA.setHostname(otaHostname);
  ArduinoOTA.setPassword(otaPassword);
  ArduinoOTA.begin();


  server.on("/", []() {
    String html = "<html><head><title>Wind Data</title></head><body>";
    html += "<h1>Wind Speed and Sensor Data</h1>";
    html += "<p><strong>temperature (deg C):</strong> " + String(temperature) + "</p>";
    html += "<p><strong>humidity (%):</strong> " + String(humidity) + "</p>";
    html += "<p><strong>pressure (hPA):</strong> " + String(pressure) + "</p>";
    html += "<p><strong>Force NS (N):</strong> " + String(forceNS) + "</p>";
    html += "<p><strong>Force EW (N):</strong> " + String(forceEW) + "</p>";
    html += "<p><strong>Vitesse instantanee (km/h):</strong> " + String(wind_speed) + "</p>";
    html += "<p><strong>Direction du vent:</strong> " + String(medianDir60s) + " (" + wind_cardinal + ")</p>";
    html += "<p><strong>Vitesse mediane 60s:</strong> " + String(median60s) + " km/h</p>";
    html += "<p><strong>Rafales maximales 5s:</strong> " + String(maxMedian2_5s) + " km/h</p>";
    html += "<p><strong>Offset scaleNS:</strong> " + String(scaleNS.get_offset()) + " </p>";
    html += "<p><strong>Offset scaleEW:</strong> " + String(scaleEW.get_offset()) + " </p>";
    html += "<p><strong>Uptime:</strong> " + String(((millis()/1000)/60)) + " minutes</p>";
    html += "<p><strong>Uptime:</strong> " + String(((millis()/1000)/60/60)) + " heures</p>";
    html += "<button onclick=\"tareSensors()\">Tarer les capteurs</button>";
    html += "<a href='/download_csv' download><button>Telecharger CSV</button></a>";
    html += "<script>function tareSensors() { fetch('/tare').then(response => alert(response.text())); }</script>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/tare", HTTP_GET, tareSensors);
  server.on("/download_csv", HTTP_GET, []() {
    // Contenu du fichier CSV
    String csvContent = "vent,rafales,dir\n";
    csvContent += String(median60s) + "," + String(maxMedian2_5s) + "," + String(medianDir60s) + "\n";

    // Réponse HTTP avec entêtes pour forcer le téléchargement
    server.sendHeader("Content-Type", "text/csv");
    server.sendHeader("Content-Disposition", "attachment; filename=wind_data.csv");
    server.send(200, "text/plain", csvContent);
  });

  server.begin();
  
  updateEnvData();

  //Remplir la variable median60s
  updateWindDirectionAndForce();
  median60s = wind_speed;

}

void loop() {
  unsigned long currentTime = millis();

  // Collecte toutes les 50ms
  if (currentTime - lastCollectTime >= TIMER_INTERVAL) {
    lastCollectTime = currentTime;
    updateWindDirectionAndForce();

    // Mise à jour des buffers pour 60 secondes et 2.5 secondes
    windSpeeds60s[index60s] = wind_speed;
    index60s = (index60s + 1) % 300;
    if (index60s == 0) bufferFull60s = true;

     windDir60s[index60sD] = wind_direction;
    index60sD = (index60sD + 1) % 300;
    if (index60sD == 0) bufferDirFull60s = true;

    windSpeeds2_5s[index2_5s] = wind_speed;
    index2_5s = (index2_5s + 1) % 13;
    if (index2_5s == 0) bufferFull2_5s = true;
  }

  // Calcul de la médiane sur 60 secondes
  if (currentTime - last60sTime >= TIMER_60S) {
    last60sTime = currentTime;
    median60s = getMedian(windSpeeds60s, bufferFull60s ? 300 : index60s);
    medianDir60s = getMedian(windDir60s, bufferDirFull60s ? 300 : index60sD);
    Serial.print("Médiane 60s : ");
    Serial.println(median60s);
  }
  
  if (currentTime - lastWifiTime >= TIMER_Wifi) {
    lastWifiTime = currentTime;
    wificonnect();
  }
  
  if (currentTime - lastTareTime >= TIMER_Tare) {
        lastTareTime = currentTime;
        tareSensor();
        updateEnvData();
  }

  // Calcul des rafales toutes les 2.5 secondes
  if (currentTime - last2_5sTime >= TIMER_2_5S) {
    last2_5sTime = currentTime;

    // Calcul de la médiane sur 2.5 secondes
    float median2_5s = getMedianGust(windSpeeds2_5s, bufferFull2_5s ? 13 : index2_5s);

    // Stocker la médiane dans le tableau des médianes 2.5s sur 5 minutes
    median2_5sValues[indexMedian2_5s] = median2_5s;
    indexMedian2_5s = (indexMedian2_5s + 1) % 120;
    if (indexMedian2_5s == 0) bufferFullMedian2_5s = true;

    // Mise à jour de la rafale maximale sur 5 minutes
    maxMedian2_5s = 0;
    int size = bufferFullMedian2_5s ? 120 : indexMedian2_5s;
    for (int i = 0; i < size; i++) {
      if (median2_5sValues[i] > maxMedian2_5s) maxMedian2_5s = median2_5sValues[i];
    }

      Serial.println("===== Mesures en Temps Réel =====");
      Serial.print("Vitesse du vent (km/h) : "); Serial.println(wind_speed);
      Serial.print("Direction du vent (deg) : "); Serial.print(medianDir60s);
      Serial.print(" | "); Serial.println(wind_cardinal);
      Serial.print("Médiane 2.5s : "); Serial.println(median2_5s);
      Serial.print("Rafales maximales (5 min) : "); Serial.println(maxMedian2_5s);
      Serial.print("Uptime (minutes): "); Serial.println(((millis()/1000)/60));
      Serial.println("=================================");
  }

  // OTA et serveur
  ArduinoOTA.handle();
  server.handleClient();

  delay(10); // Petit délai pour éviter une surcharge
}

void tareSensor() {
    Serial.println("Tare sensor...");

    const int numSamples = 7;
    int32_t tareNS[numSamples] = {0};
    int32_t tareEW[numSamples] = {0};

    // Collecte des données
    for (int i = 0; i < numSamples; i++) {
        scaleNS.tare();
        scaleEW.tare();
        tareNS[i] = scaleNS.get_offset();
        tareEW[i] = scaleEW.get_offset();
        delay(10);
    }

    // Trier les tableaux
    std::sort(tareNS, tareNS + numSamples);
    std::sort(tareEW, tareEW + numSamples);

    // Sélection de la médiane (valeur centrale)
    int32_t tareMedianNS = tareNS[numSamples / 2];
    int32_t tareMedianEW = tareEW[numSamples / 2];

    //NSOffset = tareMedianNS;
    //EWOffset = tareMedianEW;
    if ( Offsetindex >= (numSamples) ){
      Offsetindex=0;
    }
    NSOffsetValue[Offsetindex] = tareMedianNS;
    EWOffsetValue[Offsetindex] = tareMedianEW;
      Offsetindex++;


    NSOffset = getIntMedian(NSOffsetValue, numSamples);
    EWOffset = getIntMedian(EWOffsetValue, numSamples);

    // Enregistrement dans la NVS
    preferences.begin("sensorOffsets", false);  // "false" = mode lecture/écriture

    preferences.putFloat("NSOffset", NSOffset);
    preferences.putFloat("EWOffset", EWOffset);

    preferences.end();  // Toujours fermer après usage

    scaleNS.set_offset(NSOffset);
    scaleEW.set_offset(EWOffset);

    Serial.print("NS (médiane): ");
    Serial.println(NSOffset);
    Serial.print("EW (médiane): ");
    Serial.println(NSOffset);
    printArray(NSOffsetValue, numSamples);
    printArray(EWOffsetValue, numSamples);
}

void updateEnvData() {
  HTTPClient http;
  http.begin(serverName);
  int httpCode = http.GET();

  if (httpCode > 0) { // Si la requête HTTP est réussie
    String payload = http.getString(); // Récupérer le contenu du CSV

    // Séparer le contenu en lignes
    int lineIndex = payload.indexOf('\n');
    if (lineIndex >= 0) {
      String secondLine = payload.substring(payload.indexOf('\n') + 1); // Ligne 2 du CSV

      // Séparer les valeurs par des virgules
      int firstComma = secondLine.indexOf(',');
      int secondComma = secondLine.indexOf(',', firstComma + 1);
      int thirdComma = secondLine.indexOf(',', secondComma + 1);
      int fourthComma = secondLine.indexOf(',', thirdComma + 1);

      if (firstComma > 0 && secondComma > 0 && thirdComma > 0 && fourthComma > 0) {
        // Extraire les valeurs
        String tempStr = secondLine.substring(0, firstComma); // temperature
        String humStr = secondLine.substring(secondComma + 1, thirdComma); // humidity
        String presStr = secondLine.substring(thirdComma + 1, fourthComma); // pressure

        // Convertir les valeurs en flottants
        temperature = tempStr.toFloat();
        humidity = humStr.toFloat();
        pressure = presStr.toFloat();
      }
    }
  } else {
    Serial.println("Erreur lors de la récupération des données");
  }
}

int getIntMedian(int arr[], int size) {
    // Copier le tableau pour éviter de modifier l'original
    int temp[size];
    memcpy(temp, arr, size * sizeof(int));
    
    // Trier le tableau (tri par sélection pour plus de simplicité)
    for (int i = 0; i < size - 1; i++) {
        for (int j = i + 1; j < size; j++) {
            if (temp[i] > temp[j]) {
                int swap = temp[i];
                temp[i] = temp[j];
                temp[j] = swap;
            }
        }
    }
    
    // Retourner la médiane (valeur du milieu)
    return temp[size / 2];
}
void printArray(int arr[], int size) {
    Serial.print("[");
    for (int i = 0; i < size; i++) {
        Serial.print(arr[i]);
        if (i < size - 1) {
            Serial.print(", ");  // Add comma and space except for the last element
        }
    }
    Serial.println("]");
}
