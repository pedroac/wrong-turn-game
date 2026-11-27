wrong-turn: main.c
	$(CC) -O2 -Wall -o $@ main.c $(shell pkg-config --cflags --libs raylib) -lm

run: wrong-turn
	./wrong-turn

clean:
	rm -f wrong-turn wrong-turn-web.zip
	rm -rf web

# ---- WebAssembly build (desktop + mobile browsers):  make web  ->  web/index.html (+ .js .wasm .data) ----
# ---- itch.io upload:  make web-zip  ->  wrong-turn-web.zip (upload as an HTML5 game) ----
# raylib-web/ holds raylib built for PLATFORM_WEB (libraylib.web.a and its headers).
web: main.c shell.html
	mkdir -p web
	emcc -O2 -Wall -std=gnu99 -DPLATFORM_WEB -Iraylib-web main.c raylib-web/libraylib.web.a -o web/index.html \
	    -sUSE_GLFW=3 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=32MB -sSTACK_SIZE=1MB \
	    --preload-file assets/fonts/Tilt_Neon/TiltNeon-Regular.ttf --preload-file assets/fonts/neoneon/Neoneon.otf \
	    --preload-file assets/fonts/beon/Beon-Regular.ttf --shell-file shell.html

web-zip: web
	cd web && rm -f ../wrong-turn-web.zip && zip -r ../wrong-turn-web.zip index.html index.js index.wasm index.data

.PHONY: run clean web web-zip
