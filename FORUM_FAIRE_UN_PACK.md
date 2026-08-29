# Fabriquer le pack de votre flipper

Un **pack** contient tout ce qu'il faut pour qu'un mur reproduise *votre*
machine : le **plan du plateau** (où sont les lucarnes) et l'**attract** (ce que
la ROM allume, et quand). Deux fichiers, que n'importe qui peut ensuite charger
depuis la page de son mur — sans câble et sans ordinateur.

Ce guide s'adresse à celui qui **fabrique** le pack. Le fabriquer demande un
ordinateur ; l'utiliser, non.

---

## Ce qu'il vous faut

| | |
|---|---|
| La **table Visual Pinball** de votre machine | elle donne la position des lucarnes |
| La **ROM** du jeu, dans `~/.pinmame/roms/` | elle donne l'attract, en la faisant tourner |
| **libpinmame** compilé | c'est lui qui exécute la ROM |
| Python 3 et un compilateur C++ | |

La ROM doit être **la vôtre**, celle de votre machine. Une révision différente
donne un attract différent : notez-la dans le nom du pack.

---

## En une commande

```bash
tools/mkgame.sh "chemin/vers/Votre Table.vpx" <nom_pinmame> 120
```

Par exemple :

```bash
tools/mkgame.sh "~/tables/Alien Poker (Williams 1980) v600.vpx" alpok_l6 120
```

Le résultat sort dans `packs/<nom_pinmame>/` : `plateau.json` et `attract.bin`.
Mettez les deux dans une archive au nom du jeu **et de la révision de ROM**, et
partagez-la sur le forum.

---

## Les pièges, tous rencontrés pour de vrai

### Le fichier de configuration se passe par `--config`

```bash
python3 tools/vpx_inserts.py table.vpx --config tools/games/monjeu.json
```

En le passant **en argument positionnel il est ignoré en silence**, et l'outil
retombe sur ses valeurs par défaut. Le résultat paraît correct — il ne l'est pas.

### Les noms de lampes ne sont pas tous en majuscules

Les auteurs de tables écrivent `L1` ou `l1`, `GI` ou `gi`, indifféremment. Les
motifs de reconnaissance sont insensibles à la casse ; si vous en ajoutez,
gardez-le.

### Une lucarne, deux sources lumineuses

Beaucoup d'auteurs posent **deux** objets lumineux sur la même lucarne : un pour
l'insert, un pour le halo. Même numéro de lampe, quelques pour cent d'écart. Sur
le vrai plateau il n'y en a qu'une.

À ne **pas** confondre avec deux lucarnes réelles partagées par une même lampe —
ça existe. La distance tranche : sur Alien Poker, les doublons de halo sont à
**0,04** l'un de l'autre, les vraies paires à **0,58**, soit plus d'un
demi-plateau. Listez les doublons à la main dans `drop` du fichier de jeu ; un
seuil automatique se trompe en silence.

⚠️ **Ne retirez pas de lucarnes d'un plan déjà en service.** Le mur mémorise
quelle LED est sur quel insert **par son rang dans la liste** : en retirer
décale tous les suivants, et le mapping pointe alors sur les mauvaises lucarnes
sans qu'aucun message ne le dise. Sur un plan neuf, aucun risque.

### La durée du pas de capture

L'outil interroge la matrice de lampes à intervalle régulier, et l'API ne rend
que **les changements depuis le dernier appel** : ce qui s'allume et s'éteint
entre deux interrogations est invisible. Mesuré sur Alien Poker, même ROM :

| pas | durée d'allumage médiane |
|---|---|
| 50 ms | 400 ms |
| 16 ms | **128 ms** |

Le pas grossier **étirait chaque événement d'un facteur trois** — l'attract
paraissait trois fois trop lent. L'outil est réglé sur une trame émulée par pas ;
ne l'augmentez pas.

### Le calage de l'image

Si les pastilles tombent toutes légèrement au-dessus ou en dessous des lucarnes,
c'est que l'image de plateau et les coordonnées d'objets n'ont pas la même
origine. Les entrées `offset_x` et `offset_y` du fichier de jeu corrigent ça, en
fraction de plateau. Vérifiez à l'œil sur la photo : c'est un réglage de goût,
pas un calcul.

---

## Installer un pack sur un mur

Depuis la page du mur, section **Which machine** : chargez `plateau.json` puis
`attract.bin`. La carte **vérifie les deux avant de les installer** — un plan
doit contenir un tableau `inserts` non vide, un attract un en-tête cohérent. Un
fichier douteux est refusé, jamais installé à moitié.

Ensuite, dites au mur quelle LED est sur quelle lucarne : **Playfield → Map the
pixels**, une fois pour toutes.

---

## Ce qu'un pack ne contient pas

- **La photo du plateau.** Elle se charge séparément, et c'est volontaire :
  l'illustration d'une table appartient à son auteur. Prenez la vôtre, ou
  demandez son accord.
- **Le son.** L'attract de beaucoup de machines est muet dans la ROM — vérifié
  sur Alien Poker : trois minutes de capture, crête de 1 sur 32767. Si la vôtre
  émet quelque chose, enregistrez-la et chargez le fichier dans la page.
