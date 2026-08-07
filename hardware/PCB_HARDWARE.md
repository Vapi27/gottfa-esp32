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

### La PSRAM : tranchée, par la mesure

`CONFIG_SPIRAM=y` venait de la configuration d'exemple esp-matter, pas d'un
besoin démontré. Testé sur la vraie carte le 2026-08-07, à source identique
(seul `CONFIG_SPIRAM` diffère), sous une séquence de contraintes identique —
tous les modes, la page complète, un scan WiFi, l'image du plateau :

| | avec PSRAM | sans PSRAM |
|---|---|---|
| Tas libre | 76 783 | 60 836 |
| **Minimum atteint sous charge** | **9 287** | **25 340** |
| Plus gros bloc contigu | 25 600 | 14 848 |

**La PSRAM dégrade la marge de RAM interne**, et le journal de démarrage dit
pourquoi : `esp_psram: Reserving pool of 32K of internal memory for DMA/internal
allocations`. Elle prélève 32 ko d'interne sur les 320 disponibles, au profit de
8 Mo dont cette application ne se sert pas.

Une réserve, la seule : le plus gros bloc contigu tombe de 25,6 à 14,8 ko. Une
allocation d'un seul tenant au-delà de 14,8 ko échouerait sans PSRAM. Le pic
d'appairage Matter n'a **pas** encore été mesuré — il demande de dépairer et
réappairer depuis un téléphone.

### Décision retenue

- **Entrée USB-C 5 V directe**, deux résistances de 5,1 kΩ sur CC1/CC2. Ni
  négociation PD, ni convertisseur pour le rail LED.
- **`LED_POWER_BUDGET_MA` = 2000** : le garde-fou est logiciel. Le firmware
  assombrit la trame entière plutôt que de dépasser, donc la carte ne peut pas
  tirer plus que l'USB-C n'accepte, quoi que fasse le client. La valeur est
  calée sous le seuil de la protection de sortie (§E).
- **Cuivre et fusible dimensionnés 5 A**, pas 3 : le surcoût est nul.
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
| Composant | **AP2552** (famille AP2552/AP2553), limitation réglable par résistance |
| Seuil | jusqu'à **2,36 A** typique, ±6 % → 2,22 à 2,50 A |
| Programmation | `RLIM`, résistance 1 %, 10 kΩ ≤ RLIM ≤ 210 kΩ |
| Marge | budget firmware à **2,0 A**, franchement sous le seuil bas de 2,22 A |
| Comportement | limitation à courant constant, drapeau de défaut, réarmement automatique |

Il se déclenche donc **sous** les 3 A du chargeur, ce qu'aucun fusible ni PTC ne
sait faire dans cette fenêtre. ⚠ Référence LCSC **non confirmée** — à vérifier
sur stock avant de figer la nomenclature.

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

## 4bis. Références — état de vérification

Vérifiées sur les pages LCSC / JLCPCB le 2026-08-07. Les prix sont des « à partir
de » : à confirmer dans le panier à la quantité réellement commandée.

| Rôle | Référence | LCSC | État |
|---|---|---|---|
| Module | ESP32-S3-WROOM-1-N16R8 | **C2913202** | ✅ vérifié — 25,5 × 18 × 3,1 mm, *Extended*, ~3,78 $ |
| Module, sans PSRAM | ESP32-S3-WROOM-1-N16 | **C2913199** | ✅ vérifié — ~3,75 $, 1 146 en stock. Non retenu : voir §1 |
| Module, antenne externe | ESP32-S3-WROOM-1U-N16R8 | **C3013946** | ✅ vérifié |
| Protection ESD USB | USBLC6-2SC6 (ST) | **C7519** | ✅ vérifié — SOT-23-6, 3,5 pF |
| Protection ESD USB, équivalent | USBLC6-2SC6 (UMW) | **C2687116** | ✅ vérifié — souvent moins cher |
| Ampli micro | MAX9814ETD+T | **C41714** | ✅ vérifié — CAG, gain 40/50/60 dB |
| Limiteur de courant | AP2552 | — | ⚠️ **à trouver** — famille confirmée par le datasheet Diodes, référence LCSC non identifiée |
| Micro MEMS analogique | LMA2718T381-OAK02 | C5373228 | ⚠️ **non vérifié** — datasheet inaccessible depuis ici |
| Micro MEMS analogique | MSM381A2718Z9QM2 | C966934 | ⚠️ **non vérifié** |
| Micro MEMS analogique | ZTS6211E | C481297 | ⚠️ **non vérifié** |

Les trois micros MEMS sont des candidats trouvés au catalogue, **pas des choix** :
ni la sensibilité, ni la tension d'alimentation, ni le stock n'ont pu être
confirmés — LCSC sert une coquille aux requêtes automatiques. À valider à la main
avant de figer la nomenclature.

Abandonné en cours de route : **CH224K (C970725)**, contrôleur de négociation PD.
La référence est bonne, le besoin ne l'est pas — voir §3bis.

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
