# Wall Pinball Playfield — matériel de la carte définitive

Tout ce qui suit est **relevé du firmware qui tourne**, pas estimé. Chaque
contrainte porte sa source, pour qu'elle soit revérifiable quand le code bouge.

Mesuré le 2026-08-07 sur le build `95f4bf1` et sur le mur `Arena`
(MAC `14:C1:9F:22:84:58`).

---

## 1. Ce que le firmware impose

| Contrainte | Valeur | D'où elle vient |
|---|---|---|
| Flash | **16 Mo minimum** | `partitions_s3.csv` finit à `0x820000` = 8,12 Mo. Un module 8 Mo ne tient pas. |
| PSRAM | **octale** | `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y` dans le `sdkconfig` compilé |
| RAM interne | 62 388 / 327 680 o (19 %) | sortie du build |
| Taille de l'application | 974 ko dans un emplacement de 3 Mo | sortie du build |
| Pixels maximum | **150** | `LED_MAX` |
| Pixels par défaut | 100 | `LED_COUNT_DEFAULT` |
| Mur de référence | 42 | `/api/state` |
| Courant par canal | 17,5 mA | `LED_MA_PER_CHANNEL` |
| Courant théorique maximal | **10,5 A** | 150 × 4 canaux × 17,5 mA |
| Plafond appliqué | **9,0 A** | `LED_POWER_BUDGET_MA` — le firmware assombrit l'image entière plutôt que de dépasser |
| Rafraîchissement | 60 Hz | `LED_FRAME_HZ` (4,8 ms sur le fil à 150 pixels) |

⚠ **Ne pas dimensionner l'alimentation sur ces 9 A.** C'est un plafond de
sécurité logiciel, pas un point de fonctionnement : le modèle du firmware compte
quatre canaux par pixel, là où le blanc d'un RGBW n'en utilise qu'un. Mesuré, le
mur maximal tire **2,92 A**. Voir §3bis, qui est la section qui décide du
connecteur, du fusible et du cuivre.

### Référence retenue

**ESP32-S3-WROOM-1-N16R8** — LCSC **C2913202**, **25,5 × 18 × 3,1 mm**, pièce
*Extended* chez JLCPCB (inspection aux rayons X, MSL 3).
Variante antenne externe : **ESP32-S3-WROOM-1U-N16R8**, LCSC **C3013946** — à
retenir si la carte finit dans un boîtier fermé, l'antenne PCB exigeant une zone
totalement dégagée de cuivre.

### Décision à trancher : la PSRAM

`CONFIG_SPIRAM=y` vient de la configuration d'exemple esp-matter, pas d'un
besoin démontré : l'application n'occupe que 19 % de la RAM interne. Un module
**N16R8** (flash 16 Mo + PSRAM octale 8 Mo) coûte sensiblement plus qu'un
**N16R2** ou un N16 nu.

**Ne pas trancher au jugé.** CHIP alloue beaucoup pendant la poignée de main
d'appairage, et c'est justement le moment où la carte avait déjà planté par
manque de tas (d'où le serveur web différé dans `arena_net.cpp`). La marche à
suivre : désactiver `CONFIG_SPIRAM`, reconstruire, **appairer réellement**, et
seulement alors conclure. Tant que ce test n'est pas fait, la carte se dessine
avec un **N16R8**.

---

## 2. Brochage (ESP32-S3)

Relevé de `include/arena_config.h`, vérifié sans doublon.

| GPIO | Fonction | Remarque |
|---|---|---|
| **16** | Données chaîne LED 1 | la sortie principale |
| 6 | Données chaîne LED 2 | `LED_CHAIN2_ENABLE 0` — désactivée, et elle **recopie** la chaîne 1, ce n'est pas une seconde zone indépendante |
| **47** | I²C SDA (écran) | mêmes broches que le GottFA80+ |
| **21** | I²C SCL (écran) | |
| **15** | Bouton ▲ | |
| **17** | Bouton ▼ | |
| **7** | Bouton OK | partagé avec le poussoir de l'encodeur |
| 4 / 5 | Encodeur A / B | optionnel, à prévoir en empreinte non montée |
| 0 | Bouton de façade | ⚠ **à déplacer, voir §3** |
| 34 | Micro | ⚠ **invalide, voir §3** |

Interdits sur ce module : **26-32** (flash), **33-37** (PSRAM octale),
**19/20** (USB natif), **48** (LED WS2812 embarquée sur le devkit).

---

## 3. Deux défauts à corriger avant de router

### 3.1 Le bouton de façade est sur la broche de strap

`PIN_ARENA_BUTTON 0`. GPIO0 est la **broche de sélection du mode de démarrage**
de l'ESP32-S3 : maintenue à la masse à la mise sous tension, elle fait démarrer
la puce en mode téléversement. Sur un prototype c'est commode. Sur une pièce
vendue, un client qui garde le doigt sur le bouton en branchant l'alimentation
obtient un mur qui ne s'allume pas et ne répond à rien — un défaut qui revient
en SAV comme une carte morte.

**À faire : déplacer le bouton de façade sur GPIO18**, libre et sans fonction de
strap. Garder GPIO0 pour un poussoir BOOT séparé, côté carte, non accessible
depuis la façade.

### 3.2 Le micro est sur une broche qui n'existe pas comme entrée analogique

`PIN_ARENA_MIC 34`, lu par `analogRead()` dans le mode Music. Sur l'ESP32-S3,
ADC1 couvre GPIO1-10 et ADC2 GPIO11-20 : **GPIO34 n'est aucun des deux**, et sur
un module à PSRAM octale la broche appartient de toute façon à la PSRAM. Le mode
Music est proposé au client dans le menu de l'écran et dans la page web, et il
ne réagit à rien.

**À trancher avant de router** : soit on retire le mode Music, soit on prévoit
un micro et on lui donne une vraie broche ADC. Un module analogique type MAX9814
sur GPIO8 se contente d'un fil ; un MEMS I²S sonne beaucoup mieux mais demande
un autre chemin de code.

---

## 3bis. Consommation réelle — MESURÉE, et elle corrige le firmware

Relevé au wattmètre sur la prise secteur, mur `Arena` (42 pixels, luminosité
255/255), bloc 5 V / 5 A :

| État | À la prise |
|---|---|
| Mur éteint (électronique seule) | **1,1 W** |
| Attract | 2,0 W |
| À fond | **5,6 W** |

En retirant la base et le rendement du bloc (~85 % en marginal), les LED tirent
**0,76 A à fond pour 42 pixels, soit 18,2 mA par pixel**.

`LED_MA_PER_CHANNEL` vaut 17,5 mA. **Le mesuré correspond donc à UN canal, pas
à quatre** : sur du RGBW le blanc se fait au canal W seul. Le modèle du firmware
multiplie par quatre et se trompe d'autant. C'est un plafond de sécurité valide,
pas un point de fonctionnement — ne jamais dimensionner l'alimentation dessus.

Extrapolation au mur maximal du firmware (150 pixels) :

| | Continu | Ampères |
|---|---|---|
| Attract | 2,7 W | 0,55 A |
| À fond | 13,7 W | 2,73 A |
| **+ électronique** | | **2,92 A** |
| Pire cas absolu (4 canaux) | 52 W | 10,5 A — plafonné à 9 A par le firmware |

**Conséquence : un USB-C 5 V nu (3 A) ne laisse que 3 % de marge** sur un mur de
150 pixels. Suffisant pour un mur de 42, insuffisant pour le plus grand que le
firmware accepte.

### Décision retenue

- **Entrée USB-C 5 V directe**, deux résistances de 5,1 kΩ sur CC1/CC2. Ni PD ni
  convertisseur pour le rail LED.
- **`LED_POWER_BUDGET_MA` = 2400** : le garde-fou est logiciel, pas matériel. Le
  firmware assombrit la trame entière plutôt que de dépasser, donc la carte ne
  peut pas tirer plus que ce qu'un USB-C accepte, quoi que fasse le client.
- **Cuivre et fusible dimensionnés 5 A**, pas 3 : le surcoût est nul et ça ouvre
  la variante grand mur sans refaire la carte.
- **Empreinte CH224K + abaisseur laissée non montée** : à peupler pour négocier
  15-20 V et redescendre en 5 V, quand un mur dépassera ~130 pixels.

Largeurs de piste, IPC-2221, cuivre 1 oz en couche externe, +10 °C :

| Courant | Largeur |
|---|---|
| 3 A | 1,4 mm |
| **5 A** | **2,8 mm** ← à router |
| 9 A | 6,3 mm |

### Chaînage

Quatre murs en attract : **1,4 A** — dans les 3 A d'un seul USB-C.
Quatre murs à fond : **3,8 A** — ça déborde.

Le firmware connaît déjà le nombre de murs présents (`arena_peers.cpp`). Diviser
le budget par ce nombre rendrait le chaînage sûr par construction. Non implémenté
à ce jour.

## 4. Blocs de la carte

### A — Entrée d'alimentation (USB-C 5 V)

Dimensionné sur les mesures du §3bis : besoin réel **2,92 A** au pire, cuivre et
protection posés à **5 A** pour ouvrir la variante grand mur sans refaire la
carte.

| Rôle | Spécification | Pourquoi |
|---|---|---|
| Connecteur | réceptacle **USB-C 16 broches** | alimente et flashe par le même câble |
| Négociation | 2 × **5,1 kΩ** de CC1 et CC2 vers la masse | sans elles aucun chargeur ne débite. C'est tout ce qu'il faut pour 5 V / 3 A |
| Protection ESD | réseau type **USBLC6-2SC6** sur D+/D− | |
| Paire USB | **90 Ω différentiel**, courte et appairée, vers GPIO19/20 | USB natif du S3 : pas de pont série à acheter |
| Fusible | **5 A**, boîtier 1206 ou 2920 | au-dessus du besoin réel, en dessous de ce qu'un chargeur peut fournir en défaut |
| Anti-inversion | **non nécessaire** | l'USB-C ne peut pas être branché à l'envers ; c'est un des intérêts du connecteur |
| Écrêtage | TVS unidirectionnel 5 V, SOD-123 | transitoire à l'enfichage |
| Réservoir | 2 × 470 µF 10 V faible ESR + 4 × 100 nF | la chaîne appelle par bouffées à chaque trame |

Pistes 5 V et masse : **2,8 mm** en cuivre 1 oz externe (IPC-2221, +10 °C).

**Empreinte à prévoir non montée : CH224K (LCSC C970725) + abaisseur.** Elle ne
sert que le jour où un mur dépassera ~130 pixels : le CH224K négocie alors 15 ou
20 V, l'abaisseur redescend en 5 V, et le même circuit imprimé encaisse jusqu'à
45 W. Quelques millimètres carrés aujourd'hui contre une refonte plus tard.

**Sortie de chaînage.** Un second USB-C présentant 5 V est légitime à condition
de porter les résistances **Rp** côté source (et non Rd) — un réceptacle qui
sort de la tension sans se déclarer est hors spécification. ⚠ Ne **jamais** y
présenter 20 V : un téléphone branché dessus attend 5 V tant qu'il n'a pas
négocié, et n'y survit pas. Si la variante PD est un jour peuplée, la sortie de
chaînage doit passer sur un connecteur détrompé, pas sur un USB-C.

### B — Rail 3,3 V

Convertisseur **abaisseur à découpage**, 5 V → 3,3 V, **1,5 A minimum**.
Pas de régulateur linéaire : un AMS1117 dissiperait `(5 − 3,3) × 0,5 = 0,85 W`
dans un boîtier SOT-223, à côté d'un plan qui porte déjà 9 A.

Bobine 2,2 à 4,7 µH selon le composant retenu, 2 × 22 µF en sortie, et le
diviseur de retour au plus près.

L'ESP32-S3 appelle des pointes de l'ordre de 500 mA en émission WiFi, sur un
rail par ailleurs calme : la marge sert à ça. Mesuré, toute l'électronique tient
en **1,1 W à la prise** — c'est la seule charge de ce convertisseur.

### C — Module

**ESP32-S3-WROOM-1-N16R8** (voir §1 pour la question de la PSRAM).

- Découplage : 10 µF + 3 × 100 nF au plus près des broches d'alimentation
- EN : 10 kΩ vers 3,3 V + 1 µF vers la masse (démarrage propre)
- Poussoirs BOOT (GPIO0) et RESET (EN) côté carte
- **Zone d'antenne dégagée** : aucun cuivre, aucun plan, aucune piste sous
  l'antenne, et le module en bord de carte. C'est la faute la plus fréquente et
  elle ne se voit qu'à la portée WiFi.

### D — USB-C

USB **natif** du S3 sur GPIO19/20 : pas de pont série, un composant et un coût
en moins que sur le devkit.

- Réceptacle USB-C 16 broches
- 2 × 5,1 kΩ sur CC1/CC2 vers la masse (sans quoi aucun chargeur ne débite)
- Réseau de protection ESD type USBLC6-2SC6 sur D+/D−
- Paire D+/D− en **90 Ω différentiel**, courte et appairée
- ⚠ Le 5 V de l'USB ne doit **pas** remonter sur le rail des LED : le diriger
  vers l'entrée du convertisseur 3,3 V seul, à travers une Schottky ou un
  aiguillage idéal.

### E — Sortie LED

| Rôle | Spécification | Pourquoi |
|---|---|---|
| Adaptateur de niveau | **74AHCT1G125**, SOT-23-5, alimenté en 5 V | voir ci-dessous |
| Résistance série | 330 Ω, en sortie de l'adaptateur | amortit le front, réduit le rayonnement |
| Connecteur données | JST-XH 3 broches (données + masse de référence) | le courant ne passe pas par là |
| Connecteur puissance | bornier à vis, séparé | 9 A ne passent pas dans un JST-XH |

**Pourquoi un adaptateur de niveau alors que `ARENA_LED.md` dit qu'on peut s'en
passer.** La note a raison sur son terrain : un SK6812 veut `VIH ≥ 0,7 × VDD`,
donc 3,5 V sous 5,0 V, et les 3,3 V de l'ESP sont hors spécification. La parade
documentée consiste à descendre l'alimentation à 4,4 V, ce qui ramène le seuil à
3,08 V. C'est juste, c'est mesuré, et c'est **une consigne de paillasse, pas un
produit** : elle demande au client de régler un potentiomètre au voltmètre, sous
charge, et elle dérive avec la température et le vieillissement du bloc. Un
74AHCT1G125 coûte quelques centimes, supprime la consigne, et laisse
l'alimentation à 5,0 V nominal.

Prévoir aussi **des points d'injection** : à 150 pixels et 9 A, le 5 V ne peut
pas parcourir toute la chaîne par le premier connecteur. Sortir deux paires
+5 V / masse supplémentaires sur bornier, à répartir le long du plateau.

### F — Écran et commandes

| Rôle | Spécification |
|---|---|
| Écran | SSD1306 **128 × 32** I²C, adresse 0x3C — le 0,91" allongé |
| Connecteur écran | 4 broches (VCC / GND / SCL / SDA), pas 1,25 mm ou 2,54 mm selon ton stock |
| Tirages I²C | 2 × 4,7 kΩ vers 3,3 V, **à ne pas monter** si le module en porte déjà |
| Boutons | 3 poussoirs tactiles CMS : ▲ GPIO15, ▼ GPIO17, OK GPIO7 |
| Encodeur | empreinte EC11 sur GPIO4/5/7, **non montée** — le firmware lit les deux |
| Bouton de façade | 1 poussoir sur **GPIO18** (et non GPIO0, voir §3.1) |

Pas de résistance de tirage sur les boutons : le firmware active les tirages
internes (`INPUT_PULLUP`). Chaque poussoir se câble simplement entre sa broche
et la masse. Un condensateur de 100 nF en parallèle ne fait pas de mal si le
câble jusqu'à la façade dépasse une dizaine de centimètres.

**Sur le choix de l'écran.** Le 128 × 32 est le bon compromis pour ce boîtier,
et il ferme une porte qu'il faut connaître : **le QR d'appairage Matter n'y est
pas lisible**. Le code fait 29 modules, donc un pixel par module sur 32 lignes,
donc des modules de l'ordre du dixième de millimètre — un iPhone doit s'approcher
plus près que sa distance minimale de mise au point et n'y arrive pas (constaté).
Le firmware affiche donc le code **en chiffres**, en grand, et l'appairage se
fait à la main dans l'app Maison. Si tu tiens au QR à l'écran, il faut le
**0,96" 128 × 64**, où le même code passe à 2 pixels par module. Le firmware gère
déjà les deux : une ligne, `ARENA_OLED_H`.

---

## 5. Ce qui reste hors carte

| Élément | Spécification |
|---|---|
| Bloc d'alimentation | 5 V, **15 A**, 75 W, sortie réglable de préférence |
| Pixels | SK6812MINI-RGBW-NW-P6, jusqu'à 150 |
| Cartes porte-pixel | une par insert, avec 100 nF par pixel |
| Bus de puissance | AWG18 rouge/noir |
| Sauts de données | AWG26-24, **moins de 15 cm par saut** |

---

## 6. Ordre de travail suggéré

1. **Trancher la PSRAM** — une reconstruction et un appairage réel. Décide du
   module, donc du prix de série.
2. **Trancher le micro** — retirer le mode Music, ou lui donner une broche ADC.
   Décide s'il y a un connecteur micro sur la carte.
3. Déplacer le bouton de façade sur GPIO18 dans `arena_config.h` **avant** de
   figer le schéma, pour que le code et la carte partent alignés.
4. Router, en traitant le rail 9 A et la zone d'antenne comme les deux
   contraintes qui ne pardonnent pas.
