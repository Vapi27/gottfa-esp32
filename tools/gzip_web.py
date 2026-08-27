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
    src = os.path.join(env.subst("$PROJECT_DIR"), "data", "arena.html")
    if not os.path.exists(src):
        return
    dst = src + ".gz"
    with open(src, "rb") as f:
        raw = f.read()
    blob = gzip.compress(raw, 9)
    old = open(dst, "rb").read() if os.path.exists(dst) else None
    if old != blob:
        with open(dst, "wb") as f:
            f.write(blob)
        print("gzip_web: arena.html.gz regenere (%d -> %d octets)" % (len(raw), len(blob)))
    else:
        print("gzip_web: arena.html.gz deja a jour")

env.AddPreAction("$BUILD_DIR/littlefs.bin", build_gz)
build_gz()
