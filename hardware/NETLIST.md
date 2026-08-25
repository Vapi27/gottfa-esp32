# Connexions au niveau broche

Les numéros et noms de broches ci-dessous sont **lus dans les symboles réellement
importés** (`WallPlayfield.kicad_sym`), pas dans les datasheets — ils correspondent
donc exactement à ce que KiCad t'affichera.

Ce document complète le schéma bloc ([schema_preliminaire.svg](schema_preliminaire.svg))
et la nomenclature ([BOM_PCB.csv](BOM_PCB.csv)).

---

## U4 — SY8089A1AAC, rail 3,3 V

| Broche | Nom | Connexion |
|---|---|---|
| 4 | IN | **+5V** · C17 22 µF vers GND, au plus près |
| 1 | EN | **+5V** (toujours actif) |
| 3 | LX | **L1** 2,2 µH → nœud +3V3 |
| 5 | FB | point milieu **R8 68 k** (vers +3V3) / **R9 15 k** (vers GND) |
| 2 | GND | GND |

Sortie de L1 = **+3V3** · C18 22 µF vers GND.
`VOUT = 0,6 × (1 + 68/15) = 3,32 V`.
Boucle de retour courte : router FB au plus loin de LX.

## U5 — AP22652W6-7, limiteur de la sortie LED

> Remplace l'**AP2552W6-7** (C441824), que Diodes marque **NRND** — le bandeau
> figure en page 1 de sa propre fiche : *« NOT RECOMMENDED FOR NEW DESIGN / USE
> AP22652/AP22653/AP22652A/AP22653A »*. Boîtier, brochage, polarité de l'enable,
> nature du `FAULT` et courant continu (**2,1 A**) sont **identiques** : le
> remplacement ne touche à aucune piste. **Seule R4 change** (voir plus bas), et
> le repliement en court-circuit franc passe de 2 620 mA à **700 mA typ**, ce qui
> est un gain net pour le chargeur comme pour le câblage du plateau.

| Broche | Nom | Connexion |
|---|---|---|
| 1 | IN | **+5V** · **C5 100 nF** vers GND au plus près — *« connect a 0.1uF or greater ceramic capacitor from IN to GND as close to IC as possible »* |
| 6 | OUT | **+5V_LED** · **C26 2,2 µF** vers GND → J2 broche 1 — *« bypassing the device output with a 0.1μF to 4.7μF ceramic capacitor improves the immunity of the device to short-circuit transients »* ⚠ **C26 manque encore au schéma KiCad**, à poser |
| 3 | EN/~EN | **net `EN_LED`** : R10 10 kΩ vers +5V, tiré à la masse par **SW1** — actif bas, voir ci-dessous |
| 5 | ILIM | **R4 11 k 1 %** vers GND — ⚠ **plus 10 k** : à résistance égale l'AP22652 limite 12,7 % plus haut que l'AP2552 |
| 4 | ~FAULT | **GPIO4** du module — drain ouvert, actif bas, tirage INTERNE de l'ESP |
| 2 | GND | GND |

⚠ **Ne jamais tirer `~FAULT` vers le 5 V.** U5 est alimenté en 5 V mais sa
sortie est à drain ouvert : c'est le tirage qui fixe le niveau haut. Tiré au
5 V, il mettrait **5 V sur une broche de l'ESP**. Le tirage interne de l'ESP
(vers 3,3 V) suffit — le signal est lent, aucune résistance externe n'est
nécessaire.

Le firmware le surveille : il **ignore les 1,5 premières secondes** (le limiteur
limite forcément au démarrage, le temps de charger la capacité de la chaîne) et
exige ensuite **200 ms de persistance** avant de déclarer un défaut. Un défaut
fugitif est du bruit ; un court-circuit dure. L'écran affiche alors « DEFAUT
SORTIE » en priorité sur tout le reste, et `/api/state` expose `ledfault` et
`ledfaultn`.

⚠ **La broche 3 est un enable ACTIF BAS** sur cette variante — le symbole
l'affiche `EN/~{EN}` justement parce que la famille existe dans les deux
polarités. Reliée à IN, réflexe habituel du concepteur, **le limiteur reste
désactivé et la sortie LED est morte en permanence**. Elle est tirée à la masse
**par SW1**, avec R10 en rappel vers +5V — et non câblée en dur à la masse, sans
quoi l'interrupteur marche/arrêt n'aurait plus de prise dessus.

## U6 — SN74AHCT1G125DCKR, tampon 5 V des données LED

| Broche | Nom | Connexion |
|---|---|---|
| 5 | VCC | **+5V** · **C6** 100 nF vers GND, au plus près |
| 2 | A | **GPIO16** du module (LED_DATA, 3,3 V) |
| 1 | #OE | **GND** (sortie toujours validée) |
| 4 | Y | **R3 330 Ω** → J2 broche 3 (DATA) |
| 3 | GND | GND |

C'est l'AHCT qui rend le montage légal : son `VIH` vaut **2,0 V fixe** (entrée
compatible TTL), là où un HCT ou un SK6812 attendrait `0,7 × VDD = 3,5 V`, hors
de portée d'une sortie 3,3 V.

## U2 — USBLC6-2SC6, protection ESD de l'USB

Brochage **lu dans le schéma fonctionnel du datasheet ST** (rév. 2, figure 1) —
le symbole importé ne porte que des numéros, et un résumé automatique du PDF m'a
d'abord donné un brochage faux :

```
        I/O1  1 ┌─────┐ 6  I/O1
        GND   2 │ U2  │ 5  VBUS
        I/O2  3 └─────┘ 4  I/O2
```

| Broche | Nom | Connexion |
|---|---|---|
| 1 | I/O1 | **J1 broche A7/B7 (D−)** — côté connecteur |
| 6 | I/O1 | **GPIO19** du module — côté protégé |
| 3 | I/O2 | **J1 broche A6/B6 (D+)** — côté connecteur |
| 4 | I/O2 | **GPIO20** du module — côté protégé |
| 5 | VBUS | **+5V** · **C4 100 nF** vers GND, contre le boîtier |
| 2 | GND | GND |

**Les broches 1 et 6 sont le même nœud interne, 3 et 4 aussi.** La ligne
*traverse* le composant : elle entre par le côté connecteur et ressort par le
côté puce. Ce n'est pas un dérivé posé à côté de la piste, et c'est ce qui fait
que la protection est efficace — le courant d'ESD ne peut pas contourner le
composant.

⚠ **À router dans ce sens** : connecteur → U2 → module. Poser U2 au plus près de
J1, avant que la paire n'entre dans la carte.

## U3 — MAX9814ETD+T, ampli micro à CAG

Brochage et valeurs **lus dans le datasheet Maxim** (19-0764 rév. 2, table
« Pin Description » + « Applications Information »). La version précédente de ce
document se trompait sur **six lignes** — valeurs de condensateurs, broches N.C.,
et la broche 12 purement absente.

| Broche | Nom | Connexion |
|---|---|---|
| 5 | VDD | **+3V3** · **C20 1 µF** vers GND — *« Bypass to GND with a 1 µF capacitor »* |
| 7, 15 | GND, EP | GND — *« Connect the TDFN EP to GND »*, c'est aussi le retour thermique |
| **4, 11** | N.C. | ⚠ **à la MASSE** — *« No Connection. Connect to GND »*. Pas « en l'air ». |
| 8 | MICIN | **C19 100 nF** ← MIC1 broche 1 (OUT) |
| 6 | MICOUT | **GPIO1** (broche 39 du module, ADC1_CH0) |
| **12** | **BIAS** | ⚠ **C25 470 nF** vers GND — *« Bypass BIAS with a 470nF capacitor to ground »* |
| 1 | CT | **C22 470 nF** vers GND — fixe attaque et relâchement |
| 3 | CG | **C23 2,2 µF** vers GND — *« to ensure zero offset at the output »* |
| 13 | MICBIAS | source du diviseur de seuil (voir br.14) — ne polarise aucune capsule, un MEMS s'alimente seul |
| 14 | TH | **R14 100 k** de MICBIAS vers TH, **R15 68 k** de TH vers GND |
| 9 | A/R | **en l'air** = rapport attaque/relâchement 1:4000 |
| 10 | GAIN | **en l'air** = 60 dB |
| 2 | ~SHDN | **VDD** (actif bas : à VDD, l'ampli fonctionne) |

### Trois pièges du datasheet

**Les broches 4 et 11, marquées « N.C. », vont à la masse.** Le texte est
explicite : *« No Connection. Connect to GND »*. Les laisser flotter est
l'interprétation naturelle du sigle, et elle est fausse ici.

**La broche 12 (BIAS) n'est pas optionnelle.** C'est la référence de
polarisation interne de l'ampli, bufferisée et à faible bruit. Sans son 470 nF,
les 60 dB de gain amplifient le bruit de cette référence. Elle était **absente**
de la première version de ce document.

**CT et CG ne sont pas des 100 nF.** `CT` à 470 nF donne 1,1 ms d'attaque et
4,4 s de relâchement (table 2 du datasheet, A/R en l'air) : le contrôle de gain
réagit vite au transitoire et redescend lentement, ce qui évite qu'il « pompe »
sur la musique. `CG` doit être un **2,2 µF** — c'est lui qui annule le décalage
continu en sortie.

### Le seuil du CAG — broche 14

*« To set the output-voltage threshold, an external resistor-divider must be
connected from MICBIAS to ground, with the output applied to TH. »* Le
débattement de sortie est alors limité à **deux fois V_TH**.

MICBIAS vaut 2 V. Avec **R14 100 k** et **R15 68 k** :
`V_TH = 2 × 68/168 = 0,81 V` → écrêtage à **1,6 V crête**, confortablement sous
la pleine échelle de l'ADC (3,1 V).

⚠ **Ne pas relier TH à MICBIAS** : le datasheet précise que cela **désactive le
CAG**, ce qui annulerait la raison d'avoir choisi ce composant.

### Le gain — broche 10 en l'air



Table du datasheet MAX9814, **vérifiée** — la version précédente de ce document
l'avait inversée :

| broche 10 | gain |
|---|---|
| **VDD** | 40 dB |
| **GND** | 50 dB |
| **en l'air** | **60 dB** ← retenu |

Le dimensionnement, à partir de la sensibilité du ZTS6216 (−38 dBV/Pa,
soit 12,6 mV/Pa) :

| scène | niveau du micro | à 40 dB | à 60 dB |
|---|---|---|---|
| Conversation (60 dB SPL) | 0,25 mV | 25 mV | **252 mV** |
| Musique de salon (75 dB) | 1,42 mV | 142 mV | **1,4 V** |
| Musique forte (90 dB) | 7,96 mV | 796 mV | saturé, rattrapé par le CAG |

L'ADC vise environ 1 V utile. **À 40 dB, la musique de salon ne produirait que
140 mV** — le mode Music réagirait à peine, et seulement aux passages forts. À
60 dB elle tombe pile dans la plage, et la compression du contrôle automatique
de gain écrête les pointes au lieu de saturer l'entrée.

C'est précisément le rôle du CAG, et la raison d'avoir choisi ce composant
plutôt qu'un ampli à gain fixe : on peut viser haut sans craindre les fortes
sonorités.

⚠ **En l'air veut dire vraiment en l'air** : pas de piste, et un drapeau « non
connecté » sur la broche pour que le contrôle de règles ne la signale pas.
Prévoir tout de même **deux pastilles à côté**, une vers GND et une vers VDD :
si la mise au point montre que 60 dB sature en permanence dans une pièce
bruyante, une goutte de soudure descend à 50 ou 40 sans refaire la carte.

**Les cinq condensateurs du bloc micro**, chacun son rôle — aucun n'est
optionnel, et la nomenclature n'en prévoyait d'abord que deux :

| | rôle |
|---|---|
| **C19** | couplage MIC1 br.1 → U3 br.8 (MICIN) |
| **C20** | découplage de `VDD` de U3 (br.5) |
| **C22** | sur `CT` (br.1) — constante de temps du contrôle de gain |
| **C23** | sur `CG` (br.3) |
| **C24** | découplage de `VDD` de MIC1 (br.4) |

## MIC1 — ZTS6216, micro MEMS analogique

| Broche | Nom | Connexion |
|---|---|---|
| 4 | VDD | **+3V3** · **C24** 100 nF vers GND, au plus près |
| 1 | OUT | **C19 100 nF** → U3 broche 8 (MICIN) |
| 2, 3 | GND | GND |

Port **sur le dessus** : prévoir un trou en face avant, et ne pas le boucher
avec du vernis épargne.

---

## Le module U1 — ESP32-S3-WROOM-1

| Signal | GPIO | Vers |
|---|---|---|
| LED_DATA | **16** | U6 broche 2 |
| I²C SDA | **47** | J3 broche 4 |
| I²C SCL | **21** | J3 broche 3 |
| BTN_UP (gauche) | **15** | S1 → GND |
| BTN_DOWN (droite) | **17** | S2 → GND |
| BTN_OK (milieu) | **7** | S3 → GND |
| ~~BTN_FACE~~ | ~~18~~ | **supprimé** (`ARENA_FACE_BTN_ENABLE 0`) — GPIO18 réellement libre |
| **LED_FAULT** | **4** | U5 broche 4 (~FAULT) — récupéré de l'encodeur abandonné |
| ~~ENC_B~~ | ~~5~~ | libre (`ARENA_ENC_ENABLE 0`) |
| MIC_OUT | **1** | U3 broche 6 |
| STATUS_PX | **48** | D2 (WS2812B-2020) |
| USB D− / D+ | **19 / 20** | U2 → J1 |
| BOOT | **0** | S5 → GND |
| EN | — | R5 10 k vers +3V3, C7 1 µF vers GND, S6 → GND |

Alimentation : **+3V3** · **C8 22 µF** + **C9 et C10 100 nF**, tous groupés au plus
près de la **broche 2**, retour de masse vers la broche 1 ou 40.

⚠ Le module n'a qu'**UNE SEULE broche d'alimentation** (la 2). La formulation
« un condensateur par broche d'alimentation » décrivait un composant qui
n'existe pas, et aurait fait disperser les condensateurs autour du boîtier au
routage — ce qui dégrade l'inductance de boucle au lieu de l'améliorer.
Espressif dessine 22 µF + 0,1 µF, pas davantage.

**S1, S2 et S3 sont des poussoirs à poussée LATÉRALE** (Panasonic **EVQP7A01P**,
`C79167`, série EVQ-P7 « Side Push »), à poser **en bord de carte, actionneur
débordant du contour** : gauche, droite, et OK au milieu. On presse par la
tranche — rien ne traverse le circuit, le dos reste net.

⚠ **Ne pas se fier au champ « Right Angle » de LCSC** pour juger du sens
d'actionnement : il désigne souvent l'orientation des BROCHES. L'ALPS
`C127472`, annoncé « Right Angle », est actionné **par le dessus** — vérifié sur
la fiche constructeur après coup. Seule la documentation du fabricant tranche.

⚠ Conséquence pour le routage : prévoir le **dégagement du contour** en face de
chaque actionneur (0,25 mm de course, corps 3,6 × 3,5 mm), et ne pas router de piste sous l'empreinte
côté bord. L'effort étant latéral, **élargir les pastilles** et poser les trois
poussoirs sur la même arête pour que la façade soit droite.

Aucun tirage externe sur les boutons : le firmware active les tirages internes
(`INPUT_PULLUP`). Chaque poussoir se câble simplement entre sa broche et la masse.

---

## SW1 — interrupteur marche/arrêt (ALPS SSSS811101, `C109335`)

⚠ **Il ne coupe pas le rail 5 V.** Il tient **300 mA** et la carte en tire
jusqu'à 2,92 A : le mettre en série le détruirait au premier allumage. Il
commande deux **broches de validation**, qui ne demandent que des microampères.

| Position | EN du module (U1) | EN du limiteur (U5, actif bas) | Résultat |
|---|---|---|---|
| **Marche** | tiré au haut par R5 | tiré à la masse par SW1 | tout fonctionne |
| **Arrêt** | tiré à la masse par SW1 | relâché au haut par R10 | ESP en reset, sortie LED coupée |

Câblage : le commun de SW1 va à la **masse**. Une position tire l'EN de U5 (avec
**R10 10 kΩ** de rappel vers +5V), l'autre tire l'EN du module. À l'arrêt il ne
reste que le repos du convertisseur, de l'ordre de quelques dizaines de µA.

**Brochage de SW1** — établi le 2026-08-09 sur le catalogue ALPS lui-même
(*SSSS8 Series, Drawing No.7*, celui que la table du catalogue associe au
SSSS811101), et non déduit : le repère **C du schéma de circuit est sur le
terminal ②**, qui est donc le **commun**. ① et ③ sont les deux directions.

| Terminal | Rôle | Va à |
|---|---|---|
| ② | **commun** | **GND** |
| ① | direction A | `EN_LED` → U5 broche 3, avec R10 vers +5V |
| ③ | direction B | `EN_MCU` → U1 broche 3 (EN du module) |
| ④⑤⑥⑦ | *ground terminals* (pattes de maintien) | **GND** |

⚠ Le pas des trois terminaux est **asymétrique — 3 mm puis 1,5 mm** : le commun
n'est *pas* au milieu géométriquement. L'empreinte `SW-SMD_SSSS811101` respecte
bien cet écartement (pastilles à −2,25 / +0,75 / +2,25 mm), c'est vérifié.
Regarder l'empreinte pour deviner le commun mène donc à l'erreur.

⚠ **U5 broche 3 ne doit PAS rester câblée à la masse.** Elle l'était dans le
schéma jusqu'au 2026-08-09 — partagée avec la broche 2 — ce qui rendait
l'interrupteur sans effet sur la sortie LED. Corrigé : la branche a été coupée et
la broche porte maintenant `EN_LED`.

Curseur **en bord de carte**, débordant du contour comme S1–S3 : la façade garde
une seule arête utile. Prévoir le dégagement sur 4,1 mm de course.

## Entrée et sortie

**J1 USB-C** : VBUS → F1 5 A → +5V · CC1 et CC2 chacune par **5,1 kΩ vers GND** ·
D+/D− → U2 → GPIO20/19 · masses du connecteur à la masse.
D1 (TVS) et C1·C2 (2 × 470 µF) + C21 (22 µF) + C3–C6 (100 nF) sur +5V.

**J4 chaînage** : VBUS pris **après F1** · GND commun · D+/D− **non connectés**
(on ne relaie pas l'USB de données) · CC1 et CC2 chacune par **R11/R12 12 kΩ
vers +3V3**.

⚠ Ce sont des **Rp** (tirage vers le haut, côté source), et non les Rd de 5,1 kΩ
de l'entrée. Un réceptacle USB-C qui présente de la tension sans se déclarer est
hors spécification. **12 kΩ** annonce 1,5 A pour un tirage vers 3,3 V (table 6 AN1953 : 36 k / 12 k / 4,7 k). ⚠ Les valeurs 56 k / 22 k / 10 k qu'on lit partout sont celles d'un tirage vers 5 V — les appliquer à 3,3 V n'annonce rien de valide. Ne pas mettre 4,7 kΩ (3 A) : la
carte partage déjà les 3 A de son propre chargeur, ce serait un mensonge.

⚠ **Le vrai garde-fou est logiciel.** Quatre murs, chacun plafonné à 2 A,
réclameraient 8 A à un chargeur qui en donne 3 — il replierait et toute la
chaîne s'écroulerait. Le réglage **`/api/link?shared=1`** fait diviser le
plafond de chaque mur par le nombre de murs vus sur le réseau : quatre murs
chaînés se partagent 2 A au lieu d'en demander 8. À activer sur chaque mur d'une
chaîne, et à laisser sur 0 si chacun a son propre chargeur.

**J2 plateau** : 1 = +5V_LED (sortie de U5) · 2 = GND · 3 = DATA (sortie de R3).
Bornier à ressort **KF250NH-3.5-3P** (`C976557`) depuis le 2026-08-10, à la place
du JST XH — **brochage inchangé**, seule l'empreinte change. Pas 3,50 mm au lieu
de 2,50, et pose à **0°** au lieu de 180° pour que l'entrée du fil (le côté large
du corps, 5,70 mm) regarde le bord bas, côté poussoirs.

**J3 écran** : 1 = GND · 2 = +3V3 · 3 = SCL (GPIO21) · 4 = SDA (GPIO47).
R6/R7 4,7 kΩ vers +3V3 **non montées** si le module écran porte déjà ses tirages.

---

## Table maîtresse — chaque net, chaque broche

Tout ce qui précède, vu depuis les nets plutôt que depuis les composants.

| Net | Broches reliées |
|---|---|
| **+5V** | J1 VBUS (A4/B4, A9/B9) → **F1** → tout le reste · D1 anode · C1 C2 C3–C6 C21 · U2 br.5 + C4 100 nF · U4 br.4 (IN) + C17 · U5 br.1 (IN) + C5 100 nF · U6 br.5 (VCC) + 100 nF · J4 VBUS · R10 |
| **+3V3** | U4 sortie via **L1** · C18 · U1 br.2 (3V3) · C8 + C9–C12 · U3 br.5 (VDD) + 100 nF · MIC1 br.4 (VDD) + 100 nF · J3 br.2 · D2 br.4 (VDD) · R5 · R6 R7 · R11 R12 |
| **+5V_LED** | U5 br.6 (OUT) · **C26 2,2 µF** vers GND au plus près · → **J2 br.1** |
| **GND** | commun à tout · J1 GND (A1/B1, A12/B12) · U2 br.2 · U4 br.2 · U5 br.2 · U6 br.3 · U3 br.7 **et br.15 (pastille exposée)** · MIC1 br.2 et br.3 · U1 br.1, 40, 41 · D2 br.2 · J2 br.2 · J3 br.1 · J4 GND · S1 S2 S3 · SW1 commun · R1 R2 R4 R9 · tous les découplages |
| **USB_DM** | J1 A7/B7 → U2 br.1 · U2 br.6 → U1 br.13 (**IO19**) |
| **USB_DP** | J1 A6/B6 → U2 br.3 · U2 br.4 → U1 br.14 (**IO20**) |
| **CC1** | J1 A5 → R1 5,1 k → GND |
| **CC2** | J1 B5 → R2 5,1 k → GND |
| **CC1_OUT** | J4 A5 → R11 22 k → +3V3 |
| **CC2_OUT** | J4 B5 → R12 22 k → +3V3 |
| **LED_DATA** | U1 br.9 (**IO16**) → U6 br.2 (A) |
| **LED_DATA_5V** | U6 br.4 (Y) → R3 330 Ω → **J2 br.3** |
| **ILIM** | U5 br.5 → **R4 11 k** → GND — piste la plus courte possible, la fiche l'exige : *« The traces routing the RLIM resistor should be as short as possible to reduce parasitic effects on the current-limit accuracy »* |
| **LED_FAULT** | U5 br.4 → U1 br.4 (**IO4**) — drain ouvert, tirage interne |
| **EN_LED** | U5 br.3 (EN, **actif bas**) · R10 10 k → +5V · **SW1** |
| **EN_MCU** | U1 br.3 (EN) · R5 10 k → +3V3 · C7 1 µF → GND · **SW1** · S6 → GND |
| **I2C_SDA** | U1 br.24 (**IO47**) → J3 br.4 · R6 → +3V3 (DNP) |
| **I2C_SCL** | U1 br.23 (**IO21**) → J3 br.3 · R7 → +3V3 (DNP) |
| **BTN_LEFT** | U1 br.8 (**IO15**) → S1 → GND |
| **BTN_RIGHT** | U1 br.10 (**IO17**) → S2 → GND |
| **BTN_OK** | U1 br.7 (**IO7**) → S3 → GND |
| **MIC_OUT** | MIC1 br.1 → C19 100 nF → U3 br.8 (MICIN) |
| **ADC_IN** | U3 br.6 (MICOUT) → U1 br.39 (**IO1**) |
| **STATUS_PX** | U1 br.25 (**IO48**) → D2 br.3 (DI) |
| **BOOT** | U1 br.27 (**IO0**) → S5 → GND |

### SW1 — repérage des pastilles

D'après la géométrie de l'empreinte importée :

| Pastilles | Rôle |
|---|---|
| **1, 2, 3** (alignées) | **contacts électriques** — SPDT |
| 4, 5, 6, 7 (aux quatre coins) | **ancrages mécaniques** — à relier à GND, ils ne conduisent rien |

⚠ **Lequel des trois est le commun n'est pas déductible de l'empreinte.** Sur un
SPDT c'est normalement celui du milieu (**pastille 2**), et c'est ainsi qu'il
faut câbler : commun → GND, une position vers `EN_LED`, l'autre vers `EN_MCU`.
**À confirmer à l'ohmmètre sur la pièce reçue**, avant de lancer la série. Une
erreur ici ne casse rien : au pire le mur ne s'allume pas dans une position.

## À vérifier une fois le schéma saisi

1. **U5 broche 3 est bien à la masse**, pas à IN.
2. **U3 broche 15 (pastille exposée) est bien reliée à GND** — un oubli classique.
3. Le diviseur R8/R9 est bien sur FB (broche 5) de U4, pas sur EN.
4. La paire D+/D− part de U2 vers **GPIO19/20** et nulle part ailleurs.
5. Aucune résistance de tirage ajoutée sur les boutons.
6. **U2 est traversé, pas dérivé** : D± entrent par les broches 1 et 3, ressortent
   par les broches 6 et 4. S'ils sont câblés en dérivation, la protection ne sert
   à rien.
