Five maps are kept in this repository (Busan_Port, Normandie, Titanic_Arena, babelstairs,
Creative_Flat): between them every game mode can be played. The server downloads each map its rotation
names from https://github.com/ngspades/maps into this folder at start-up
(`[maps] download` in the config), and `tools/gen_maps.py` writes a random one:

    tools/gen_maps.py --kind caves --size 512 --seed 1 --out server/Resources/maps

Credits: Creative_Flat is by Gustavo Toledo (ngspades), GPL-3.0-or-later like the server. The
others belong to their authors, named in each map's `.txt` and `.toml`: Busan_Port (ilrak,
phalanx, kmsi), Normandie (Influx), Titanic_Arena (Serious Mapping), babelstairs (Danke).
