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
| BTN_OK (milieu) | **7** | S3 → GND (partagé avec ENC1 SW) |
| ~~BTN_FACE~~ | ~~18~~ | **supprimé** — trois boutons suffisent, le menu couvre tout. GPIO18 redevient libre. |
| ENC_A / ENC_B | **4 / 5** | ENC1 (empreinte non montée) |
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

**J2 plateau** : 1 = +5V_LED (sortie de U5) · 2 = GND · 3 = DATA (sortie de R3).

**J3 écran** : 1 = GND · 2 = +3V3 · 3 = SCL (GPIO21) · 4 = SDA (GPIO47).
R6/R7 4,7 kΩ vers +3V3 **non montées** si le module écran porte déjà ses tirages.

---

## À vérifier une fois le schéma saisi

1. **U5 broche 3 est bien à la masse**, pas à IN.
2. **U3 broche 15 (pastille exposée) est bien reliée à GND** — un oubli classique.
3. Le diviseur R8/R9 est bien sur FB (broche 5) de U4, pas sur EN.
4. La paire D+/D− part de U2 vers **GPIO19/20** et nulle part ailleurs.
5. Aucune résistance de tirage ajoutée sur les boutons.
