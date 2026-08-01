# Wall Pinball Playfield

**Un plateau de flipper accroché au mur, qui joue sa propre lumière.**

Des LEDs adressables prennent la place des ampoules sous chaque insert d'un
plateau de Gottlieb Arena. Le mur ne joue pas « une animation qui ressemble à
un flipper » : il rejoue **l'attract mode d'origine de la machine**, capturé
lampe par lampe depuis sa vraie ROM — chenillards, rollovers, drop targets
W-A-L-L, à la cadence exacte de 1987. Le tout se pilote depuis une page web,
se met à jour par WiFi, et se transpose à n'importe quelle autre table.

---

## 1. Le matériel

| Élément | Choix | Rôle |
|---|---|---|
| Pixels | **SK6812MINI-RGBW** sur petites cartes (une par insert) | 4 dies dont un blanc dédié — indispensable pour le rendu incandescent |
| Contrôleur | **D1 Mini ESP32** (WROOM-32, 4 Mo) — S3 et C3 supportés | WiFi, OTA, RMT (la chaîne ne coûte ~rien en CPU) |
| Alimentation | 5 V, fusible en série (3 A sur le montage de référence) | le firmware mesure chaque trame et écrête avant le fusible |
| Niveau logique | banc : **pixel répéteur caché** (2× 1N4148, maintenu éteint à ~4,2 V) · PCB : **74AHCT125** | l'ESP sort 3,3 V, la chaîne 5 V exige 3,5 V — voir ARENA_LED.md §4 |
| Micro (option) | **MAX9814** → GPIO34 | mode musique autonome (`ARENA_MIC_ENABLE 1`) |
| Bouton (option) | poussoir → GPIO0 | cycle des modes / mode nuit sans téléphone |

**Broches** : 27 = data, 26 = data 2 (option), 34 = micro, 0 = bouton.

### Câblage

```
Alim 5V ─┬─ fusible ─┬────────────── bus +5V (injection tous les 30-40 px)
         │           └────────────── ESP broche 5V
         └── GND ────┬────────────── bus GND (continu : c'est le retour du data)
                     └────────────── ESP GND
ESP GPIO27 ─[330 Ω]─► répéteur (4,2 V) ─► LED1 ─► LED2 ─► … (sauts ≤ 15 cm, le long du bus)
```

Règles éprouvées sur ce montage : **masse d'abord**, injection d'alimentation
tous les 30-40 pixels ramenée en étoile, condensateur 1000 µF par point
d'injection, et **allumer par paliers** (10, 30, 75 pixels) — un palier qui
échoue désigne le saut fautif, un mur fini ne dit rien.

### Le PCB final (v2)

74AHCT125 (remplace le répéteur → `LED_REPEATER_PIXEL 0`, bus à 5 V plein),
MAX9814, bouton GPIO0, entrée 5 V avec porte-fusible + 1000 µF + protection
d'inversion, data en JST **avec GND adjacent**, sortie chaîne 2 en option.

---

## 2. Première mise en route

1. **Flash initial** (USB, la seule fois où il est nécessaire) :
   ```sh
   pip install platformio
   tools/arena_flash.sh          # détecte le port, compile, flashe UI + firmware
   ```
2. La carte ouvre le WiFi **`Arena-LED`** (mot de passe `pinball87`) →
   `http://192.168.4.1/` → renseigner le WiFi maison (ou y rester).
3. Ensuite elle répond sur **`http://arena.local/`** et tout le reste — mises à
   jour comprises — se fait par WiFi.
4. **Test une LED** : mode `test` → la LED cycle rouge → vert → bleu → blanc.
   Couleurs inversées ? Changer `order` en direct (`grbw` par défaut), sans
   reflasher.

---

## 3. La page web

### Les modes

| Mode | Ce qu'il joue |
|---|---|
| `attract` | **l'attract d'origine de la machine**, depuis la ROM (voir §5) |
| `arena` | animations géométriques sur le plateau (onde qui monte, ondulation du centre) |
| `classic` | blanc chaud fixe, micro-scintillement « incandescent » |
| `music` | le mur suit la pièce : basse = respiration, beat = onde, aigus = étincelles (§6) |
| `night` | veilleuse chaude ~10 % |
| `rainbow` | balayage de teintes |
| `test` | R/G/B/W + pixel baladeur — câblage et ordre des couleurs |
| `off` | éteint (la chaîne reste rafraîchie) |

### Les réglages

- **Brightness / Speed** — la vitesse est **aimantée sur ×1**, la cadence
  exacte de la ROM ; l'affichage est un multiplicateur.
- **Glow** — l'éclairage général : sur une vraie machine le GI reste allumé en
  attract. 0 = vraiment éteint (au goût du propriétaire).
- **Warmth** — quelle part du blanc porte un filament chaud (0 = spectral/orangé,
  255 = blanc avant).
- **Filament** — la simulation d'incandescence : montée ~40 ms, extinction par
  la braise rouge (~ demi-seconde), couleur qui suit la température du corps
  noir (table de Planck embarquée, regénérable par `tools/filament_lut.py`).
- **Colour + presets** — `original bulb` (verre clair, la #47 exacte) et
  `older bulb` (l'ambre d'une ampoule fatiguée). Filament ON : la couleur
  teinte **le verre** de l'ampoule (blanc pur = verre clair). Filament OFF :
  interrupteur franc dans cette couleur.
- **Power** — jauge temps réel contre le budget (mA). Le budget ne compte
  **pas** l'ESP (~100-250 mA sur le même fusible) : dimensionner budget + 300 mA.

---

## 4. Le plan du plateau et le mappage

Le plan embarqué contient **75 inserts** avec leurs positions réelles
(extraites de la table Visual Pinball du jeu) et leurs **numéros et fonctions
du manuel de service** (« L9 — #1 TOP ROLLOVER », « L36 — W DROP TARGET »).

**Assistant de mappage** (panneau *Mapping wizard*) :
- **Place pixels** — la carte fait clignoter un pixel, on clique l'insert où
  il se trouve. Chaque clic est sauvegardé (NVS : survit aux mises à jour).
  `Go to pixel` saute à un numéro ; les pixels sont numérotés **depuis 0**.
- **Edit inserts** — cliquer un insert allume le pixel qui s'y trouve et
  permet : de le **renommer** (le manuel de la machine fait autorité — les
  noms livrés sont vérifiés pour Arena mais une autre table peut porter des
  erreurs d'auteur VP), et de lui donner la **couleur de son plastique**, qui
  filtre la lumière dans tous les modes, comme sur le vrai plateau.

---

## 5. L'attract d'origine

`data/arena_attract.bin` = 120 s de la matrice de lampes commandée par la ROM
d'Arena, échantillonnée toutes les 50 ms **sur l'horloge émulée** (PinMAME).
Chaque numéro de lampe correspond à un insert du plan, donc la capture tombe
directement sur le mur.

- **Certaines lampes ne s'allument jamais hors partie** (L3 Shoot Again…) :
  c'est le comportement d'origine, pas une panne.
- **Machine « qui a joué »** : `/api/latch?n=L9,L48` maintient des lampes
  allumées par-dessus la séquence, comme après une partie ; `?clear=1` relâche.
- La séquence n'est jamais retouchée : « c'est la ROM » reste vrai.

---

## 6. Le mode musique

Trois façons de le nourrir :

1. **Un ordinateur qui écoute la pièce** (zéro matériel) :
   ```sh
   python3 tools/music_feeder.py        # micro de l'ordinateur → /api/music à 20 Hz
   ```
2. **Micro embarqué** : MAX9814 sur GPIO34 + `ARENA_MIC_ENABLE 1` (défaut 0 :
   un pin flottant lit le bruit WiFi comme de la musique).
3. **`/api/music?e=&b=&t=`** (0-255, ~15 Hz) depuis n'importe quoi.

Sans signal, le mode respire doucement au lieu de s'éteindre.

---

## 6 bis. Commande vocale

**Siri — natif, zéro configuration côté carte.** L'app **Raccourcis** d'iOS
sait appeler une URL, et toute l'API est en GET :

1. Raccourcis → **+** → action « **Obtenir le contenu de l'URL** »
2. URL : `http://arena.local/api/set?mode=attract` (ou `mode=off`, `mode=music`…)
3. Nommer le raccourci « **Allume le flipper** » → c'est la phrase Siri.

Un raccourci par phrase : « Allume le flipper » → `mode=attract`, « Éteins le
flipper » → `mode=off`, « Flipper en musique » → `mode=music`, « Flipper en
veilleuse » → `mode=night`. Fonctionne depuis l'iPhone, l'Apple Watch, et via
un HomePod (qui relaie au téléphone). Si `arena.local` est capricieux depuis
iOS, réserver l'IP de la carte dans la box et l'utiliser en direct.

**Google Home — pas de chemin direct.** Les routines Google ne savent pas
appeler une URL locale, et les passerelles grand public (IFTTT) sont devenues
payantes et peu fiables. Deux vraies options :
- **Home Assistant** (si vous en avez un) : une entité REST vers l'API, puis
  l'intégration Google Home standard — « OK Google, allume le flipper ».
- **Matter, sur le PCB final** : la carte se présenterait comme une lampe
  Matter et s'appairerait nativement à Google Home, Apple Maison **et** Alexa,
  en local, sans nuage. C'est la réponse « produit » — mais c'est un vrai
  chantier firmware (pile ESP-IDF, partitions, appairage), pas un réglage.

## 7. Changer de jeu

Le firmware est **agnostique** : un jeu = un *bundle* de deux fichiers.

- **Le jeu est dans `bundles/`** → page web, panneau **Game**, téléverser
  `pf.json` puis `attract.bin`. La carte **valide avant d'accepter** (un
  mauvais fichier est refusé avec la raison, sans rien casser), redémarre, et
  garde le mappage. Nouvelle table = nouveaux inserts → repasser par le wizard.
- **Sinon** → il faut la table `.vpx` du jeu et **sa** ROM (le bundle n'est
  qu'un enregistrement de masques de lampes — la ROM ne se redistribue pas) :
  ```sh
  cp <jeu>.zip ~/.pinmame/roms/
  tools/mkgame.sh "<table>.vpx" <nom_pinmame>
  ```
  `tools/games/<jeu>.json` (optionnel) porte la table de lampes du manuel et
  les corrections d'erreurs de nommage VP — voir `arena.json` pour le format.

---

## 8. Mises à jour

```sh
pio run -e arenaled_d1mini32 && curl -F u=@.pio/build/arenaled_d1mini32/firmware.bin http://arena.local/update
pio run -e arenaled_d1mini32 -t buildfs && curl -F u=@.pio/build/arenaled_d1mini32/littlefs.bin "http://arena.local/update?target=fs"
```

**Un OTA réussi répond souvent HTTP 000** : la carte redémarre avant d'envoyer
la réponse. Vérifier par `up` remis à zéro dans `/api/state`, ne pas renvoyer
à l'aveugle. Le mappage, les couleurs, les noms et les réglages sont en NVS et
survivent aux deux types de flash ; une coupure pendant un flash d'interface
laisse simplement **l'ancienne version** en place — re-flasher.

---

## 9. API REST

Tout ce que fait la page passe par là ; base `http://arena.local/`.

| Endpoint | Paramètres | Rôle |
|---|---|---|
| `/api/state` | — | état complet (mode, réglages, mA, budget, fps, attract chargé, stockage…) |
| `/api/set` | `mode` `bright` `speed` `gi` `warm` `inc` `r g b w` `count` `budget` `order` | tout changer, effet immédiat ; rien ne persiste sans `/api/save` |
| `/api/save` | — | figer l'état courant comme démarrage |
| `/api/latch` | `n=L9,L48` · `clear=1` | lampes maintenues pendant l'attract (noms machine) |
| `/api/music` | `e b t` (0-255) | piloter le mode musique (~15 Hz) |
| `/api/pf` | — | le plan : noms, fonctions du manuel, lampes, positions, couleurs |
| `/api/ledmap` · `/api/ledmap/reset` | — | pixel ↔ insert · tout oublier |
| `/api/assign` | `led=N` + `ins=M`\|`none` | placer un pixel (sauvegardé à chaque appel) |
| `/api/insert` | `ins=N` + `name=`/`r g b w` · `clear=1` | renommer / colorer un insert |
| `/api/identify` | `led=N`\|`zone=N`\|`clear=1`, `ms=` | projecteur de mappage par-dessus tout mode |
| `/api/zones` (GET/POST) · `/api/zones/reset` | POST = JSON brut (`--data-binary`, pas `-d`) | plages nommées des effets de secours |
| `/api/game` (POST) | `?target=pf`\|`attract` + fichier | téléverser un bundle — validé puis reboot |
| `/update` (POST) | fichier ; `?target=fs` pour l'interface | OTA firmware / système de fichiers |
| `/api/wifi` | `ssid` `pass` | rejoindre un réseau (NVS) ; repli SoftAP `Arena-LED` |

---

## 10. Dépannage

| Symptôme | Cause la plus probable | Remède |
|---|---|---|
| Tout est noir après le pixel N | pixel N+1 mort ou non alimenté (un pixel est un répéteur : muet = tout le reste noir) | VDD sur la carte, continuité du saut, sens IN/OUT, puis **échanger la carte avec une saine** — ça sépare « carte morte » de « position mauvaise » |
| Un insert ne s'anime jamais en attract | lampe de partie (normal hors partie) → `latch` ; ou insert mal nommé côté table VP | l'allumer via `identify` prouve que le pixel est sain |
| Upload « accepté » mais rien ne change | coupure pendant le flash — l'ancienne version est restée | re-téléverser, vérifier par `up`/`atr` |
| Page lente, `arena.local` capricieux | résolution mDNS | utiliser l'IP directe |
| Le mur « danse » dans le silence (musique) | micro activé mais pas câblé (pin flottant = bruit WiFi) | `ARENA_MIC_ENABLE 0` ou câbler le micro |
| Rouge et vert échangés | ordre du lot de LEDs | `order=rgbw`… en direct, sans reflasher |
| Le limiteur assombrit tout | budget atteint | le mur baisse uniformément — c'est la protection du fusible qui travaille |

---

## 11. Caractéristiques

- 63 images/s, jusqu'à **150 pixels** (`LED_MAX`), une ou deux chaînes.
- Modèle de courant 17,5 mA/die (limiteur par trame) — *à confronter une fois
  à un ampèremètre : c'est la dernière constante non mesurée du projet.*
- Capture d'attract : ≤ 12 288 trames (10 min à 50 ms) — plafond vérifié à
  l'upload **et** au chargement.
- Stockage : bundle ~24 Ko, partition 1,4 Mo (~87 % libre sur le montage de
  référence).
- Réglages, mappage, couleurs, noms : **NVS** (survivent à tous les OTA) ; le
  mode est persisté **par nom** (insensible aux évolutions du firmware).

## Pour aller plus loin

- **[ARENA_LED.md](ARENA_LED.md)** — notes d'ingénierie complètes (EN) :
  marges de niveau logique, mesures du banc, leçons apprises.
- **[bundles/README.md](bundles/README.md)** — la bibliothèque de jeux.
- **`tools/`** — extraction VPX, capture PinMAME, empaquetage, feeder musique.
