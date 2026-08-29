# Conformité — ce qui est rédigé, et ce qui ne peut pas l'être

**Ces documents sont des brouillons à compléter et à signer. Ils ne valent pas
conformité.** Aucun texte ne remplace un essai en laboratoire, et rien ici n'est
une certification.

Ce dossier contient trois pièces :

| fichier | ce que c'est |
|---|---|
| `DECLARATION_UE.md` | la déclaration UE de conformité, à compléter et signer |
| `MARQUAGE_ET_ETIQUETTE.md` | ce qui doit figurer sur le produit et l'emballage |
| `DOSSIER_TECHNIQUE.md` | ce que la documentation technique doit contenir |

---

## Les deux choses que j'ai vérifiées et qui changent le plan

### 1. Le module pré-certifié ne suffit pas

L'ESP32-S3-WROOM porte sa propre certification RED. Mais elle **couvre le module
seul, jamais le produit fini** : toute modification de l'antenne, du logiciel
radio ou de l'intégration mécanique annule la conformité héritée, et **des essais
CEM sur le produit complet restent obligatoires dans tous les cas**.

Concrètement : la carte a son propre routage, ses propres pistes d'alimentation,
une guirlande de plusieurs mètres qui se comporte comme une antenne, et un
firmware maison. C'est un produit à tester, pas un module à recopier.

### 2. L'article 3.3 s'applique depuis le 1er août 2025

Depuis cette date, la directive RED impose aussi ses exigences de
**cybersécurité** (article 3.3 d/e/f) aux équipements radio connectés à
internet. Ce mur en est un. Les normes harmonisées correspondantes sont la série
**EN 18031** (-1 protection du réseau, -2 données personnelles, -3 fraude).

Une déclaration qui ne cite que la directive sans détailler les normes **par
article** est incomplète, et la présomption de conformité ne peut alors pas être
invoquée.

Ce qui a été fait ce jour va dans ce sens et sera à verser au dossier :
le mot de passe optionnel fermant toutes les routes, la mise à jour signée du
firmware, l'absence de service ouvert par défaut. Ce qui manque encore est
l'**évaluation** documentée face à EN 18031.

---

## Ce que personne ne peut écrire à ma place

1. **Les essais.** CEM (EN 301 489-1/-17), radio (EN 300 328), sécurité
   (EN 62368-1) et exposition RF sur le **produit fini**. Comptez un laboratoire
   notifié ou accrédité.
2. **L'évaluation EN 18031.** Elle porte sur des choix d'architecture, pas sur du
   texte.
3. **L'enregistrement DEEE** auprès de l'éco-organisme, et l'identifiant unique
   qui en découle. C'est une démarche administrative nominative.
4. **La signature.** La déclaration est faite « sous la seule responsabilité du
   fabricant ». C'est toi, pas moi.

---

## ⚠️ Un point qui touche à ta règle sur la boutique

Tu as pour règle de **ne jamais publier d'adresse postale sur pinballs.store** —
contact public par email et formulaire uniquement.

**La déclaration UE de conformité, elle, exige légalement le nom et l'adresse
postale du fabricant**, et le marquage exige une adresse sur le produit ou son
emballage. Les deux ne se contredisent pas, mais il faut le décider en
connaissance de cause :

- la **boutique** garde email + formulaire, comme aujourd'hui ;
- la **déclaration et l'étiquette** portent l'adresse, parce que la loi l'impose
  et qu'elles accompagnent le produit, elles ne sont pas des pages web.

Si tu ne veux pas de ton adresse personnelle sur des milliers d'étiquettes,
l'usage est une **adresse professionnelle** ou une domiciliation. C'est une
décision à prendre avant d'imprimer quoi que ce soit.
