# Marquage du produit et étiquette

## Sur le produit lui-même (ou sur son emballage s'il est trop petit)

1. **Marquage CE**, hauteur minimale 5 mm, proportions respectées.
2. **Nom du fabricant** et **adresse postale** à laquelle il peut être contacté.
   Une seule adresse, celle du point de contact.
3. **Modèle**, **numéro de lot ou de série** — permettant d'identifier l'appareil.
4. **Pictogramme DEEE** : la poubelle barrée. Il signifie que l'appareil ne se
   jette pas avec les ordures ménagères.
5. **Tension et courant d'alimentation** : 5 V continu, [courant] A.

## Sur l'emballage ou la notice

- Les **bandes de fréquences** et la **puissance maximale** émise.
- L'adresse internet où la **déclaration UE de conformité** est consultable.
- Les **restrictions d'usage** éventuelles selon les États membres (à vérifier :
  la bande 2,4 GHz n'en a normalement pas en usage intérieur).

## Étiquette de la carte — proposition

```
   ┌──────────────────────────────────────────┐
   │  [nom du produit]          Modèle : ____ │
   │  S/N : ________            5 V ⎓  ___ A  │
   │                                          │
   │  [Raison sociale]                        │
   │  [Adresse postale]                       │
   │                                          │
   │   C E          🗑  (poubelle barrée)      │
   │                                          │
   │  WiFi : Playfield-XXXX                   │
   │  2400–2483,5 MHz — ___ dBm               │
   └──────────────────────────────────────────┘
```

**Les quatre caractères `XXXX` du réseau WiFi doivent figurer sur cette
étiquette.** Ils viennent de l'adresse matérielle de la carte et sont propres à
chaque exemplaire : c'est ce que le client cherche sur son téléphone au premier
allumage. Sans eux, la notice lui demande de deviner.

## Ce qui ne va PAS sur l'étiquette

- Aucune donnée personnelle au-delà de l'identification légale du fabricant.
- Pas de numéro de téléphone : le support passe par le formulaire de la boutique.
