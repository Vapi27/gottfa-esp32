# Matter — Google Home, Apple Maison (Siri) et Alexa, en natif

Le mur s'appairera comme une lampe du commerce : QR code dans le livret →
app Maison ou Google Home → « OK Google / Dis Siri, allume le flipper ».
Local, sans nuage, les trois assistants d'un coup. Ce document est le plan
d'exécution et les décisions — y compris celles qui fâchent (certification).

## Ce que Matter change — et ce qu'il ne change pas

| | |
|---|---|
| Build | PlatformIO/Arduino → **ESP-IDF + esp-matter**, avec **arduino-esp32 en composant IDF** : tout le code existant (effets, page web AsyncWebServer, NVS, OTA, RMT) est embarqué tel quel dans le nouveau build |
| Vu des assistants | une **lampe à variateur** : On/Off (attract ↔ off), Brightness, et en option la teinte du verre (ColorControl). Les modes fins restent sur la page web — Matter voit une lampe, la page voit la machine |
| Appairage | **BLE + QR code** imprimé dans le livret (code de setup unique par carte, partition manufacturing générée par l'outil esp-matter) |
| Radio | le WROOM-32 actuel a le BLE nécessaire ✓ |

## Les deux décisions matérielles pour le PCB final

1. **Module 8 Mo** (WROOM-32E-N8 ou S3-WROOM-1-N8, quelques centimes de plus) :
   Matter + Arduino ≈ 2,5 Mo d'app — sur 4 Mo il ne reste **pas** deux slots
   OTA. 8 Mo rend l'OTA A/B confortable. Sur 4 Mo : app unique, mise à jour
   par Matter OTA uniquement — fragile pour un produit.
2. L'antenne BLE travaille pendant l'appairage seulement — pas de contrainte
   nouvelle de placement au-delà de celles du WiFi.

## La vérité sur la certification (décision de vendeur, pas de code)

Sans certification CSA (adhésion + tests, **plusieurs milliers d'euros**) :

- **Apple Maison** : l'appairage fonctionne, avec un avertissement
  « accessoire non certifié ». Acceptable avec une ligne dans la notice.
- **Google Home** : un appareil non certifié ne s'appaire que via un compte
  lié à un projet Google Home Developer Console déclarant le VID/PID de test.
  **Le client lambda ne peut pas l'appairer.** Pour vendre « compatible
  Google », la certification est en pratique incontournable.
- **Alexa** : fonctionne avec avertissement.

Conclusion produit : Matter non certifié = argument **Apple** immédiat
(mieux que HomeSpan : même parcours, plus Google/Alexa pour les bricoleurs),
argument **Google grand public** seulement après certification. À chiffrer
comme un coût produit, pas comme un détail.

## Étapes

- [x] **P0 — outillage prouvé** (2026-08-01, VPS 24 cœurs) : exemple `light`
      compilé pour ESP32 — `light.bin` 1,53 Mo. **Matrice de versions, payée en
      neuf tentatives — ne pas dévier :**

      | Composant | Version | Pourquoi précisément celle-là |
      |---|---|---|
      | esp-matter | main (2026-08) | — |
      | ESP-IDF | **v5.4.1** | main exige `esp_driver_ledc` (≥ 5.3) ; v5.2.3 échoue |
      | Python | **3.11** | le codegen CHIP appelle `getLevelNamesMapping` (3.11+) ; 3.10 échoue |
      | idf-component-manager | **~=2.2** (2.5.0) | la 3.x abandonne l'interface 3 qu'IDF 5.4.1 demande |
      | mobly (constraints CHIP) | **1.12.2** | 1.13 n'existe pas pour l'hôte ; `scripts/setup/constraints.txt` amont cassé |
      | `scripts/tests/requirements.txt` | **neutralisé** | deps de tests hôte (bluezoo…) exigent 3.11+ et ne servent pas au firmware |
      | Env | `IDF_PYTHON_CHECK_CONSTRAINTS=no` | CHIP installe cryptography 44, IDF 5.2/5.4 contraint <42 — faux conflit |
      | Deps apt | liste CHIP complète **+ libevent-dev** | ot-commissioner ne compile pas sans |

      Donnée qui engage le PCB : l'exemple **nu** fait déjà 1,53 Mo — avec
      arduino-esp32 et le firmware Arena par-dessus, le module **8 Mo** n'est
      plus une recommandation, c'est un prérequis pour garder l'OTA A/B.
- [x] **P1 — preuve radio** (2026-08-01) : exemple `light` appairé à Apple
      Maison en VID de test, Siri On/Off/Level vérifiés au port série
- [x] **P2+P3 — LE VRAI FIRMWARE APPAIRÉ** (2026-08-02) : Arena complet +
      Matter dans un binaire, appairé à Maison, Siri le pilote. 18 builds ;
      les leçons durement payées, dans l'ordre où elles ont mordu :
      1. arduino-esp32 épinglé **3.2.0** (jumeau d'IDF v5.4.1)
      2. sa lib Matter embarquée **excisée** (elle exige l'esp_matter du
         registre, pas notre build source)
      3. macros lwip `INADDR_*` dé-définies dans le seul fichier voyant les
         deux mondes
      4. **`btInUse()` surchargé** — Arduino libère la RAM du contrôleur BT
         au boot si le sketch ne s'en sert pas, tuant le BLE de CHIP
      5. **SoftAP compilé absent** + bouchon linker — compilé présent, CHIP
         l'active et ses tampons balise affament la puce (abort mesuré)
      6. **serveur web différé** après l'IP — AsyncWebServer pendant la
         crypto PASE = abort OOM
      7. **rendu LED gelé** pendant la connexion BLE (événements CHIP)
      8. **capture attract (19 Ko) différée** après l'appairage
      9. **8 s de grâce après l'IP** — l'IP arrive AVANT la fin de
         l'appairage ; dépenser la RAM à cet instant re-crée le bug n°6
      10. **l'IP se lit via `esp_netif`**, jamais `WiFi.localIP()` — sous
          Matter, Arduino jure qu'il n'y a pas de réseau pendant que CHIP
          route Siri dessus
      11. `erase_flash` **obligatoire** avant le premier flash Matter ;
          ensuite, flash d'app seule = l'appairage survit (fabric en NVS)
      Verdict matériel : 58 Ko de tas libre en régime établi sur le
      WROOM-32 — ça marche, mais le **S3 + PSRAM du PCB final** n'est plus
      un confort, c'est la marge de sécurité du produit.
- [ ] **P2 — portage** : le firmware Arena devient un composant du projet
      IDF (arduino-esp32 en composant, `data/` en LittleFS, mêmes clés NVS —
      le mappage du client survit à la migration)
- [ ] **P3 — pont** : endpoint lampe → `arenaled` (On/Off ↔ mode, Level ↔
      bright, ColorControl ↔ verre de l'ampoule)
- [ ] **P4 — produit** : partition manufacturing par carte, QR dans le
      livret, page « appairage » dans la notice
- [ ] **P5 — décision certification** (voir ci-dessus)

## Règles de migration

- Les données du client sont sacrées : mêmes clés NVS, même format de
  bundles, même API HTTP. Matter s'**ajoute**, il ne remplace rien.
- La carte du mur actuel ne migre qu'en dernier, par USB, une fois P1-P3
  prouvés sur la carte de rechange.
