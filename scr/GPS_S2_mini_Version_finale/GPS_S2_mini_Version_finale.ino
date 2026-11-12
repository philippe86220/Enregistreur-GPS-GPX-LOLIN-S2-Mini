/*
  LOLIN - S2 mini V1.0.0 + shield micro SD
  RX = 1
  TX = 2

  I2C :
  static const uint8_t SDA = 33;
  static const uint8_t SCL = 35;

  SPI :
  static const uint8_t SS    = 12;
  static const uint8_t MOSI  = 11;
  static const uint8_t MISO  = 9;
  static const uint8_t SCK   = 7;

  buttonPin = 0  INPUT_PULLUP
  pinInterrupteur = 40 - R10K - 3.3 - GND

*/

#include <WiFi.h>
#include "ESPAsyncWebServer.h"
#include "AsyncTCP.h"
#include <SoftwareSerial.h>
#include <TinyGPSPlus.h>
#include <SD.h>
//ECRAN
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SleftEEN_WIDTH 128 // OLED display width, in pixels
#define SleftEEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SleftEEN_ADDRESS 0x3C

//WIFI
#include <secrets.h>


//Objet écran
Adafruit_SSD1306 display(SleftEEN_WIDTH, SleftEEN_HEIGHT, &Wire, OLED_RESET);


//WIFI
const char* ssid     = SECRET_SSID;
const char* password = SECRET_PASS;

// Défini une adresse IP statique
IPAddress local_IP(IP_STATIQUE);
// Défini l’adresse IP de la passerelle
IPAddress gateway(PASSERELLE);
// Défini le masque de sous-réseau
IPAddress subnet(SUBNET);

AsyncWebServer server(SERVEUR); // serveur

//  carte sd
const uint8_t chipSelect = 12;  // Lecteur SD D4
File gpx;
unsigned short fileID = 0;
char GPXFILE[16];

// Timer - rythme l'ajout des Trackpoints et le chrono
unsigned long t_start;   // timer for sampling
unsigned long t_ms;
unsigned long t_chrono;   // timer for sampling chrono
const int PROGMEM sample = 1500; // ms entre deux points

// objets pour le bouton, serveur...
const uint8_t buttonPin = 0;
const uint8_t pinInterrupteur = 40;
bool b_gpx_tracking = false; // flag for starting the gpx logging
bool b_gpx_init = false; // flag that indicates if gpx file is initialized
void affiche();
enum {TRANSFERT, ARRET, NOFIX, NB_FICHIERS, FIXSAT, EN_ATTENTE} etatCourant; // machine à état

// heure d'été
uint8_t heures;
// pour écraser le fichier le plus vieux
const byte nbFichiersMax = 200; // défini le nombre de fichiers maximum
boolean nbAtteint; // détermine  si le nombre de fichiers maximum est atteint
char nmrFichier[15];

// Connexion série avec le module GPS
SoftwareSerial ss(1, 2); // Here are the aliased pins 1 et 2

TinyGPSPlus gps;

// Déclaration des fonctions et procédures
void initGPX(void); //Initialisation du fichier GPX
void addTrackpt(void); // ajout d'un point de cheminement
void completeGPX(void); // fin de la structure GPX
// Déclaration pour écraser le fichier le plus vieux
bool isnNbTxt();
void suppressionFichiers(int x);
void fichierLePlusVieux_E(int nmr);
char* fichierLePlusVieux_L ();
// Reseau
void initReseauWifi ();
// heure d'été - heure d'hiver
bool RTCete(uint8_t anneeUTC, uint8_t moisUTC, uint8_t jourUTC, uint8_t heureUTC);

void setup() {
  // Fuseau UTC sans changement d'heure saisonnier
  setenv("TZ", "UTC0", 1);
  tzset();

  ss.begin(9600);
  //écran
  if (!display.begin(SSD1306_SWITCHCAPVCC, SleftEEN_ADDRESS)) {
    for (;;); // Don't proceed, loop forever
  }
  display.display();
  delay(2000); // Pause for 2 seconds
  display.clearDisplay();
  display.setTextSize(2);   // Normal 1:1 pixel scale
  display.setTextColor(1); // Draw white text
  display.setCursor(0, 0);

  // initialise card SD
  if (!SD.begin(chipSelect)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("ERR SD");
    display.display();
    while (1);
  }

  // initialise  boutton start et ledPin
  pinMode(buttonPin, INPUT_PULLUP); //PULLUP interne
  pinMode(pinInterrupteur, INPUT); //pin haut 3.3v - pin milieu D13 + R10K - pin bas gnd

  // machine à état
  etatCourant = TRANSFERT;
  nbAtteint = false;
}

void loop() {
  // call back machine à état - on ne traite qu'en mode GPS
  if (digitalRead(pinInterrupteur) == LOW) {
    if (ss.available() > 0) gps.encode(ss.read());
  }

  switch (etatCourant) {

    case TRANSFERT:
      if (digitalRead(pinInterrupteur) == HIGH) {
        initReseauWifi();
        etatCourant = ARRET;
      }
      else if (digitalRead(pinInterrupteur) == LOW) {
        etatCourant = NOFIX;
      }
      break;


    case ARRET:
      // Le programme est fini, il faut basculer l'interrupteur sur OFF
      break;

    case NOFIX: // pas de fix
      if (gps.location.isUpdated()) etatCourant = NB_FICHIERS;
      break;

    case NB_FICHIERS:
      if (isnNbTxt()) {
        const char* s = fichierLePlusVieux_L();
        fileID = s ? atoi(s) : 0;

        nbAtteint = true;
      }
      etatCourant = FIXSAT;
      break;

    case FIXSAT:
      if (gps.location.isUpdated() && !b_gpx_init) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("PRET !!!");
        display.display();
        if (RTCete(gps.date.year() % 100, gps.date.month(), gps.date.day(), gps.time.hour())) heures = 2;
        else heures = 1;
        etatCourant = EN_ATTENTE;
      }
      break;


    case EN_ATTENTE :

      if (digitalRead(buttonPin) == LOW && ss.available() > 0) {
        if (!b_gpx_tracking) {

          if ((fileID < nbFichiersMax) && nbAtteint)  { // le nombre de fichiers max est atteint - on supprime le prochain
            suppressionFichiers((fileID + 1) % nbFichiersMax);
          }

          if ((fileID == nbFichiersMax - 1) && !nbAtteint) { // on atteint pour la 1ère fois le nombre de fichiers max et on repart à zéro
            suppressionFichiers((fileID + 1) % nbFichiersMax);
            nbAtteint = true;
            fileID = 0;
          }
          if ((fileID == nbFichiersMax - 1) && nbAtteint)  { // le nombre de fichiers max est atteint une nouvelle fois - on supprime le prochain et on repart à zéro
            if (SD.exists("/nb.txt")) SD.remove("/nb.txt");
            suppressionFichiers((fileID + 1) % nbFichiersMax);
            fileID = 0;
          }
          // Défini un nom de fichier non existant
          while (1) {
            if (!nbAtteint && fileID >= nbFichiersMax) {   // tous les numéros sont pris (cas d'un redémarrage sans nb.txt)
              nbAtteint = true;
              fileID = 0;
              suppressionFichiers(0);        // on écrase le plus ancien
            }
            sprintf(GPXFILE, "/track%03d.gpx", fileID);
            if (!SD.exists(GPXFILE)) {
              break;
            }
            else {
              fileID++;
            }
          }
          t_chrono = millis(); // point de depart chrono
          initGPX(); // initialise l'entête du fichier GPX
          b_gpx_tracking = true;
          b_gpx_init = true; // le fichier GPX est initialisé
        }
        else {
          b_gpx_tracking = false;
        }

        delay(100);
        while (digitalRead(buttonPin) == LOW) {
          ;
        }
        delay(100);
      }

      // Si les données data sont disponibles via la connection serie
      if (ss.available() > 0) {

        // Enregistre uniquement si le délais est atteint et si le fix est effectif
        if (millis() - t_start > sample) {
          // enregistre un nouveau Trackpt
          if (b_gpx_tracking) {
            if (gps.location.isUpdated()) {
              affiche();
              addTrackpt();
            }
          }
          //Complete le fichier GPX si l'enregistrement est arrêté aprés avoir été executé une fois
          if (b_gpx_init && !b_gpx_tracking) {
            completeGPX();
            b_gpx_init = false; // le fichier actuel est terminé et n’est plus initialisé
            if (nbAtteint) fichierLePlusVieux_E(fileID);
          }

          // le timer est réinitialisé pour une prochaine itération
          t_start = millis();
        }

      }
      //break; pas de break car on reste dans l'attente permanaente de l'enregistrement de données ou on éteint tout
  } // fin switch

}

//=========================================== Initialise la structure du fichier gpx
void initGPX() {

  gpx = SD.open(GPXFILE, FILE_WRITE);

  if (!gpx) return; // échec : on sort

  // header
  gpx.print(F("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\" ?>\n"));
  // start of gpx file body
  gpx.print(F("<gpx version=\"1.1\" creator=\"P. COSTES\" xmlns=\"http://www.topografix.com/GPX/1/1\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"));
  // metadata
  gpx.print(F("  <metadata>\n"));
  gpx.print(F("    <name>")); gpx.print(GPXFILE); gpx.print(F("</name>\n"));
  gpx.print(F("    <desc>Wemos S2 mini - GPS_S2_ mini</desc>\n"));
  gpx.print(F("    <author>\n"));
  gpx.print(F("      <name>P. COSTES</name>\n"));
  //gpx.print(F("      <email>ph.bied13@gmail.com</email>\n"));
  gpx.print(F("    </author>\n"));
  gpx.print(F("  </metadata>\n"));

  // start track and track segment (only single segmented tracks possible)
  gpx.print(F("  <trk>\n"));
  gpx.print (F("    <name>")); gpx.print(GPXFILE); gpx.print(F("</name>\n"));
  gpx.print(F("    <desc>Wemos S2 mini</desc>\n"));
  gpx.print(F("    <trkseg>\n"));
  gpx.close();

}

//================================================ Création d'un trackpoint dans la syntaxe gpx
void addTrackpt() {
  //Ajoute un  Trackpoint
  gpx = SD.open(GPXFILE, FILE_APPEND); //FILE_APPEND

  if (!gpx) return; // échec : on sort

  gpx.print(F("      <trkpt lat=\""));
  gpx.print(gps.location.lat(), 8);
  gpx.print(F("\" lon=\""));
  gpx.print(gps.location.lng(), 8);
  gpx.println(F("\">"));
  gpx.print(F("        <ele>")); gpx.print(gps.altitude.meters()); gpx.print(F("</ele>\n"));

  //date et heure
  struct tm t = {};
  t.tm_year = gps.date.year() - 1900;
  t.tm_mon  = gps.date.month() - 1;
  t.tm_mday = gps.date.day();
  t.tm_hour = gps.time.hour() + heures;
  t.tm_min  = gps.time.minute();
  t.tm_sec  = gps.time.second();
  mktime(&t);
  char time[40];
  size_t n = strftime(time, sizeof(time), "%Y-%m-%dT%H:%M:%S", &t);
  snprintf(time + n, sizeof(time) - n, "+%02d:00", heures);

  gpx.print(F("        <time>")); gpx.print(time); gpx.print(F("</time>\n"));
  gpx.print(F("        <sat>")); gpx.print(gps.satellites.value()); gpx.print(F("</sat>\n"));
  gpx.print(F("        <hdop>")); gpx.print(gps.hdop.value()); gpx.print(F( "</hdop>\n"));
  gpx.print(F("      </trkpt>\n"));
  gpx.close();
}
//======================================================= Fin de la structure GPX
void completeGPX() {

  display.println(F("STOP ..."));
  display.display();
  gpx = SD.open(GPXFILE, FILE_APPEND);

  if (!gpx) return; // échec : on sort

  // Ferme le parcours enregistré, le segment et le corps du fichier gpx
  gpx.print(F("    </trkseg>\n"));
  gpx.print(F("  </trk>\n"));
  gpx.print(F("</gpx>\n"));
  gpx.close();

}

//=============================== Initialise le reseau Wifi et le serveur
void initReseauWifi () {
  // Quel est le dernier fichier ?
  if (isnNbTxt()) {
    const char* s = fichierLePlusVieux_L();
    fileID = s ? atoi(s) : 0;

    sprintf(GPXFILE, "/track%03d.gpx", fileID); // il est là
  }
  else if (!isnNbTxt()) {
    while (1) {
      sprintf(GPXFILE, "/track%03d.gpx", fileID);
      if (!SD.exists(GPXFILE)) {
        break;
      }
      else {
        fileID++;

      }
    }
    sprintf(GPXFILE, "/track%03d.gpx", fileID - 1); // il est là
  }

    // WiFi
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);                                   // mode station avant la config

  // IP statique
  bool ipStatiqueOK = WiFi.config(local_IP, gateway, subnet);

  WiFi.begin(ssid, password);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WIFI...");
  display.display();

  // attente de la connexion, 20 s maximum
  unsigned long debut = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - debut < 20000) {
    delay(500);
  }

  display.clearDisplay();
  display.setCursor(0, 0);

  if (WiFi.status() != WL_CONNECTED) {
    display.println("WIFI KO");
    display.display();
    return;                                              // pas de serveur sans Wi-Fi
  }

  if (ipStatiqueOK) {
    display.println("WIFI OK");
  }
  else {
    display.println("IP STAT KO");
    //display.println("IP DHCP :");
  }

  display.setTextSize(1,2);                                // les adresses tiennent sur une ligne
  display.println(WiFi.localIP());
  display.println(WiFi.gatewayIP());
  display.println(WiFi.subnetMask());
  display.setTextSize(2);                                // retour à la taille normale
  display.display();


  //serveur
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SD, GPXFILE, "text/xml", true);
  });
  server.on("/interpret", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SD, GPXFILE, "text/xml", false);
  });
  server.begin();

}

//=====================================// supprime un fichier sur la carte SD
void suppressionFichiers(int x) {
  char bufer[16];
  sprintf(bufer,  "/track%03d.gpx", x);
  if (SD.exists(bufer)) SD.remove(bufer);
}

//=======================================// vérifie l'existence du fichier "/nb.txt"
bool isnNbTxt() {
  while (1) {
    if (!SD.exists("/nb.txt")) {
      return false;
      break;
    }
    else {
      return true;
      break;
    }
  }
}
//==================================== écrit une valeur dans le fichier "nb.txt"
void fichierLePlusVieux_E(int nmr) {
  File nombre = SD.open("/nb.txt", FILE_WRITE);
  if (!nombre) return;
  //nombre.seek(0);
  nombre.print(nmr);
  nombre.close();
}
//==================================== lit la valeur dans le fichier "/nb.txt"
char* fichierLePlusVieux_L () {
  unsigned short indexNbe = 0;
  char r;

  File nombre = SD.open("/nb.txt");
  if (nombre) {
    while (nombre.available() > 0 ) {//&& r != -1
      r =  nombre.read() ;
      nmrFichier[indexNbe++] = r;
    }
    nombre.close();

    nmrFichier[indexNbe] = '\0';
    return nmrFichier;
  }
  else {
    return nullptr;
  }

}
//===============================================  affichage sur oled
void affiche() {
  t_ms = millis() - t_chrono;

  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print ((t_ms / 1000) / 3600); // heures
  display.print (" ");
  display.print (((t_ms / 1000) / 60) % 60);      // minutes
  display.print ("' ");
  display.print ((t_ms / 1000) % 60);      // secondes
  display.println ("''");
  display.println(GPXFILE);
  //distanceKm = TinyGPSPlus::distanceBetween(gps.location.lat(), gps.location.lng(), POSITION_LAT, POSITION_LNG) / 1000.0;
  //display.print(distanceKm);
  //display.println(" kms");
  //display.print(gps.speed.kmph());
  //display.println(" km/h");
  display.display();
}
//===============================================// calcul si on est en été
bool RTCete(uint8_t anneeUTC, uint8_t moisUTC, uint8_t jourUTC, uint8_t heureUTC)
{
  //En France métropolitaine :
  //Passage de l'heure d'hiver à l'heure d'été le dernier dimanche de mars à 1h00 UTC (à 2h00 locales il est 3h00)
  //Passage de l'heure d'été à l'heure d'hiver le dernier dimanche d'octobre à 1h00 UTC (à 3h00 locales il est 2h00)
  //Soyons propres :
  const uint8_t MARS = 3;
  const uint8_t OCTOBRE = 10;
  if (moisUTC == MARS)
  {
    uint8_t dernierDimancheMars = 31 - ((5 + anneeUTC + (anneeUTC >> 2)) % 7);
    return jourUTC > dernierDimancheMars || (jourUTC == dernierDimancheMars && heureUTC != 0);
  }
  if (moisUTC == OCTOBRE)
  {
    uint8_t dernierDimancheOctobre = 31 - ((2 + anneeUTC + (anneeUTC >> 2)) % 7);
    return jourUTC < dernierDimancheOctobre || (jourUTC == dernierDimancheOctobre && heureUTC == 0);
  }
  return MARS < moisUTC && moisUTC < OCTOBRE;
}
