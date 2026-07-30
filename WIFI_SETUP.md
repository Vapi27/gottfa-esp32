# Connecter la carte au WiFi — guide client

> Aucun ordinateur, aucun câble USB, aucun logiciel à installer.
> Un téléphone ou une tablette suffit.

## 1. Premier allumage

Allumez le flipper. Au bout de quelques secondes, la carte crée son propre réseau WiFi :

| | |
|---|---|
| **Nom du réseau (SSID)** | `GottFA80-XXXXXX` — les 6 caractères sont propres à votre carte |
| **Mot de passe d'usine** | `pinball80` |
| **Adresse** | `http://192.168.4.1/` |

Depuis votre téléphone : *Réglages → Wi-Fi → GottFA80-XXXXXX*.

La page de configuration s'ouvre **toute seule** (la fenêtre « Se connecter au réseau » de
votre téléphone). Si elle ne s'ouvre pas, ouvrez un navigateur sur `http://192.168.4.1/`.

## 2. Deux choix — les deux sont valables

### A. Brancher le flipper sur votre box (recommandé si vous avez du WiFi)

1. Sur la page, la liste des réseaux détectés s'affiche. Touchez le vôtre.
   (Réseau masqué ? Tapez son nom à la main dans le champ.)
2. Saisissez le mot de passe de votre box.
3. **Se connecter.**

La carte essaie **et vous dit ce qui s'est passé** :

* **Réussi** → l'adresse de la carte sur votre réseau s'affiche. Le point d'accès
  `GottFA80-XXXXXX` s'éteint 30 secondes plus tard. Reconnectez votre téléphone à votre WiFi
  habituel, puis ouvrez **`http://gottfa.local/`** (ou l'adresse IP affichée).
* **Échoué** → la raison est indiquée en clair (*mot de passe refusé par la box*, *réseau
  introuvable*…). Le point d'accès **reste allumé**, vous pouvez corriger et réessayer.
  La carte n'est jamais laissée injoignable.

Le réseau est mémorisé : aux allumages suivants, la carte s'y reconnecte toute seule.

### B. Rester en mode point d'accès (aucune box nécessaire)

Le flipper n'a **pas besoin d'internet**. Vous pouvez garder son hotspot en permanence :
touchez **« Rester en mode point d'accès »**. Ce choix est enregistré, la carte ne vous
redemandera plus rien, et la fenêtre de configuration cessera de s'ouvrir à chaque connexion.

Pour utiliser le flipper : connectez-vous au réseau `GottFA80-XXXXXX` et ouvrez
`http://192.168.4.1/` (ou `http://gottfa.local/`).

## 3. Changez le mot de passe du point d'accès

> ⚠️ **À faire dès la première mise en service, surtout en mode point d'accès permanent.**

`pinball80` est le mot de passe d'usine, **identique sur toutes les cartes et publié dans cette
notice**. L'interface du flipper n'a pas de compte ni de mot de passe : toute personne connectée
au point d'accès peut piloter les bobines, reprogrammer les ROM et le FPGA. Tant que le mot de
passe d'usine est en place, cela vaut pour n'importe qui à portée de WiFi (le voisin, la rue).

Sur la page de configuration : section **« Point d'accès — sécurité » → Changer le mot de passe**
(8 caractères minimum). Le hotspot redémarre, reconnectez-vous avec le nouveau mot de passe.
Le point d'accès est toujours en **WPA2** — il n'est jamais ouvert.

## 4. Si vous changez de box / d'opérateur

Ouvrez `http://gottfa.local/wifi` depuis votre réseau actuel et refaites l'étape 2.
Vous n'avez plus accès à l'ancien réseau ? Utilisez la réinitialisation ci-dessous.

## 5. Réinitialisation (tout oublier)

Deux façons, au choix :

* **Sans ordinateur** — carte **allumée**, maintenez le bouton **BOOT** de la carte ESP32
  pendant **5 secondes**. Le réseau enregistré est effacé, le point d'accès revient avec le mot
  de passe d'usine, l'assistant est de nouveau disponible. *(La carte ne redémarre pas : cette
  manipulation est sans danger, même en cours de partie.)*
* **Depuis la page** — section **Réinitialisation → « Oublier le réseau »**.

> Le bouton doit être maintenu **pendant que la carte fonctionne**, pas à l'allumage :
> sur l'ESP32, BOOT enfoncé au démarrage fait entrer la puce en mode programmation USB et
> le programme ne démarre pas du tout.

## 6. Si le WiFi de la maison tombe

La carte réessaie toute seule. Si la box ne revient pas au bout de ~20 secondes, le point
d'accès `GottFA80-XXXXXX` **se rallume** pour que le flipper reste joignable. Dès que la box
revient, la carte s'y reconnecte et le point d'accès s'éteint (jamais pendant que quelqu'un
l'utilise).

---

# Notes techniques (installateur / développeur)

Module : `src/wifiprov.{h,cpp}` + `src/wifiprov_page.h` (page embarquée en flash).

* **Stockage : NVS** (`Preferences`, espace de noms `wifiprov`), **pas LittleFS** — la route
  `/fsup` réécrit toute l'image LittleFS, des identifiants stockés là seraient effacés par une
  simple mise à jour de l'interface web. NVS survit aussi à une OTA applicative.
* **Page embarquée en PROGMEM**, pas servie depuis LittleFS : le portail est le chemin de
  secours, il doit fonctionner même si LittleFS est vide, corrompu ou à moitié écrit par un
  `/fsup` interrompu. Coût : ~7,6 ko de flash.
* **DNS captif** sur le port 53 (toutes les requêtes → 192.168.4.1) + réponses aux sondes de
  connectivité iOS / Android / Windows / Firefox. Une fois le choix fait, ces sondes reçoivent
  la réponse « succès » attendue pour ne plus déclencher la fenêtre de connexion.
* **Rien de bloquant dans les handlers HTTP** : ils empilent une intention sous spinlock et
  répondent immédiatement (`202 Accepted`). Scans et tentatives de connexion sont pilotés par
  `wifiprov::tick()` depuis `loop()`.
* Le mot de passe du réseau domestique n'est **jamais** journalisé, ni renvoyé par une route
  HTTP ; les traces série n'affichent que sa longueur.
* Réglages surchargeables à la compilation :
  `-DWIFIPROV_AP_BASE='"MonFlipper"'`, `-DWIFIPROV_AP_PASS_DEFAULT='"..."'` (utile pour livrer
  un lot de cartes avec un mot de passe d'usine propre au revendeur).
* Bouton usine : **GPIO0** (BOOT) sur ESP32-S3, libre dans `include/board_config.h`
  (JTAG TCK y est sur GPIO4). Sur la cible **C3**, GPIO0 **est** `PIN_JTAG_TCK` : le bouton est
  retiré à la compilation, seule l'action web reste.
* `WIFI_STA_SSID` / `WIFI_STA_PASS` / `WIFI_AP_SSID` / `WIFI_AP_PASS` ont été **supprimés** de
  `include/board_config.h` en v1.0.0 : ils étaient morts depuis l'arrivée de ce module, et l'un
  d'eux contenait un vrai mot de passe de box, commité dans git. Seul `WIFI_STA_TIMEOUT_MS`
  reste — c'est le budget du premier essai de connexion au boot.
* `GET /sysinfo` indique `"apPassDefault":true` tant que le mot de passe d'usine n'a pas été
  changé : de quoi repérer d'un coup d'œil les cartes d'un lot restées au réglage par défaut.
