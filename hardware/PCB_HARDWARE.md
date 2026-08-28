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
| PSRAM | **aucune** | `CONFIG_SPIRAM` désactivé — mesuré meilleur sans, appairage compris (voir plus bas) |
| RAM interne | 62 388 / 327 680 o (19 %) | sortie du build |
| Taille de l'application | 974 ko dans un emplacement de 3 Mo | sortie du build |
| Pixels maximum | **150** | `LED_MAX` |
| Pixels par défaut | 100 | `LED_COUNT_DEFAULT` |
| Mur de référence | 42 | `/api/state` |
| Courant par canal | **R/G/B 9 mA · W 18 mA** | datasheet SK6812MINI §11 — le blanc consomme le double |
| Courant théorique maximal | **6,9 A** | 150 × (9+9+9+18+1 mA) — tous canaux à fond |
| Plafond appliqué | **1,9 A** | `LED_POWER_BUDGET_MA` — le firmware assombrit l'image entière plutôt que de dépasser. Ramené de 2,0 à 1,9 A le 2026-08-09 : il est fixé par le mini garanti du limiteur AP22652 avec R4 = 11 k (2 174 mA), et borné en dur à 2 100 mA dans `setBudgetMa()` |
| Rafraîchissement | 60 Hz | `LED_FRAME_HZ` (4,8 ms sur le fil à 150 pixels) |

Ces **6,9 A** sont le pire cas théorique — les quatre canaux de chaque pixel à
fond — que le blanc n'atteint jamais puisqu'il n'utilise que le canal W. Le
plafond firmware de 2,0 A l'arrête de toute façon. C'est la section
« Consommation réelle » plus bas qui décide du connecteur, du fusible et du
cuivre.

### Référence retenue

**ESP32-S3-WROOM-1-N16** — LCSC **C2913199**, **25,5 × 18 × 3,1 mm**, *Extended*
chez JLCPCB. **Sans PSRAM** : l'appairage Matter a été refait en entier sur une
image sans elle, et il passe avec 12,9 ko de marge de tas EN PLUS (voir plus bas).
Le **N16R8** (`C2913202`) reste un remplacement broche-à-broche pour ~0,03 $ de
plus, si tu préfères garder la porte ouverte — dans ce cas, livrer quand même
avec `CONFIG_SPIRAM` désactivé.
Variante antenne externe : **ESP32-S3-WROOM-1U-N16R8**, LCSC **C3013946** — à
retenir si la carte finit dans un boîtier fermé, l'antenne PCB exigeant une zone
totalement dégagée de cuivre.

### La PSRAM : tranchée, appairage réel à l'appui

`CONFIG_SPIRAM=y` venait de la configuration d'exemple esp-matter, pas d'un
besoin démontré. Testé sur la vraie carte, à source identique (seul
`CONFIG_SPIRAM` diffère) :

| | avec PSRAM | sans PSRAM |
|---|---|---|
| Tas libre | 79 207 | 65 124 |
| **Plancher sous charge** | **9 287** | **22 448** |
| Plus gros bloc contigu | 25 600 | 14 848 |

**La PSRAM dégrade la marge de RAM interne**, et le journal de démarrage dit
pourquoi : `esp_psram: Reserving pool of 32K of internal memory for DMA/internal
allocations`. Elle prélève 32 ko d'interne sur les 320 disponibles, au profit de
8 Mo dont cette application ne se sert jamais.

**Le test qui décide a été fait le 2026-08-07 : dépairage et réappairage complets
depuis l'app Maison, sur l'image sans PSRAM.** L'appairage est le seul moment où
CHIP alloue vraiment — c'est lui qui avait fait planter cette carte par manque de
tas, au point qu'on a dû différer le serveur web au démarrage. Il **passe**, sans
redémarrage, et le plancher reste à **22 448 octets**, soit **12,9 ko de marge de
plus** qu'avec la PSRAM.

La seule réserve identifiée — un plus gros bloc contigu tombant à 14,8 ko — ne
s'est pas matérialisée : aucune allocation d'un seul tenant de l'appairage ne
dépasse cette taille.

### Consommation réelle — MESURÉE sur cinq points

Relevé au wattmètre sur la prise secteur, mode `classic` (toutes LED allumées),
le 2026-08-07. **Cinq points, deux pentes** — et non plus une mesure unique
extrapolée, qui était le défaut de la version précédente.

**Pente par pixel**, à 100 % :

| pixels | mesure |
|---|---|
| 10 | 2,1 W |
| 20 | 3,0 W |
| 40 | 5,9 W |

Régression : **0,129 W par pixel**, socle **0,65 W**, R² = 0,989.
Soit **25,9 mA par pixel** à 5 V.

⚠ **Correction.** La version précédente annonçait 18,2 mA par pixel, déduits
d'un point unique. Trois points en donnent **25,9**, soit **40 % de plus**. Tout
ce qui en découlait était optimiste d'autant.

Le **socle de 0,65 W** sort de la régression sans avoir rien eu à débrancher :
il couvre l'électronique du mur *et* les pertes du bloc. Les 1,1 W relevés à
vide étaient donc surtout des pertes du bloc à charge nulle, qui s'effondrent
dès qu'il débite.

**Ce que tire un mur, à fond :**

| pixels | à la prise | continu |
|---|---|---|
| 42 (mur de référence) | 6,1 W | 1,03 A |
| **50 (cible produit)** | **7,1 W** | **1,21 A** |
| 100 | 13,6 W | 2,31 A |
| 150 (`LED_MAX`) | 20,0 W | **3,41 A** |

⚠ **Un mur de 150 pixels dépasse les 3 A d'un chargeur USB-C.** C'est
`LED_POWER_BUDGET_MA` qui l'en empêche : il assombrit la trame plutôt que de
laisser le chargeur replier.

**La luminosité n'est pas proportionnelle :**

| réglage | LED seules | part du maximum |
|---|---|---|
| 25 % | 1,25 W | **24 %** |
| 50 % | 1,45 W | **28 %** |
| 100 % | 5,25 W | 100 % |

Passer de 25 à 50 % ne coûte que **4 %** de puissance en plus : il y a une courbe
de gamma entre le réglage et la puissance émise. Un client qui baisse un peu la
luminosité divise sa consommation par près de quatre, et les chiffres « à fond »
ci-dessus sont un pire cas que personne n'habite au quotidien.

### Recoupement à trois voies

Les mesures au wattmètre, le datasheet et le modèle firmware disent-ils la même
chose ? Vérifié le 2026-08-07, et oui — une fois le modèle corrigé.

Le datasheet SK6812MINI (Normand, rév. 01, §11, conditions d'essai) donne :

| | |
|---|---|
| `Iout R/G/B` | **9 mA** par canal couleur |
| `Iout W` | **18 mA** — le double |
| `IDD` | 1 mA, statique, par contrôleur |
| `VIH` | **0,7 × VDD** — la contrainte de niveau, confirmée à la source |

En mode `classic` seul le canal W est piloté. Le datasheet prédit donc, pour
40 pixels : `40 × 18 + 40 × 1 = 760 mA`. La mesure donne 5,25 W de LED à la
prise, soit **0,76 A continu pour un rendement de bloc de 72 %** — plausible
pour un petit bloc chargé à 15 % de sa puissance.

**Les deux sources concordent**, et l'écart restant est exactement le rendement
du bloc, qui n'a jamais été mesuré.

⚠ **Le modèle firmware a été corrigé en conséquence.** Il multipliait les quatre
canaux par une constante unique — d'abord 17,5 mA, puis 25 mA calés sur une
seule mesure. Il compte désormais **9 mA pour R, G et B, 18 mA pour W**, comme
le composant. L'ancien modèle surestimait les couleurs du double et se trompait
de pire cas.

### Décision retenue

- **Entrée USB-C 5 V directe**, deux résistances de 5,1 kΩ sur CC1/CC2. Ni
  négociation PD, ni convertisseur pour le rail LED.
- **`LED_POWER_BUDGET_MA` = 1900** : le garde-fou est logiciel. Le firmware
  assombrit la trame entière plutôt que de dépasser, donc la carte ne peut pas
  tirer plus que l'USB-C n'accepte, quoi que fasse le client. La valeur est
  calée sous le seuil de la protection de sortie (§E).
- **Fusible dimensionné 5 A**, pas 3 : le surcoût est nul, et il protège le chaînage (voir la section routage). ⚠ **Le cuivre, lui, n'a PAS pu suivre** : les 2,80 mm calculés ne rentraient pas sur 100 × 25 mm en 2 couches. Le rail est routé en 0,40 mm et le courant passe par des zones de cuivre — détail et mesures dans la section routage.
- **Pas de CH224K, pas d'abaisseur, pas d'empreinte réservée.** Abandonné : la
  consommation mesurée ne le justifie pas, et une empreinte non peuplée sur une
  carte de série est un coût de conception et une source d'erreur au montage
  pour un cas qui n'arrivera peut-être jamais. Si un mur de plus de 110 pixels
  devient un vrai besoin, ce sera une révision de carte assumée.

Largeurs de piste, IPC-2221, cuivre 1 oz en couche externe, +10 °C :

| Courant | Largeur |
|---|---|
| 3 A | 1,4 mm |
| **5 A** | **2,8 mm** ← à router |
| 9 A | 6,3 mm |

### Chaînage — et pourquoi on reste à 3 A

Sur la base des mesures ci-dessus, pour le mur type visé (**50 pixels**) :
**7,1 W à fond**, soit **1,21 A**.

| Chargeur | Murs à fond |
|---|---|
| **USB-C nu, 3 A (15 W)** ← retenu | **2** |
| PD 5 V / 5 A (25 W) | 3 |

**Décision : on reste à 3 A, sans négociation PD.** Le gain serait d'un seul mur
de plus à pleine luminosité, contre trois contraintes lourdes : un contrôleur de
négociation à bord (sans lui la carte obtient 3 A quoi qu'on branche), un câble
e-marqué (un cordon ordinaire est plafonné à 3 A), et une alimentation d'un type
rare — l'offre 5 V / 5 A est *optionnelle* en PD et pratiquement personne ne
l'utilise à part le Raspberry Pi 5.

Le mode de panne tranche : si l'alimentation ou le câble ne suit pas, le mur
**fonctionne quand même**, en silence, à 3 A, et personne ne comprend pourquoi il
s'assombrit. Deux chargeurs coûtent moins qu'un retour.

Et le pire cas « tous à fond » n'existe guère : à 50 % de luminosité un mur ne
consomme que **28 %** de son maximum, donc un même chargeur en tient bien
davantage en usage réel.

Cuivre et fusible restent **dimensionnés 5 A** : la variante vitrine (repeupler
un CH224K, `C970725`) ne demanderait pas de revoir le routage.

**Le garde-fou du chaînage reste logiciel** : `/api/link?shared=1` divise le
plafond de chaque mur par le nombre de murs vus.

### Protection : ni fusible, ni autoréarmable — un limiteur de courant

Le raisonnement compte plus que la conclusion, parce qu'il se reposera à chaque
changement d'alimentation.

**Un fusible d'entrée ne sert à rien ici.** Un chargeur USB-C limite déjà son
courant, c'est dans la spécification. Le besoin maximal est de 2,92 A et la
source plafonne à 3 A : il n'y a pas de place entre les deux pour un fusible.
Trop bas il saute en usage normal, à 5 A le chargeur se coupe bien avant lui.

**Un PTC autoréarmable en sortie ne marche pas davantage**, et l'arithmétique
est nette : un PTC se déclenche vers **deux fois** son courant de maintien. Pour
ne pas sauter à 2,4 A, avec le déclassement thermique derrière un plateau (40 à
50 °C), il lui faut un maintien d'environ 3,3 A, donc un déclenchement vers
**6,6 A** — que la source ne peut pas fournir. Il ne se déclencherait jamais. Il
n'apporterait qu'une résistance série, qui de surcroît **augmente à chaque
déclenchement** et ne revient jamais à sa valeur d'origine.

**Ce qui fonctionne : un interrupteur à limitation de courant réglable**, sur la
sortie vers le plateau — le seul endroit où passe du câblage fait à la main,
avec un bornier et des fils qu'on manipule en accrochant la pièce.

| | |
|---|---|
| Composant | **AP22652W6-7** (Diodes) — LCSC **C2158038**, SOT-26. Vérifié le 2026-08-09 : 2 514 en stock chez JLCPCB (2 510 chez LCSC — pools distincts), Extended, 0,31 $/1 → 0,24 $/100 |
| Pourquoi pas l'AP2552 | l'AP2552W6-7 porte en **page 1 de sa propre fiche** le bandeau *« NOT RECOMMENDED FOR NEW DESIGN / USE AP22652/AP22653 »*, et la page produit Diodes le classe **NRND** dans l'*Inactive Datasheet Archive*. Il reste en stock et fonctionnel : c'est un choix de pérennité, pas une urgence. Aucun PCN d'obsolescence, donc pas de date de dernier achat publiée |
| Ce qui change | **rien au câblage** : même SOT-26, même brochage, même enable actif bas, même `FAULT` à drain ouvert, même 2,1 A continu. **Seule R4 change** |
| Ligne de la fiche | **2 398 / 2 665 / 2 931 mA à RLIM = 10 kΩ** sur −40 à +85 °C (DS41186 Rev. 5-2, mars 2026) — à résistance égale, **12,7 % plus haut** que l'AP2552 (2 200 / 2 365 / 2 542) |
| R4 retenu | **11 kΩ 1 %** (C25950) → **2 174 / 2 416 / 2 657 mA** sur −40 à +85 °C |
| Marge basse | plafond firmware ramené à **1,9 A** : le pire cas bas (2,174 A) est **14 % au-dessus** |
| Plafond | pire cas haut 2,657 A + **95 mA** de carte (socle mesuré 0,65 W) = **2,75 A**, soit 8 % **sous** les 3 A du chargeur |
| Court-circuit franc | **700 mA typ** (contre 2 620 mA sur l'AP2552) — le repliement est 3,7 fois plus dur, le chargeur ne le voit presque pas |
| ⚠ Enable | **ACTIF BAS** : `EN` se câble **à la masse**. Relié à IN — le réflexe habituel — le limiteur reste désactivé et la sortie LED est morte en permanence. L'erreur était sur le schéma v0.1, attrapée en vérifiant la fiche. |
| Comportement | limitation à courant constant, `FAULT` après **6 ms typ**, réarmement automatique |

Il se déclenche donc **sous** les 3 A du chargeur, ce qu'aucun fusible ni PTC ne
sait faire dans cette fenêtre.

⚠ **11 kΩ n'est pas une ligne tabulée.** La fiche ne garantit que 10, 15, 20,
49,9 et 210 kΩ. Le typ vient de son équation *best-fit*
`ILIMIT_Typ[mA] = 30321 / R[kΩ]^1,055`, et la fenêtre des **±10 %** qui
reproduisent **exactement** les quatre lignes tabulées sur −40 à +85 °C. Entre
10 et 20 kΩ l'équation colle à **0,4 %** près (vérifié : 2 671 contre 2 665 à
10 k, 1 742 contre 1 735 à 15 k) — elle dérive en revanche de 14 % à 210 kΩ, donc
elle ne vaut que dans cette plage.

**Pourquoi ne pas simplement garder 10 kΩ.** Parce que le pire cas y monterait à
2 931 mA, soit **3,03 A** avec la carte : au-dessus du chargeur. Le bloc perdrait
précisément ce qui justifie son existence — se déclencher **avant** la source.
L'inverse, 12 kΩ, ferait tomber le pire cas bas à 1 983 mA, sous la charge :
le mur afficherait « DÉFAUT SORTIE » sur une trame blanche légitime. La fenêtre
utile est étroite, 11 kΩ est le seul point qui tienne des deux côtés.

⚠ **Coût.** Le 10 kΩ était *Basic* chez JLCPCB, le 11 kΩ est *Extended* : une
redevance de chargeur en plus. C25804 reste malgré tout au BOM pour R5 et R10 —
**qui ne suivent pas R4** et restent à 10 kΩ.

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

**Sortie de chaînage.** Un second USB-C présentant 5 V est légitime à condition
de porter les résistances **Rp** côté source (et non Rd) — un réceptacle qui
sort de la tension sans se déclarer est hors spécification. ⚠ Ne **jamais** y
présenter 20 V : un téléphone branché dessus attend 5 V tant qu'il n'a pas
négocié, et n'y survit pas. Si la variante PD est un jour peuplée, la sortie de
chaînage doit passer sur un connecteur détrompé, pas sur un USB-C.

### B — Rail 3,3 V

**Référence retenue : SY8089A1AAC** (Silergy) — LCSC **C479074**, SOT-23-5,
*Extended* chez JLCPCB. Vérifié le 2026-08-07 : entrée **2,5–5,5 V**, **2 A**,
synchrone, 1,5 MHz, sortie ajustable par diviseur (`VOUT = 0,6 × (1 + RH/RL)` —
formule du datasheet). Pour 3,3 V : **RH 68 k / RL 15 k** → 3,32 V. Self 2,2 µH
(saturation ≥ 2,5 A), 22 µF de part et d'autre.

Pourquoi celui-là et pas le MT2492 (C89358, moitié moins cher) : le MT2492
démarre à **4,5 V d'entrée minimum**. Un rail USB-C nominal 5 V descend sous
cette barre avec un câble médiocre sous charge — le convertisseur décrocherait
précisément quand le mur tire le plus. La plage 2,5–5,5 V du SY8089 régule
jusqu'au fond de l'affaissement. Second choix à specs égales : TLV62569DBVR
(TI, C141836), même plage, même courant — **brochage à vérifier avant toute
substitution**, les SOT-23-5 de bucks ne sont pas interchangeables.

Les deux candidats sont *Extended* : aucun avantage de forfait à espérer, la
carte en paie déjà (le module ESP32 est *Extended* aussi).

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
| Adaptateur de niveau | **SN74AHCT1G125DCKR**, **SC-70-5** (le suffixe DCKR = SC-70 ; DBV = SOT-23), alimenté en 5 V | voir ci-dessous |
| Résistance série | 330 Ω, en sortie de l'adaptateur | amortit le front, réduit le rayonnement |
| Connecteur de sortie | **bornier à ressort KF250NH-3.5-3P**, 3 pôles : `+5V_LED`, `GND`, `DATA` | un seul organe suffit — voir ci-dessous |

⚠ **Correction du 2026-08-10.** Ces deux lignes annonçaient auparavant un JST-XH
pour les données **et** un bornier à vis séparé pour la puissance, au motif que
« 9 A ne passent pas dans un JST-XH ». Les 9 A ne concernent pas cette carte :
son plafond est **2,1 A**, imposé matériellement par le limiteur U5, et le budget
logiciel est à 1,9 A. Un seul bornier 3 pôles porte donc l'ensemble, avec une
marge confortable même sur la plus pessimiste des deux valeurs contradictoires
annoncées pour ce bornier (8 A chez le fabricant, 15 A chez le distributeur).

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
| ~~Encodeur~~ | **abandonné** — GPIO4 porte désormais `LED_FAULT` de U5 ; y remettre une empreinte EC11 mettrait un contact en parallèle d'une sortie à drain ouvert et rendrait la détection de court-circuit aveugle |
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

## 4bis. Références — nomenclature complète

Toute la nomenclature est dans [BOM_PCB.csv](BOM_PCB.csv), 31 lignes, chacune
confirmée sur sa page LCSC/JLCPCB le 2026-08-07. Ce qui suit n'est que ce qui
ne se lit pas dans un tableau.

### Le piège qui invalide une vérification faite depuis LCSC

**lcsc.com (vente au détail) et jlcpcb.com (bibliothèque d'assemblage) ont des
stocks SÉPARÉS.** Une pièce peut afficher « Out of Stock » sur l'un et des
millions d'unités sur l'autre :

| Référence | lcsc.com | jlcpcb.com |
|---|---|---|
| C25804 (10 kΩ) | Out of Stock | **3 423 487** |
| C23186 (5,1 kΩ) | Out of Stock | **6 074 400** |
| C23138 (330 Ω) | 0 In Stock | **2 350 491** |
| C15849 (1 µF) | Out of Stock | **15 642 436** |

Pour une carte assemblée chez JLCPCB, c'est le stock jlcpcb.com qui compte.
Juger la disponibilité depuis une page LCSC produit des faux négatifs — et
c'est probablement ce qui a condamné à tort les trois premiers candidats micro.

### Ce qui n'existe pas en Basic (vérifié, pas supposé)

- **Aucune inductance de puissance**, toutes valeurs confondues : la case à
  cocher « Basic » est désactivée dans la catégorie. L1 sera Extended quoi qu'il
  arrive.
- **Aucun encodeur rotatif.**
- **Aucune embase femelle.**
- **Un seul poussoir haut** — et il est plat (1,5 mm), donc inutilisable pour
  une façade. S1–S4 seront Extended ; autant prendre le bon.

Les **six valeurs de résistance** et les **quatre valeurs de condensateur**
sont toutes disponibles en Basic, de la même famille pour les résistances
(UNI-ROYAL 0603WAF) : une empreinte, un fabricant, aucun frais de mise en place.

### Les trois pièges relevés en vérifiant

**Isat n'est pas Irms.** Sur `C135268`, LCSC et JLCPCB affichent ces deux
colonnes **inversées** l'une par rapport à l'autre. La `C167869` retenue est
cohérente entre les deux sites, ce qui est une raison de plus de la préférer.
Le piège classique reste de regarder l'Isat et d'oublier l'Irms : la `C50543`
tient 3,2 A de saturation mais seulement **1,65 A en continu**, sous nos 2 A.

**Le 22 µF du buck perd sa capacité sous tension.** Un X5R 0805 s'effondre par
polarisation continue. Le hasard aide : la seule 22 µF 0805 *Basic* du catalogue
est justement une **25 V** (`C45783`), donc la moins affectée.

**Le « TS-1187A » n'a pas de tige.** La référence citée partout pour ce montage
fait 1,5 mm de haut — un bouton rase-carte. La hauteur affichée par LCSC est
celle du poussoir entier, pas de l'actionneur. D'où la `C480275`, 7,5 mm, la
seule qui accepte un capuchon ou un perçage de façade.

### Réserve honnête sur MIC1

`C47148419` est confirmé en stock **chez LCSC** (1 050 pièces). Sa présence dans
la **bibliothèque d'assemblage JLCPCB n'a pas pu être confirmée** — la fiche ne
rend rien d'exploitable. À valider avant de commander : si elle en est absente,
il faudra fournir la pièce ou la souder à part.

### Empreintes à graver

- **ENC1** : standard **ALPS EC11E** (KiCad `RotaryEncoder_Alps_EC11E-Switch_Vertical_H20mm`),
  recoupé sur deux plans constructeur. Ce n'est pas une empreinte propriétaire :
  n'importe quel EC11 traversant à poussoir s'y soude, aujourd'hui et dans dix ans.
- **L1** : NR4030 standard, 4,0 × 4,0 × 3,0 mm.
- **J2** : **bornier à ressort KF250NH-3.5-3P** (LCSC `C976557`) depuis le
  2026-08-10, à la place du JST XH. Pas **3,50 mm**, perçage **1,50 mm**,
  pastille Ø 2,30. Corps **11,10 × 10,00 mm** et **12,32 mm de haut** — cotes
  relevées sur le modèle 3D, pas estimées. Le corps est asymétrique autour de la
  rangée de broches (4,30 mm d'un côté, **5,70 mm** de l'autre) : c'est le côté
  5,70 qui porte l'entrée du fil et il doit regarder le **bord bas**, côté
  poussoirs — donc pose à **0°**, alors que le JST était à 180°.
  ✅ **Les 12,32 mm passent** — vérifié visuellement sur la machine par le
  propriétaire le 2026-08-10. Niveau de preuve : contrôle à l'œil, pas une cote
  relevée. À refaire au pied à coulisse si un boîtier ou un capot s'ajoute.

Abandonné en cours de route : **CH224K (C970725)**, contrôleur PD. La référence
est bonne, le besoin ne l'est pas — voir §3bis.

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

## Routage — décisions et mesures du 2026-08-10

Carte **100 × 25 mm**, 2 couches, 63 empreintes. Routée par Freerouting 2.3.0
après huit tours, dont voici ce qui a compté.

**Les largeurs de piste du dossier ne rentraient pas.** Les 2,80 mm calculés pour
5 A rendaient le routage impossible : à 1,5 mm par net de puissance, il restait
43 liaisons non routées sur 85. Le rail a été routé en **0,40 mm** et le courant
est porté par des **zones de cuivre** :

| zone | couche | étendue |
|---|---|---|
| +5V entrée | F.Cu | X 120,6 → 167,5 |
| +5V consommateurs | F.Cu | X 187,5 → 219,4 |
| GND | B.Cu | toute la carte |

**17 des 18 pastilles +5V sont dans une zone.** Sans elles, 100 mm de piste de
0,24 mm à 2,1 A donnent **0,43 V de chute et 0,9 W dissipés** — mesuré, pas estimé.
Seule `U2.5` reste hors zone : c'est une référence de tension, elle ne consomme rien.

**Ce qui a débloqué le routage**, dans l'ordre de gain :

| changement | non routés |
|---|---|
| départ, POWER à 1,5 mm | 43 |
| R1, R2 et U2 recollés à J1 (ils en étaient à 19, 24 et 14 mm) | 39 |
| POWER à 0,40 mm | 13 |
| signaux à 0,15 mm — c'est ce qui fait sortir le TDFN de U3 | 6 |
| trous de montage posés, routage repris de zéro | **2** |

⚠ **Freerouting descend sous le minimum si on le laisse faire** : 22 segments
étaient à **0,112 mm**, sous les 0,15 mm de la contrainte et sous le minimum
JLCPCB. Repris à 0,150 mm. À revérifier après chaque tour d'autoroutage.

**Trous de montage** : H1 en (130,5 ; −90,0), H2 en (200,5 ; −84,0), **Ø3,2 mm
non métallisés**, pour vis à bois de 3 mm. Dégagement de tête vérifié : 3,55 et
4,08 mm au composant le plus proche, pour 3,20 requis. S6 a été déplacé pour H2.

**Le fusible F1 reste justifié**, contrairement à ce qu'un calcul rapide laisse
croire. Pour un seul mur sur un chargeur de 3 A il ne peut effectivement jamais
fondre — la source replie avant. Mais **J4 est alimenté APRÈS F1** : avec une
alimentation de 5 V / 6 A et quatre murs chaînés, jusqu'à 8 A traversent J1, qui
n'est donné que pour 5 A. C'est là que F1 sert, et c'est le seul cas.

---

## Antenne vers le bas — ÉTUDIÉ ET REJETÉ le 2026-08-10

Question posée : tourner le module U1 de 180° pour que son antenne pointe vers le
bord bas au lieu du bord haut. **Réponse : non**, et la raison n'est pas un
principe mais une série de mesures prises dans le `.kicad_pcb`.

**Le problème supposé n'existe pas.** H1 (130,50 ; −90,00) et H2 (200,50 ; −84,00)
sont deux M3 **non métallisés intérieurs**, à mi-hauteur de carte. La carte est
donc boulonnée **à plat, dos contre le bois** : les deux bords, haut et bas,
débordent dans le vide. L'antenne n'est pas plaquée contre le plateau — le bois
est **derrière** elle, pas devant. Et le placement actuel (antenne débordant de
6,04 mm hors carte, première pastille à 1,007 mm du bord) est déjà exactement la
recommandation principale d'Espressif.

**La rotation rapprocherait du bois au lieu d'en éloigner.** H1/H2 sont décalés de
0,50 mm sous la mi-hauteur : à 12,7 mm d'épaisseur de tranche, l'écart
antenne↔bois passe de 5,643 à 5,351 mm, soit **−0,292 mm**.

**Le gain RF est nul.** Le chiffre de « 7 à 9,6 dB » qui circule vient de
l'option 6 du guide ESP-WROOM-02 (cuivre sous l'antenne + placement central), qui
ne décrit pas ce cas. L'analogue correct pour du diélectrique sans conducteur est
l'option 4 : **−1,03 / −1,01 / +0,06 dB** selon le canal. Rien.

**Le bord bas est le pire endroit de la carte pour une antenne** : `/+5V_LED`
court sur B.Cu à Y = −77,12, soit 2,12 mm du bord, en travers de toute la largeur
du module, à 2,1 A commutés. S'y ajoutent la sortie du câble d'un mètre, les
poussoirs à 2,65 mm, et **la main de l'utilisateur à chaque appui** — un désaccord
intermittent est un mode de panne pire que la perte statique qu'on cherchait.

**Et le coût est massif** : U1 concentre 39 nets sur 49 pastilles ; la rotation
invalide 250 à 510 segments (31 à 65 % du cuivre) et détruit la seule paire
différentielle propre de la carte (USB, 6 segments par piste, aucun via).

⚠ Enfin, la descente de 5,11 mm qui rendait la géométrie possible est **interdite
par les règles du projet lui-même** : `min_copper_edge_clearance = 0,30 mm` en
sévérité *error*. Le plafond dur est 5,01 mm.

**Si la question revient**, la voie n'est pas la rotation mais la mécanique :
entretoises plus hautes sur H1/H2, décalage de la carte dans son perçage, ou
dégagement local du bois derrière X 168..187 / Y −100..−94 (≈115 mm², aucun
composant). Et le seul chemin vers les 15 mm de dégagement recommandés serait le
**WROOM-1U** (19,2 × 18 × 3,2 mm, qui tient entièrement dans les 25 mm) — autre
projet, à n'ouvrir que si une mesure RSSI/débit prouve un vrai problème.

## Bornier déplacé à l'extrémité droite — FAIT le 2026-08-10

J2 était le seul organe du bloc LED resté à gauche (X 157,77) alors que U5 (207),
U6 (218) et R3 sont dans la colonne droite. Conséquence mesurée : `+5V_LED`
faisait **66,16 mm en 0,40 mm de large**, soit ~70 mΩ, **147 mV et 0,33 W perdus
à 2,1 A**.

| | avant | après |
|---|---|---|
| J2 | (157,77 ; −77,53) à 180° | **(214,30 ; −80,70) à 0°** |
| S1 / S2 / S3 | 192,04 / 200,04 / 208,04 | **−2,30 mm**, entraxe 8,00 conservé |
| C26 | (170,31 ; −78,03) | **(214,26 ; −86,00)** — il découple le **connecteur** |

Jeux obtenus : U1↔S1 **0,35** · S3↔bornier **0,44** · bornier↔bord **0,40 mm**.
⚠ La note d'analyse proposait −2,80 mm : **ça fait entrer S1 dans le module de
0,135 mm**. Le décalage juste est −2,30 mm, il ne reste que 1,19 mm de jeu total à
répartir sur trois intervalles.

Perçages du bornier vérifiés côté fabricant : anneau annulaire 0,40 mm (mini
0,20), trou-à-trou 2,00 mm (mini 0,45), pastille au bord 1,05 mm (mini 0,30).

**Netlist strictement inchangée** — 234 pastilles, aucune n'a changé de net.

## Dette de routage découverte le 2026-08-10

Le dossier annonçait « 2 liaisons non routées ». **C'est faux** : il y avait
**53 éléments non connectés sur 13 nets**, et surtout **12 courts-circuits réels**,
dont plusieurs antérieurs au déplacement du bornier :

- deux segments de 40,10 et 36,80 mm (`Net-(U1-IO19)`, `Net-(U1-IO20)`) traversant
  la pastille 1 de R15 — rebuts de la réorganisation de la section micro ;
- le moignon `/STATUS_PX` pointant vers l'est, qui traversait la poche où le
  bornier devait aller ;
- une piste GND sur la pastille 1 de S6 (`/EN_MCU`).

Supprimés le 2026-08-10 : **12 objets, 96 mm de cuivre**. Résultat DRC :
courts-circuits **12 → 0**, trou-à-trou **2 → 0**, ponts de masque **45 → 2**,
total **207 → 153**. Les 89 restants sont de la sérigraphie et n'empêchent rien.

⚠ **Six violations subsistent autour de J2 uniquement parce que les zones n'ont
pas été re-remplies** depuis le déplacement : le polygone stocké garde son trou à
l'ancienne position. `kicad-cli` **ne sait pas remplir les zones** — c'est
`Édition → Remplir toutes les zones` (touche `B`) dans pcbnew.

## Routage terminé — 2026-08-10, 15:16

Freerouting 2.3.0 sur le DSN patché (plans retirés, câblage remis à zéro,
`POWER` ramenée de 1500 à **400 µm**, `USB` 300 → 200, signaux 200 → **150**) :
**170 liaisons non routées → 1** en 53,8 s. La dernière, `/+3V3` entre les
pastilles 2 et 5 de U3, a été faite au **routeur interactif** — aucun autorouteur
ne sort un fanout TDFN au pas de 0,40 mm.

⚠ Piège confirmé une seconde fois : Freerouting **descend sous la largeur
minimale** — 28 segments à 0,1124 mm pour une contrainte à 0,150. Remontés à la
main. Vérifier après **chaque** tour.

⚠ Piège de la classe de nets : `/+3V3` appartient à `POWER`, dont la largeur est
**1,50 mm**. Le routeur interactif la propose par défaut, sur une pastille de
0,22 mm. Il faut saisir **0,15 mm** à la main.

⚠ Ordre des opérations : remplir les zones **APRÈS** le routage, jamais avant. Une
via créée après le remplissage n'a pas son dégagement dans le polygone déjà
calculé — ça a produit ici une via `/AGC_TH` **à 0,0000 mm de la zone GND**, donc
un court-circuit franc du seuil de la CAG. Un second `B` l'a effacé.

**État final vérifié :**

| | |
|---|---|
| Liaisons non connectées | **0** |
| Netlist PCB ↔ schéma | **identiques, 234 pastilles** |
| Cuivre | 877 segments, **1 897 mm**, 93 vias, 9 zones remplies |
| Largeur minimale | **0,150 mm** |
| DRC | 104, dont **89 de sérigraphie** → 15 de fond |

Les 15 restantes n'ont **aucun effet électrique** : 6 anneaux annulaires + 6
padstacks sur les pastilles **mécaniques sans net** de J1, J4 et SW1 (les ancrages
des connecteurs, déjà arbitrés) ; 2 sur H1/H2 dont l'empreinte `MountingHole_3.2mm`
n'est pas dans la table de bibliothèques (elle est embarquée dans la carte) ; 1 sur
J2 dont le bloc a été reconstruit à la main — `Mettre à jour les empreintes depuis
la bibliothèque` la lève.

**`+5V_LED`, le rail qui porte 2,1 A**, mesuré de bout en bout :

| étape | longueur | résistance | chute à 2,1 A |
|---|---|---|---|
| avant | 66,16 mm en 0,40 | 82,7 mΩ | 174 mV · 0,36 W |
| bornier déplacé à droite | 18,34 mm en 0,40 | 22,9 mΩ | 48 mV · 0,10 W |
| **élargissement progressif** | **15,9 mm** | **16,3 mΩ** | **34 mV · 0,07 W** |

L'élargissement est **variable et non uniforme** : 1,50 mm sur les portions
dégagées, 0,40 au ras des broches de U5 où `+5V`, `ILIM` et `LED_FAULT` ne laissent
que 0,10 à 0,43 mm de voisinage. Un premier essai à 1,50 mm partout avait créé
**8 courts-circuits** ; la largeur est désormais calculée segment par segment
contre les 934 objets de cuivre voisins.

## Reprise de l'alimentation — FAIT le 2026-08-10

Option 1 appliquée, mais **deux spécifications de la note d'analyse se sont
révélées inapplicables à la mesure**, et ont été remplacées par mieux.

**C1 → non monté** (`dnp yes`, hors BOM) plutôt que supprimé : l'emplacement
reste sur la carte, ce qui garde l'option ouverte sans coûter un re-routage.
**C2 : 470 µF 10 V → 100 µF 25 V** — `VZH101M1ETR-0607L`, LCSC `C473422`.
Remplacement **à l'identique vérifié** : pastilles à −2,67 et +2,67, taille
3,50 × 1,20, même empreinte `-FD` que le sortant. Aucune modification du PCB.

⚠ **Piège évité de justesse.** Le premier candidat, `VZT101M1VTR-0607`
(`C249983`), est en boîtier **`-RD`** et non `-FD` : sa broche 1 est de l'autre
côté (+2,55 au lieu de −2,67). Le condensateur aurait été monté **à l'envers**.
Le suffixe de boîtier LCSC encode la polarité — toujours comparer les
coordonnées de pastilles avant de déclarer un remplacement compatible.

Le rail VBUS passe de ~984 µF à ~144 µF nominal. **À assumer par écrit : on reste
à ~13× la limite USB 2.0 §7.2.4.1.** La conformité est hors d'atteinte sur cette
topologie — seul un limiteur unique en amont de U4 **et** U5 **et** J4 la
donnerait. Le gain réel est sur le pic d'enfichage : de 14-36 A pendant ~300 µs à
~2,4 A pendant ~1 ms.

**C27 = 1 µF 0603** (`C15849`, déjà au BOM) contre la broche 6 (OUT) de U5. Il
**manquait** : le datasheet AP22652 exige 0,1 à 4,7 µF céramique au plus près de
OUT, et la céramique la plus proche était C26 à **38,8 mm mesurés**.

**C28 à C31 = 4 × 22 µF 0805** (`C45783`) répartis le long du rail `+5V_LED`.
Ceci **remplace** la spécification « 100 µF polymère 7343 », impossible pour deux
raisons mesurées : aucun emplacement 7343 libre sur la carte routée (zéro
position), et **il n'existe pas de 100 µF polymère en 7343** — les seules options
dans ce boîtier sont des tantales dont l'ESR de l'ordre de l'ohm ferait 2 V de
chute à 2,1 A. Quatre MLCC donnent quelques milliohms, ~55 µF réels après
derating, et **aucune ligne d'achat en plus**.

**État vérifié après pose :** 244 pastilles, netlist PCB ↔ schéma **identiques**,
885 segments, 98 vias, **0 liaison non connectée, 0 défaut électrique**.
L'augmentation du DRC (104 → 179) est **entièrement de la sérigraphie** :
les cinq nouvelles empreintes portent leur texte dans une zone dense.

⚠ Trois pièges de placement automatique rencontrés et corrigés — ils valent pour
toute pose calculée :
1. **minimiser la distance au rail place le composant DESSUS** : sa pastille GND
   se retrouve sous la piste `+5V_LED` ;
2. **un solveur qui place un composant à la fois ne voit pas le cuivre qu'il
   vient d'ajouter** — le suivant tombe sur la via du précédent ;
3. **approximer une pastille rectangulaire par son plus grand demi-côté
   sous-estime les coins de 0,27 mm** : prendre le rayon circonscrit.

## Nettoyage des empreintes — 2026-08-10

**179 → 50 violations DRC**, et un vrai défaut trouvé au passage.

⚠ **Le défaut réel : les cinq condensateurs ajoutés portaient un `path` inventé**
au lieu de l'UUID de leur symbole. Le champ `(path ...)` d'une empreinte est le
lien vers son symbole au schéma. Avec un mauvais UUID, un « Mettre à jour le PCB
depuis le schéma » ne les reconnaît pas : il les recrée ailleurs et **perd leur
placement**. Corrigé, les 5 liens pointent maintenant sur leur symbole.

**H1/H2** : l'empreinte s'appelle **`MountingHole_3.2mm_M3`** dans KiCad 10 — le
nom sans suffixe n'existe plus. Référence corrigée et bibliothèque
`MountingHole` ajoutée à la table du projet. `lib_footprint_issues` 2 → **0**.

**Sérigraphie, 159 → 24.** Cause racine : **toutes** les empreintes de ce projet
portent leur désignateur à **4 mm** du composant, héritage de la conversion
easyeda2kicad. Sur une carte de 25 mm de haut, ça le projette systématiquement
sur le voisin. Traitement en trois temps :

| | |
|---|---|
| 46 désignateurs de passifs (C, R, L) | F.SilkS → **F.Fab** — présents dans les données, pas imprimés |
| 100 primitives de contour sur 11 empreintes serrées | retirées : sous un 0805 posé à 0,35 mm du voisin, le contour est invisible |
| J1 et J2 | désignateur ramené **dans** leur propre contour |
| H1 H2 S1 S5 U2 U5 | désignateur en F.Fab, trop petits pour le porter |

`silk_overlap` **103 → 0**, `silk_over_copper` **29 → 0**.

⚠ **Aucune information de polarité n'a été perdue** : les seules empreintes
polarisées touchées sont C1, qui est **non monté**. C2 conserve son contour.

### Ce qui reste, et pourquoi c'est normal

| | |
|---|---|
| **24** `silk_edge_clearance` | contours de J1, J4, S3, SW1 et de l'antenne de U1 qui **débordent volontairement** du bord. Voulu, pas un défaut. |
| **6 + 6** `annular_width` / `padstack` | pastilles **mécaniques sans net** des USB-C et de l'interrupteur. Préexistantes, arbitrées. |
| **14** `lib_footprint_mismatch` | **conséquence assumée du nettoyage** : ces empreintes diffèrent désormais de la bibliothèque, exprès. |

☠️ **NE PAS lancer « Mettre à jour les empreintes depuis la bibliothèque ».**
Cela restaurerait la sérigraphie d'origine et **annulerait tout le nettoyage**.
Les 14 avertissements de non-correspondance sont le prix à payer, et ils disent
la vérité. Si le rapport doit être vierge, régler la sévérité de
`lib_footprint_mismatch` sur « ignorer » dans les règles du projet — pas en
réalignant les empreintes.

**Vérifié après nettoyage : 244 pastilles, netlist PCB ↔ schéma identiques,
0 liaison non connectée, 0 défaut électrique.**

## Revue de mise en fabrication — NO-GO en l'état, 2026-08-10

### Le défaut qui aurait coûté la série

**Les modifications d'alimentation n'avaient jamais été propagées au PCB.** Le
schéma portait bien C1 en `dnp` et C2 en `VZH101M1ETR-0607L / C473422`, mais le
`.kicad_pcb` gardait C1 **monté** et C2 en `VZT471M1ATR-0607 / C384654`. Les
fichiers de production auraient donc commandé et posé **940 µF** — exactement ce
que la reprise visait à supprimer.

⚠ **Et la vérification-phare du dossier ne pouvait pas l'attraper.** « Netlist
PCB ↔ schéma identiques sur 244 pastilles » était vrai : le remplaçant avait été
choisi **exprès** pour avoir la même empreinte et les mêmes pastilles. Une
comparaison de netlist lit les connexions, jamais les champs `Value`, `LCSC Part`
ni `dnp`. **Après toute modification de schéma, contrôler les champs du PCB
séparément.**

Corrigé le 2026-08-10 : C1 → `(attr smd exclude_from_pos_files exclude_from_bom
dnp)`, C2 → `VZH101M1ETR-0607L / C473422`, U2 → champ `LCSC Part = C7519` qui
était **totalement absent** (colonne LCSC vide au BOM exporté), H1/H2 exclus du BOM.

### Défaut électrique réel : U6 valide en permanence, LED_DATA flottant

Mesuré : `/LED_DATA` n'a que **deux nœuds** (U1.9 = IO16 et U6.2), donc **aucun
tirage**, et `#OE` de U6 est câblé à la masse. U6 est alimenté en +5 V dès que
l'USB est branché, **indépendamment de SW1**.

Cas destructeur — SW1 sur arrêt, USB branché, c'est-à-dire l'état normal la nuit :
l'ESP est en reset, IO16 en haute impédance, l'entrée du tampon flotte, sa sortie
part au hasard et pousse 5 V à travers R3 dans la DIN d'une guirlande **non
alimentée**. Environ **13 mA en permanence** dans sa diode de protection.

⚠ Recâbler `#OE` sur `EN_LED` **ne suffit pas** : dans la fenêtre de démarrage,
`EN_LED` est bas, le tampon est validé, et l'entrée flotte quand même. Seul un
**tirage bas sur `/LED_DATA`** couvre les deux cas.

**Correctif retenu : strap sur les prototypes, R16 en rev. B.** Un placement de
0603 satisfaisant toutes les contraintes de cuivre a été cherché dans toute la
colonne X 210..219,3 / Y −97..−84 : **aucun emplacement ne passe**, la zone est
trop dense. Rerouter ce coin pour une résistance n'en vaut pas le prix.
La géométrie rend le strap trivial : **U6 pad 2 (`/LED_DATA`) et pad 3 (`GND`)
sont adjacents, à 0,65 mm**. Une 0402 de 10 kΩ posée à cheval sur ces deux
pastilles EST le tirage bas. Deux minutes par carte.

### ⚠ Nettoyage de sérigraphie PERDU — cause identifiée

Le nettoyage du 2026-08-10 (179 → 50 violations) a été **écrasé** : KiCad était
resté ouvert, et un enregistrement ultérieur a réécrit le fichier depuis sa copie
en mémoire, antérieure. Signe caractéristique : le fichier est passé de **588 Ko
à 1 085 Ko** — KiCad l'a re-sérialisé à sa façon. Les corrections de champs
postérieures, elles, ont survécu.

**Règle : tant que les verrous `~<projet>.kicad_*.lck` existent, toute écriture de
fichier est en sursis.** Le nettoyage est à refaire, KiCad fermé.

### État vérifié à cet instant

244 pastilles, netlist PCB ↔ schéma **identiques**, **0 défaut électrique**,
**0 liaison non connectée**. 182 violations DRC dont **159 de sérigraphie**
(le nettoyage étant à refaire).

## D2, le pixel de statut — écart assumé, 2026-08-10

**Le problème.** D2 (WS2812B-2020) est alimenté en **3,3 V** alors que la fiche
constructeur et la page JLCPCB donnent **5 V nominal, plage 3,5–5,3 V**. On est
0,2 V sous le minimum. La justification au dossier ne portait que sur le **niveau
logique** de la donnée, jamais sur la tension d'**alimentation**.

**Ce qui a été tenté, et pourquoi ça a échoué.** Alimenter D2 depuis +5V à travers
une diode 1N4148WS (`C2128`, Basic) donnerait VDD ≈ 4,3 V — dans la plage — et
abaisserait VIH à 3,0 V, satisfait par les 3,3 V d'IO48. Le schéma a été câblé,
la diode et un 100 nF posés. **Tout a été défait** : un routeur A\* sur grille de
0,1 mm a établi qu'**aucun chemin n'existe**. La pastille 4 de D2 est dans une
poche fermée, et le couloir entre la zone +5V et le bord bas est saturé sur toute
sa largeur — rail 3,3 V, `MICBIAS`, `AGC_BIAS`, `MICIN`, les trois anti-rebond des
poussoirs et la donnée LED le traversent tous. **Déplacer D2 ne change rien** :
178 positions libres ont été testées près du bord bas, il faut franchir la même
bande dans tous les cas.

**Décision : on garde 3,3 V sur cette série.** Contorsionner une carte qui
fonctionne pour un témoin de statut n'est pas un arbitrage raisonnable.

**Ce qu'il faut savoir à la réception :**

| | |
|---|---|
| Le test | D2 doit s'allumer aux bons moments et aux bonnes couleurs — orange au flash, cyan en diagnostic, vert au lien |
| Si un exemplaire refuse | un fil de quelques millimètres entre **+5V** et sa **broche 4**, avec une diode en l'air. Dix minutes, et seulement sur les cartes concernées |
| Rev. B | la vraie correction n'est **pas** la diode : c'est de placer D2 **contre la zone +5V dès le départ**, avant que le routage ne remplisse le couloir |

⚠ **La leçon de fond** : le problème vient de **l'ordre des opérations**, pas du
composant. Un composant qui a besoin d'un rail particulier doit être placé
**avant** le routage, pas après.

⚠ **Stock** : `C965555` a été signalé indisponible. N'importe quel WS2812B-2020 de
même empreinte le remplace — mais **vérifier son brochage** : le
XL-2020RGBC-2812B (`C5349955`), candidat évident, est en empreinte `-BR` au lieu
de `-TL`, brochage inversé, et mettrait `+3V3` sur la sortie de données.

## Consommation réelle, mesurée au wattmètre (2026-08-27)

Mesures du propriétaire **au secteur**, mur de 75 LED SK6812 RGBW :

| état | secteur | estimation firmware (`ma`) |
|---|---|---|
| all on, plein blanc | **10 W** | 1395 mA = 6,97 W en 5 V |
| attract, à fond | **7 W** | dépend des trois niveaux, ~380 mA aux réglages du jour |
| attract, un cran plus bas | **5 W** | |
| attract, deux crans | **3 W** | |

**L'estimateur est juste.** Les 3 W d'écart en « all on » ne sont pas une erreur :
en reconstruisant à 80 % de rendement d'alimentation, 10 W au secteur donnent 8,0 W
en 5 V, moins ~0,75 W pour l'ESP32 et l'OLED, soit **1450 mA pour les LED contre
1395 mA estimés — +4 %**. À 85 % de rendement l'écart monte à +11 %, ce qui borne
l'incertitude sans la contredire.

Conséquences chiffrées :

- Plein blanc = **73 % du budget** (1395 sur 1900 mA). Le limiteur logiciel n'a
  aucune raison d'intervenir en usage normal, et c'est cohérent avec `limited=false`
  observé en continu.
- Le budget couvre les **LED seules**. Le contrôleur tire 100–250 mA de la même
  alimentation et du même fusible sans jamais apparaître dans l'estimation :
  dimensionner le fusible sur **budget + 300 mA**, pas sur le budget.
- Une alimentation 5 V / 2 A suffit avec de la marge ; 10 W au secteur en pointe,
  soit moins de 3 W en usage courant (attract baissé).

Méthode : la valeur `ma` de `/api/state` est une estimation par sommation des canaux,
pas une mesure. Elle n'avait jamais été confrontée à un instrument avant ce jour ;
elle l'est maintenant, et elle tient.
