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

## U5 — AP2552W6-7, limiteur de la sortie LED

| Broche | Nom | Connexion |
|---|---|---|
| 1 | IN | **+5V** |
| 6 | OUT | **+5V_LED** → J2 broche 1 |
| 3 | EN/~EN | ⚠ **GND** — voir ci-dessous |
| 5 | ILIM | **R4 10 k 1 %** vers GND |
| 4 | ~FAULT | pastille de test (collecteur ouvert, laisser en l'air sinon) |
| 2 | GND | GND |

⚠ **La broche 3 est un enable ACTIF BAS** sur cette variante — le symbole
l'affiche `EN/~{EN}` justement parce que la famille existe dans les deux
polarités. Reliée à IN, réflexe habituel du concepteur, **le limiteur reste
désactivé et la sortie LED est morte en permanence**. Elle va à la masse.

## U6 — SN74AHCT1G125DCKR, tampon 5 V des données LED

| Broche | Nom | Connexion |
|---|---|---|
| 5 | VCC | **+5V** · 100 nF vers GND |
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
| 5 | VBUS | **+5V** (protège aussi le rail) |
| 2 | GND | GND |

**Les broches 1 et 6 sont le même nœud interne, 3 et 4 aussi.** La ligne
*traverse* le composant : elle entre par le côté connecteur et ressort par le
côté puce. Ce n'est pas un dérivé posé à côté de la piste, et c'est ce qui fait
que la protection est efficace — le courant d'ESD ne peut pas contourner le
composant.

⚠ **À router dans ce sens** : connecteur → U2 → module. Poser U2 au plus près de
J1, avant que la paire n'entre dans la carte.

## U3 — MAX9814ETD+T, ampli micro à CAG

| Broche | Nom | Connexion |
|---|---|---|
| 5 | VDD | **+3V3** · 100 nF vers GND |
| 7, 15 | GND, EP | GND — **la pastille exposée doit être soudée**, c'est aussi le retour thermique |
| 8 | MICIN | **C19 100 nF** ← MIC1 broche 1 (OUT) |
| 13 | MICBIAS | **non utilisé** — un MEMS s'alimente lui-même, cette broche sert aux capsules électret. Laisser en l'air ou 1 µF vers GND. |
| 6 | MICOUT | **GPIO1** du module (ADC1_CH0) |
| 10 | GAIN | **GND** = 40 dB. En l'air = 50 dB, VDD = 60 dB. Prévoir 3 pastilles pour choisir à la main. |
| 9 | A/R | en l'air (rapport attaque/relâchement 1:4000) |
| 1 | CT | 0,1 µF vers GND (constante de temps du CAG) |
| 3 | CG | 0,1 µF vers GND |
| 14 | TH | en l'air (seuil par défaut) |
| 2 | ~SHDN | **VDD** (actif bas : à VDD, l'ampli fonctionne) |
| 4, 11 | N.C. | ne rien connecter |

## MIC1 — ZTS6216, micro MEMS analogique

| Broche | Nom | Connexion |
|---|---|---|
| 4 | VDD | **+3V3** · 100 nF vers GND au plus près |
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
| ~~BTN_FACE~~ | ~~18~~ | **supprimé** — trois boutons suffisent, le menu couvre tout. GPIO18 redevient libre. |
| ~~ENC_A / ENC_B~~ | ~~4 / 5~~ | **encodeur abandonné** — GPIO4 et GPIO5 sont libres (`ARENA_ENC_ENABLE 0`) |
| MIC_OUT | **1** | U3 broche 6 |
| STATUS_PX | **48** | D2 (WS2812B-2020) |
| USB D− / D+ | **19 / 20** | U2 → J1 |
| BOOT | **0** | S5 → GND |
| EN | — | R5 10 k vers +3V3, C7 1 µF vers GND, S6 → GND |

Alimentation : **+3V3** · C8 10 µF + C9–C12 100 nF, un par broche d'alimentation,
au plus près.

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

Curseur **en bord de carte**, débordant du contour comme S1–S3 : la façade garde
une seule arête utile. Prévoir le dégagement sur 4,1 mm de course.

## Entrée et sortie

**J1 USB-C** : VBUS → F1 5 A → +5V · CC1 et CC2 chacune par **5,1 kΩ vers GND** ·
D+/D− → U2 → GPIO20/19 · masses du connecteur à la masse.
D1 (TVS) et C1·C2 (2 × 470 µF) + C21 (22 µF) + C3–C6 (100 nF) sur +5V.

**J4 chaînage** : VBUS pris **après F1** · GND commun · D+/D− **non connectés**
(on ne relaie pas l'USB de données) · CC1 et CC2 chacune par **R11/R12 22 kΩ
vers +3V3**.

⚠ Ce sont des **Rp** (tirage vers le haut, côté source), et non les Rd de 5,1 kΩ
de l'entrée. Un réceptacle USB-C qui présente de la tension sans se déclarer est
hors spécification. 22 kΩ annonce **1,5 A** ; ne pas mettre 10 kΩ (3 A) : la
carte partage déjà les 3 A de son propre chargeur, ce serait un mensonge.

⚠ **Le vrai garde-fou est logiciel.** Quatre murs, chacun plafonné à 2 A,
réclameraient 8 A à un chargeur qui en donne 3 — il replierait et toute la
chaîne s'écroulerait. Le réglage **`/api/link?shared=1`** fait diviser le
plafond de chaque mur par le nombre de murs vus sur le réseau : quatre murs
chaînés se partagent 2 A au lieu d'en demander 8. À activer sur chaque mur d'une
chaîne, et à laisser sur 0 si chacun a son propre chargeur.

**J2 plateau** : 1 = +5V_LED (sortie de U5) · 2 = GND · 3 = DATA (sortie de R3).

**J3 écran** : 1 = GND · 2 = +3V3 · 3 = SCL (GPIO21) · 4 = SDA (GPIO47).
R6/R7 4,7 kΩ vers +3V3 **non montées** si le module écran porte déjà ses tirages.

---

## Table maîtresse — chaque net, chaque broche

Tout ce qui précède, vu depuis les nets plutôt que depuis les composants.

| Net | Broches reliées |
|---|---|
| **+5V** | J1 VBUS (A4/B4, A9/B9) → **F1** → tout le reste · D1 anode · C1 C2 C3–C6 C21 · U2 br.5 · U4 br.4 (IN) + C17 · U5 br.1 (IN) · U6 br.5 (VCC) + 100 nF · J4 VBUS · R10 |
| **+3V3** | U4 sortie via **L1** · C18 · U1 br.2 (3V3) · C8 + C9–C12 · U3 br.5 (VDD) + 100 nF · MIC1 br.4 (VDD) + 100 nF · J3 br.2 · D2 br.4 (VDD) · R5 · R6 R7 · R11 R12 |
| **+5V_LED** | U5 br.6 (OUT) → **J2 br.1** |
| **GND** | commun à tout · J1 GND (A1/B1, A12/B12) · U2 br.2 · U4 br.2 · U5 br.2 · U6 br.3 · U3 br.7 **et br.15 (pastille exposée)** · MIC1 br.2 et br.3 · U1 br.1, 40, 41 · D2 br.2 · J2 br.2 · J3 br.1 · J4 GND · S1 S2 S3 · SW1 commun · R1 R2 R4 R9 · tous les découplages |
| **USB_DM** | J1 A7/B7 → U2 br.1 · U2 br.6 → U1 br.13 (**IO19**) |
| **USB_DP** | J1 A6/B6 → U2 br.3 · U2 br.4 → U1 br.14 (**IO20**) |
| **CC1** | J1 A5 → R1 5,1 k → GND |
| **CC2** | J1 B5 → R2 5,1 k → GND |
| **CC1_OUT** | J4 A5 → R11 22 k → +3V3 |
| **CC2_OUT** | J4 B5 → R12 22 k → +3V3 |
| **LED_DATA** | U1 br.9 (**IO16**) → U6 br.2 (A) |
| **LED_DATA_5V** | U6 br.4 (Y) → R3 330 Ω → **J2 br.3** |
| **ILIM** | U5 br.5 → R4 10 k → GND |
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

Broches de U3 à traiter à part (voir sa section) : **br.10 GAIN → GND** (40 dB),
**br.2 ~SHDN → VDD**, **br.1 CT** et **br.3 CG** chacune 0,1 µF vers GND,
br.9 A/R et br.14 TH en l'air, **br.13 MICBIAS non utilisée** (le MEMS
s'alimente seul), br.4 et br.11 N.C.

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
