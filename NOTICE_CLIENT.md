# Wall Pinball Playfield — Notice de montage

*Votre plateau rejouera les vraies lumières de la machine d'origine.*

## Dans le kit

- La **carte de contrôle** (port USB-C)
- Les **cartes LED** (une par insert) + le **câble** (bus d'alimentation et liaisons data)
- Non fourni : un chargeur **USB-C 5 V / 3 A** (n'importe quel chargeur de
  téléphone récent convient) et votre plateau.

## 1 · Poser les cartes LED

Une carte sous chaque insert à éclairer, LED vers le plastique.

## 2 · Câbler — la règle d'or : la masse d'abord

1. **Le bus** : deux fils (+5 V et GND) qui courent ensemble sous le plateau et
   traversent les pattes latérales de chaque carte LED.
   Au-delà de ~30 cartes, ramenez une seconde paire du bus vers la carte de
   contrôle (« injection ») pour éviter les couleurs jaunies en bout de chaîne.
2. **La chaîne data** : `OUT` d'une carte → `IN` de la suivante. Sauts **courts**
   (≤ 15 cm), qui longent le bus — jamais en boucle qui s'en écarte.
3. La **carte de contrôle** : sortie LED → `IN` de la première carte, bus sur
   ses borniers. Puis le câble USB-C.

## 3 · Premier allumage — par paliers

Branchez avec ~10 cartes raccordées, pas tout d'un coup :

1. Cherchez sur votre téléphone un réseau WiFi dont le nom **commence par
   `Playfield-`** — les quatre caractères qui suivent sont propres à votre carte,
   ils viennent de son numéro de série. Mot de passe **`pinball87`**.
   Connectez-vous, ouvrez **`http://192.168.4.1`**, renseignez votre WiFi.
   Ensuite le plateau est joignable à **`http://playfield-XXXX.local/`**, où
   `XXXX` est ce même suffixe. Si vous renommez le mur, l'adresse suit son nom :
   « Alien Poker » donne `http://alien-poker.local/`.
2. Mode **`test`** : chaque LED cycle rouge → vert → bleu → blanc, avec un
   pixel blanc qui parcourt la chaîne. Là où il s'arrête, le raccord suivant
   est en cause.
3. Ça marche ? Ajoutez le tronçon suivant (courant coupé !) et recommencez.

## 4 · Dire au plateau où sont ses lampes (une fois)

Page web → **Mapping wizard** → réglez le nombre de LEDs → **Start walking** :
une LED clignote sur votre plateau, **cliquez l'insert correspondant sur le
plan** — et ainsi de suite. Chaque clic est enregistré ; vous pouvez vous
arrêter et reprendre quand vous voulez. C'est ce qui permet au plateau de
rejouer les vraies séquences de la machine.

## 5 · Utiliser

- **Modes** : `attract` (le jeu de lumières **d'origine**), `classic` (blanc
  chaud), `arena`, `night`, `rainbow`, `music` (suit votre musique).
- **Réglages** : luminosité, vitesse (se cale sur la cadence d'origine),
  fond lumineux, chaleur, rendu ampoule (*Filament*), couleur libre + presets
  **original bulb** / **older bulb**.
- **Save** : vos réglages deviennent ceux du démarrage.

## 6 · C'est normal (pas une panne)

- En `attract`, **certains inserts restent éteints** : la vraie machine fait
  pareil hors partie (lampes de jeu).
- Si tout **baisse d'un coup** : la protection électrique limite la puissance.
- Après une **coupure de courant**, tout revient tel quel — réglages et
  mappage compris.

## 7 · Sécurité

- Chargeur **USB-C 5 V de qualité** uniquement. Usage **intérieur**, au sec.
- **Toujours couper l'alimentation** avant de raccorder ou modifier le câblage.

## 8 · Un souci ?

- **Une LED reste noire et tout ce qui suit aussi** : c'est la première LED
  noire qui est en cause (alimentation ou raccord), pas les suivantes.
- **Page introuvable** : le plateau rouvre son propre WiFi `Playfield-…` de
  lui-même quand il ne retrouve plus votre réseau → refaites l'étape 3.1.
  L'adresse en `.local` dépend du nom du mur ; en cas de doute, passez par
  l'adresse IP que votre box attribue au plateau.
- **Support** : formulaire de contact sur **pinballs.store**
