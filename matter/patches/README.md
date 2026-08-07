# Corrections portées sur des sources tierces

Ce répertoire contenait des **copies** des fichiers corrigés. C'était un piège :
aucune étape de construction ne les appliquait, et leur présence donnait
l'illusion que les correctifs étaient en place. Le 2026-08-07 la chaîne LED est
repartie noire pour cette raison exacte — le correctif RMT vivait ici, pas dans
la source compilée.

Les deux bibliothèques sont **versionnées dans ce dépôt**. Les corrections sont
donc portées directement dans les fichiers compilés, et le CMakeLists vérifie
qu'elles y sont toujours.

| Fichier corrigé | Correction | Pourquoi |
|---|---|---|
| `matter/components/arduino-esp32/cores/esp32/esp32-hal-rmt.c` | `rmt_tx_channel_config_t tx_cfg = {}` + `tx_cfg.flags.allow_pd = 0` (idem RX) | arduino-esp32 3.2.0 n'assigne pas `flags.allow_pd`, ajouté par IDF 5.4, donc le bit valait ce qui traînait sur la pile. `rmt_tx.c` refuse le canal si `allow_pd` vaut 1 sur une puce sans rétention RMT — le S3 en fait partie. D'où un `rmtInit()` qui échoue **par intermittence** au démarrage : chaîne noire, et aucun rapport avec la broche. |
| `lib/Adafruit_NeoPixel/esp.c` | `xSemaphoreGive(show_mutex)` avant le `return` d'échec, + compteurs de diagnostic | la bibliothèque sortait sur échec de `rmtInit` **sans rendre le mutex**, qui restait pris pour toujours. Signature : 20 images/s exactement, identiques à 1, 41 ou 150 pixels. |

## Vérifier

```
grep -c allow_pd matter/components/arduino-esp32/cores/esp32/esp32-hal-rmt.c   # 5
grep -c xSemaphoreGive lib/Adafruit_NeoPixel/esp.c                            # >= 2
```

Sur la carte : `curl http://<mur>/api/state | grep rmtfail` doit rendre **0**.
