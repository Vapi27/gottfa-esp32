# Regenere data/arena.html.gz avant la construction du systeme de fichiers.
#
# La page est servie compressee (2,8x moins de temps d'antenne, ce qui compte
# parce qu'une interruption WiFi tombee dans un s_strip.show() fait decrocher la
# fin de la chaine). Mais un .gz genere a la main est un piege classique : on
# edite arena.html, on oublie de recompresser, et la carte sert l'ancienne
# interface sans qu'aucune erreur ne le dise. On le refait donc a chaque build.
import gzip, os
Import("env")

def build_gz(*args, **kwargs):
    # La page est la SOURCE, elle ne fait plus partie de l'image de fichiers :
    # elle est compilee dans le firmware (voir plus bas). data/ ne contient plus
    # que ce qui appartient au client et aux jeux.
    src = os.path.join(env.subst("$PROJECT_DIR"), "web", "arena.html")
    if not os.path.exists(src):
        return
    with open(src, "rb") as f:
        raw = f.read()
    blob = gzip.compress(raw, 9)

    # --- La page part aussi DANS LE FIRMWARE -------------------------------
    #
    # Pourquoi : mettre a jour l'interface passait par un OTA du systeme de
    # fichiers, et U_SPIFFS reecrit la partition ENTIERE. Le client y perdait sa
    # photo de plateau, son son d'attract et le plan de sa machine - tout ce
    # qu'il avait mis une soiree a poser. Constate le meme jour sur les groupes,
    # qui ont ete deplaces en NVS pour cette raison exacte.
    #
    # En embarquant la page dans la partition applicative, mettre a jour
    # l'interface devient une simple mise a jour du firmware, qui ne touche pas
    # a LittleFS. La partition de fichiers ne contient plus que ce qui
    # appartient au client, et plus rien ne l'ecrase.
    #
    # 31 ko sur les 3,3 Mo de la partition applicative, soit 1 %.
    hdr = os.path.join(env.subst("$PROJECT_DIR"), "include", "web_page.h")
    lines = ["// GENERE par tools/gzip_web.py — NE PAS EDITER.",
             "// Source : data/arena.html, compressee puis embarquee dans le firmware.",
             "#pragma once",
             "#include <pgmspace.h>",
             "static const size_t WEB_PAGE_GZ_LEN = %d;" % len(blob),
             "static const uint8_t WEB_PAGE_GZ[] PROGMEM = {"]
    for i in range(0, len(blob), 16):
        lines.append("  " + ",".join("0x%02x" % b for b in blob[i:i + 16]) + ",")
    lines.append("};")
    text = "\n".join(lines) + "\n"
    prev = open(hdr).read() if os.path.exists(hdr) else None
    if prev != text:
        with open(hdr, "w") as f:
            f.write(text)
        print("gzip_web: include/web_page.h regenere (%d octets embarques)" % len(blob))

# La page est desormais aussi compilee DANS le firmware : l'en-tete doit
# exister avant la compilation, pas seulement avant l'image de fichiers.
env.AddPreAction("$BUILD_DIR/src/arena_net.cpp.o", build_gz)
build_gz()
