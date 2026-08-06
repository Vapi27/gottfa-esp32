# Tables de jeu — switchs, bobines, lampes

## À quoi ça sert

Le test de bobines (`coiltest.h`) diagnostique par **retour de contact** : une bobine saine
déplace quelque chose, et presque tout ce qui bouge est vu par un switch. Ça ne donne un
verdict que si l'on sait **quel** switch chaque bobine est censée bouger — et cette table est
propre à chaque titre.

Personne ne la publie sous forme exploitable :

* **PinMAME** donne à *tous* les System 80 les mêmes ports d'entrée génériques. Vérifié :
  `src/wpc/gts80games.c` ligne 14 est `GTS80_INPUT_PORTS_START(gts80,1) GTS80_INPUT_PORTS_END`
  pour chaque titre. Aucun nom de switch par jeu.
* **LISY** (`lisy_5_28/src/lisy/`) ne nomme que les DIP switchs.

Elle est en revanche dans le **manuel de chaque jeu** : planche « SWITCH MATRIX », planche
« PLAYBOARD CONTROLLED SOLENOIDS AND ILLUMINATION », et sur les 80B une table typographiée
« PLAYBOARD SWITCH AND LAMP ASSIGNMENTS ». Les manuels dont on dispose sont des **scans sans
couche texte** (~10 caractères par page, soit le seul numéro de page), donc les tables ont été
lues sur les pixels — voir `gottfa-tools/pdfocr/`.

## Pourquoi la précondition est le cœur du sujet

Le lien bobine→switch est **conditionnel à l'état du plateau**. Un relève-cibles ne déplace
rien si les cibles sont déjà relevées ; un éjecteur ne déplace rien sans bille. Sans savoir ce
qu'une bobine est *censée* bouger, une bobine parfaitement saine ressort « aucune réaction » :
une accusation fausse contre du matériel sain, strictement pire que pas de test du tout.

C'est pour ça que la table est embarquée dans le firmware et pas seulement dans l'interface :
c'est le firmware qui doit pouvoir répondre « précondition non remplie, mets les cibles en
bas » au lieu de « muette ».

## La provenance fait partie de la donnée

Chaque fiche porte `src` (d'où vient la table) et `conf` :

| `conf` | Sens |
|---|---|
| `0` | brouillon OCR **non relu** — ne doit jamais être livré dans `data/` |
| `1` | relu par un humain contre la page, **une** source |
| `2` | relu **et** recoupé avec une seconde source indépendante |

`conf=2` veut dire que deux artefacts produits par des gens différents à des époques
différentes disent la même chose. Pour Volcano et Arena, la seconde source est le script de la
table Visual Pinball du titre (`cGameName` + `SolCallback` + `InitDrop`/`InitSaucer`/`InitSw`).

## Format — `data/gd-NN.json`

`NN` = le numéro de jeu du FPGA, 0..62 (annexe A de `GottFA80_PLuS_GameSelect`), c'est-à-dire
exactement ce que la carte rapporte sur le lien série.

```json
{"v":1,"g":12,"t":"Volcano","gtb":667,"fam":80,"conf":2,
 "src":"Manuel #667 p.33 + table VPX vlcno_ax : concordance totale",
 "note":"mise en garde propre au titre",
 "sw":{"20":"Outhole","30":"Couloir DROIT"},
 "lp":{"16":"BOBINE trou éjecteur"},
 "lpc":[8,12,13,14,15,16],
 "c":[{"n":9,"f":"Outhole","s":[20,30,40],"p":"une bille dans l'outhole"},
      {"n":8,"f":"Knocker","x":"ne déplace aucun switch"}]}
```

| clé | sens |
|---|---|
| `sw` | nom du switch, indexé comme le manuel l'imprime : `strobe*10 + return` |
| `lp` | nom de la sortie lampe, indexé par le **numéro L du manuel**, qui est l'indice de bit — mesuré sur machine : L12 = bit 12 = **case 13** de l'interface (les cases sont étiquetées `bit+1`) |
| `lpc` | sorties `lp` qui pilotent en fait une **bobine ou un relais**, pas une ampoule |
| `c[].s` | switchs que cette bobine doit déplacer |
| `c[].p` | ce que l'opérateur doit préparer avant le tir |
| `c[].x` | pourquoi cette bobine ne pourra **jamais** être vérifiée ainsi (exclusif avec `s`/`p`) |
| `alias` | renvoie une variante (voix / son seul) vers la table du plateau partagé |

`lpc` compte : sur Volcano quatre vraies bobines sont câblées sur des sorties **lampes** du
master driver — L15 libération de bille, L16 trou éjecteur, L8 fire pit, L14 porte à billes —
donc invisibles au test des 9 solénoïdes. Un opérateur qui cherche un trou éjecteur mort doit
savoir qu'il se déclenche depuis l'onglet Lampes. Sur Arena c'est L13/L14 (portes).

## État

| # | Titre | `conf` | Sources |
|--:|---|--:|---|
| 12, 13 | Volcano | 2 | manuel #667 p.33/34 + VPX `vlcno_ax` — 33 switchs concordants sur 33 |
| 51 | Arena | 2 | manuel #709 p.40/41 **et** table p.53 + VPX `arena` — 3 confirmations |

Les autres titres : brouillons dans `gottfa-tools/pdfocr/draft/`, avec
`draft/COVERAGE.md` par titre. **Ils ne sont pas livrés** : un brouillon non relu qui pilote
un diagnostic est précisément ce qu'on cherche à éviter.

Sur les 63 titres, à partir de 94 manuels Gottlieb 1980-1990 OCR'isés :

| État du brouillon | Titres |
|---|--:|
| relu et livré (`conf=2`) | 3 |
| `ok` — ≥ 20 switchs extraits, prêt à relire | 24 |
| `faible` — quelques switchs, scan médiocre | 15 |
| `vide` — manuel présent, extraction infructueuse | 17 |
| pas de manuel dans la bibliothèque | 4 |

Les deux formats de manuel sont gérés : les titres de 1981+ dessinent une **grille**
(`SW.nn` dans des cellules, lue géométriquement), ceux de 1980 impriment une **table
typographique** (`00  #1 Drop Target`, lue en lignes, avec ses lampes « CPU CONTROLLED
LAMPS »). `extract_tables.py --check` mesure l'extracteur contre Volcano et Arena, dont les
tables sont connues : **Volcano 11/11**, Arena 5/9 — les planches 80B sont plus denses et les
libellés y bavent. C'est pour ça que la sortie est un brouillon et pas une donnée.

## Promouvoir un brouillon

1. `cd gottfa-tools/pdfocr && python3 extract_tables.py --pdf "<manuel>" --game NN` — la
   sortie dit sur quelles pages il a trouvé la matrice et les solénoïdes.
2. Rendre ces pages et **les lire** :
   `pdftoppm -f <page> -l <page> -r 175 -png "<manuel>" /tmp/p` (les planches sont en
   **paysage** : compter les découpes en conséquence).
3. Corriger les libellés, traduire, remplir `c[].s` / `c[].p` / `c[].x` à partir de la planche
   des solénoïdes.
4. Chercher une seconde source. Une table Visual Pinball du titre en est une bonne : son
   script contient `SolCallback(n)` et les numéros de switch de `InitDrop`/`InitSaucer`.
   Extraction : `strings -a "<table>.vpx" | grep -n -A2 SolCallback`.
5. `conf=1` (une source) ou `conf=2` (deux concordantes), puis déplacer dans `data/`.
6. `../gottfa-bitstreams/pstore deploy <ip>` — la fiche est relue au démarrage et à chaque
   lancement de test.

Un titre sans fiche **fonctionne quand même** : le test apprend puis rejoue. Il ne peut
simplement ni nommer les contacts, ni distinguer une panne d'un plateau mal préparé — et il le
dit, au lieu de faire semblant.
