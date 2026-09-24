#include <Arduino.h>
#include <Keypad.h>
#include <ESP32Servo.h>

struct Secteur {
  const char* nom;
  int puissance;   // en W
  int priorite;    // 1 = critique, plus le numéro est élevé, moins c'est important
  bool actif;
};

struct NiveauCrise {
  const char* nom;
  float ratio;
  int intervalleLed;
};

const int NB_SECTEURS = 6;

Secteur secteurs[NB_SECTEURS] = {
  {"Systemes Vitaux",           250, 1, true},
  {"Systemes de communication", 150, 2, true},
  {"Serre",                     300, 3, true},
  {"Laboratoire",               200, 4, true},
  {"Eclairage",                 250, 5, true},
  {"Autres Secteurs",           400, 5, true}
};

const NiveauCrise NIVEAUX_CRISE[3] = {
  {"Crise mineure",  0.75f, 800},
  {"Crise majeure",  0.50f, 300},
  {"Crise critique", 0.25f, 100}
};

// --- LED d'alerte de crise ---
const int PIN_LED_ALERTE = 2;
int intervalleLedActuel = 0;
bool etatLedAlerte = false;
unsigned long dernierChangementLedAlerte = 0;

// --- Niveau de crise en cours ---
int niveauActuel = 0;   // 0 = aucune crise déclenchée

// --- Pad numérique 4x4 ---
const byte NB_LIGNES = 4;
const byte NB_COLONNES = 4;

char touches[NB_LIGNES][NB_COLONNES] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte pinsLignes[NB_LIGNES]     = {13, 12, 14, 27};
byte pinsColonnes[NB_COLONNES] = {26, 25, 33, 32};

Keypad pad = Keypad(makeKeymap(touches), pinsLignes, pinsColonnes, NB_LIGNES, NB_COLONNES);

// --- Capteur de luminosité ---
const int PIN_LUMIERE = 35;
const int SEUIL_LUMIERE = 1500;    // À AJUSTER selon les valeurs mesurées : en dessous = pas assez de lumière
const int PIN_LED_ECLAIRAGE = 19;  // LED flash 7 couleurs HIGH = allumée à pleine puissance (3.3V)

// --- Capteur de niveau d'eau / humidité du sol ---
const int PIN_HUMIDITE_SOL = 34;
const int SEUIL_HUMIDITE_SOL = 1500;   // À AJUSTER selon les valeurs mesurées : en dessous = manque d'eau

// --- Servo (trappe d'eau) ---
const int PIN_SERVO = 18;
const int ANGLE_FERME = 0;
const int ANGLE_OUVERT = 90;
const unsigned long DUREE_ARROSAGE = 6000;          // 6 secondes d'ouverture, ajuste selon ton débit
const unsigned long PAUSE_APRES_ARROSAGE = 15000;   // 15 secondes avant de pouvoir réarroser, le temps que l'eau s'infiltre
Servo servoTrappe;
bool enArrosage = false;
unsigned long debutArrosage = 0;
unsigned long prochainArrosagePossible = 0;

int consommationTotale() {
  int total = 0;
  for (int i = 0; i < NB_SECTEURS; i++) {
    if (secteurs[i].actif) total += secteurs[i].puissance;
  }
  return total;
}

// niveau : 1, 2 ou 3
void declencherCrise(int niveau) {
  const NiveauCrise& config = NIVEAUX_CRISE[niveau - 1];
  niveauActuel = niveau;

  int totalNominal = 0;
  for (int i = 0; i < NB_SECTEURS; i++) totalNominal += secteurs[i].puissance;
  float budget = totalNominal * config.ratio;

  for (int i = 0; i < NB_SECTEURS; i++) secteurs[i].actif = true;

  int ordre[NB_SECTEURS];
  for (int i = 0; i < NB_SECTEURS; i++) ordre[i] = i;

  for (int i = 0; i < NB_SECTEURS - 1; i++) {
    for (int j = 0; j < NB_SECTEURS - 1 - i; j++) {
      if (secteurs[ordre[j]].priorite < secteurs[ordre[j + 1]].priorite) {
        int tmp = ordre[j];
        ordre[j] = ordre[j + 1];
        ordre[j + 1] = tmp;
      }
    }
  }

  for (int i = 0; i < NB_SECTEURS; i++) {
    if (consommationTotale() <= budget) break;
    Secteur& s = secteurs[ordre[i]];
    if (s.priorite == 1) continue;
    s.actif = false;
  }

  intervalleLedActuel = config.intervalleLed;

  // Au niveau critique (3), on force l'arrêt immédiat de la serre
  if (niveauActuel == 3) {
    digitalWrite(PIN_LED_ECLAIRAGE, LOW);
    servoTrappe.write(ANGLE_FERME);
    enArrosage = false;
  }
}

void afficherEtat(const char* titre) {
  Serial.println();
  Serial.println(titre);
  for (int i = 0; i < NB_SECTEURS; i++) {
    Serial.print(secteurs[i].nom);
    Serial.print(" -> ");
    Serial.println(secteurs[i].actif ? "ACTIF" : "COUPE");
  }
  Serial.print("Total : ");
  Serial.print(consommationTotale());
  Serial.println(" W");
}

void gererLedAlerte() {
  if (intervalleLedActuel == 0) {
    digitalWrite(PIN_LED_ALERTE, LOW);
    return;
  }
  unsigned long maintenant = millis();
  if (maintenant - dernierChangementLedAlerte >= (unsigned long)intervalleLedActuel) {
    etatLedAlerte = !etatLedAlerte;
    digitalWrite(PIN_LED_ALERTE, etatLedAlerte ? HIGH : LOW);
    dernierChangementLedAlerte = maintenant;
  }
}

void gererEclairage() {
  int lumiere = analogRead(PIN_LUMIERE);
  if (lumiere < SEUIL_LUMIERE) {
    digitalWrite(PIN_LED_ECLAIRAGE, HIGH);
  } else {
    digitalWrite(PIN_LED_ECLAIRAGE, LOW);
  }
}

void gererArrosage() {
  int humiditeSol = analogRead(PIN_HUMIDITE_SOL);
  unsigned long maintenant = millis();

  if (!enArrosage && humiditeSol < SEUIL_HUMIDITE_SOL && maintenant >= prochainArrosagePossible) {
    servoTrappe.write(ANGLE_OUVERT);
    enArrosage = true;
    debutArrosage = maintenant;
    Serial.println("Manque d'eau detecte -> ouverture de la trappe");
  }

  if (enArrosage) {
    bool dureeEcoulee = (maintenant - debutArrosage >= DUREE_ARROSAGE);
    bool assezDeau = (humiditeSol >= SEUIL_HUMIDITE_SOL);

    if (assezDeau || dureeEcoulee) {
      servoTrappe.write(ANGLE_FERME);
      enArrosage = false;
      prochainArrosagePossible = maintenant + PAUSE_APRES_ARROSAGE;

      if (assezDeau) {
        Serial.println("Eau suffisante detectee -> fermeture anticipee de la trappe");
      } else {
        Serial.println("Duree maximale atteinte -> fermeture de la trappe (securite)");
      }
    }
  }
}

void gererSerre() {
  // Si on est en crise critique (niveau 3), la serre est completement a l'arret
  if (niveauActuel == 3) {
    return;
  }
  gererEclairage();
  gererArrosage();
}

void leverAlerte() {
  niveauActuel = 0;
  intervalleLedActuel = 0;
  digitalWrite(PIN_LED_ALERTE, LOW);
  for (int i = 0; i < NB_SECTEURS; i++) {
    secteurs[i].actif = true;
  }
  Serial.println();
  Serial.println("=== Alerte levee : tous les secteurs sont reactives ===");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_LED_ALERTE, OUTPUT);
  digitalWrite(PIN_LED_ALERTE, LOW);

  pinMode(PIN_LED_ECLAIRAGE, OUTPUT);
  digitalWrite(PIN_LED_ECLAIRAGE, LOW);

  servoTrappe.attach(PIN_SERVO);
  servoTrappe.write(ANGLE_FERME);

  Serial.print("Total nominal : ");
  Serial.print(consommationTotale());
  Serial.println(" W");
  Serial.println("Appuie sur 1 (75%), 2 (50%) ou 3 (25%) sur le pad pour declencher une crise.");
  Serial.println("Appuie sur 0 pour lever l'alerte et reactiver tous les secteurs.");
}

void loop() {
  char touche = pad.getKey();

  if (touche == '1' || touche == '2' || touche == '3') {
    int niveau = touche - '0';
    declencherCrise(niveau);

    char titre[64];
    snprintf(titre, sizeof(titre), "=== %s (%d%%) ===",
             NIVEAUX_CRISE[niveau - 1].nom,
             (int)(NIVEAUX_CRISE[niveau - 1].ratio * 100));
    afficherEtat(titre);
  }

  if (touche == '0') {
    leverAlerte();
  }

  gererLedAlerte();
  gererSerre();
}
