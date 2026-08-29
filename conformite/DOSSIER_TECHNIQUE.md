# Dossier technique — ce qu'il doit contenir

À constituer avant de signer la déclaration, à conserver **dix ans** après la
mise sur le marché du dernier exemplaire, et à produire sur demande des autorités.

## 1. Description du produit
- Désignation, usage prévu, photographies couleur.
- Versions matérielle et logicielle couvertes par la déclaration.

## 2. Conception et fabrication
- Schéma électrique, implantation, nomenclature.
  → `hardware/NETLIST.md`, `hardware/BOM_PCB.csv`, `hardware/PCB_HARDWARE.md`
- Description du module radio et de son antenne.
- Description de l'alimentation externe et de son propre marquage CE.

## 3. Évaluation des risques
- Analyse des risques couverts par les exigences essentielles.
- Solutions retenues. Les mesures de puissance consignées dans
  `hardware/PCB_HARDWARE.md` en font partie.

## 4. Rapports d'essai
- CEM : EN 301 489-1 et -17, **sur le produit fini**.
- Radio : EN 300 328.
- Sécurité : EN 62368-1.
- Exposition aux champs électromagnétiques.

## 5. Cybersécurité (art. 3.3, depuis le 1er août 2025)
Évaluation documentée face à EN 18031-1/-2/-3. Éléments déjà en place à verser :

- **Aucun service ouvert par défaut** au-delà de l'interface de configuration.
- **Mot de passe optionnel** fermant la totalité des routes, y compris celle qui
  permet de le changer (`src/arena_net.cpp`, middleware d'authentification).
- **Mise à jour du firmware** avec retour arrière automatique : une image non
  validée après 60 s de fonctionnement et obtention d'une adresse IP est
  abandonnée au démarrage suivant.
- **Remise à zéro d'usine accessible physiquement**, sans outil ni réseau : trois
  boutons maintenus cinq secondes.
- **Aucune donnée personnelle collectée ni transmise** : le produit ne parle à
  aucun serveur distant.

Ce qui reste à documenter : la gestion des mises à jour de sécurité dans la
durée, et la procédure de signalement de vulnérabilité.

## 6. Déclaration UE de conformité signée
→ `DECLARATION_UE.md`, une fois complétée.

## 7. Logiciel libre
Le firmware est sous GPL-3.0 et embarque des bibliothèques LGPL-3.0. Les
obligations de mise à disposition du code source sont couvertes par `LICENSE`,
`NOTICE` et la section 9 de la notice client.
