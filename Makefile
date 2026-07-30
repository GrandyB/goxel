SHELL = bash
ifeq ($(OS),Linux)
	JOBS := "-j $(shell nproc)"
else
	JOBS := "-j $(shell getconf _NPROCESSORS_ONLN)"
endif

.ONESHELL:

all:
	scons $(JOBS)

release:
	scons $(JOBS) mode=release

profile:
	scons $(JOBS) mode=profile

debug:
	scons $(JOBS) mode=debug

run:
	./goxel

clean:
	scons -c

analyze:
	scan-build scons mode=analyze

# Generate an AppImage.  Used by github CI.
appimage:
	scons mode=release
	mkdir AppDir
	DESTDIR=AppDir PREFIX=/usr make install
	curl https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20231206-1/linuxdeploy-x86_64.AppImage \
		--output linuxdeploy.AppImage -L -f
	chmod +x linuxdeploy.AppImage
	./linuxdeploy.AppImage --output=appimage --appdir=AppDir

# Targets to install/uninstall goxel and its data files on unix system.
PREFIX ?= /usr/local

.PHONY: install
install:
	install -Dm755 goxel $(DESTDIR)$(PREFIX)/bin/goxel
	for size in 16 24 32 48 64 128 256; do
	    install -Dm644 data/icons/icon$${size}.png \
	        $$(printf '%s%s' $(DESTDIR)$(PREFIX)/share/icons/hicolor/ \
	            $${size}x$${size}/apps/goxel.png)
	done
	install -Dm644 snap/gui/goxel.desktop \
	    $(DESTDIR)$(PREFIX)/share/applications/goxel.desktop
	install -Dm644 \
	    snap/gui/io.github.guillaumechereau.Goxel.metainfo.xml \
	    $$(printf '%s%s' $(DESTDIR)$(PREFIX)/share/metainfo/ \
	        io.github.guillaumechereau.Goxel.metainfo.xml)

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/goxel
	for size in 16 24 32 48 64 128 256; do \
	    rm -f $$(printf '%s%s' $(DESTDIR)$(PREFIX)/share/icons/hicolor/ \
	        $${size}x$${size}/apps/goxel.png)
	done
	rm -f $(DESTDIR)$(PREFIX)/share/applications/goxel.desktop
	rm -f $$(printf '%s%s' $(DESTDIR)$(PREFIX)/share/metainfo/ \
	         io.github.guillaumechereau.Goxel.metainfo.xml)

# Fetch latest stb_image / stb_image_write from https://github.com/nothings/stb
# and copy them into every tree that vendors those headers.
STB_RAW_URL = https://raw.githubusercontent.com/nothings/stb/master
STB_IMAGE_HEADERS = stb_image.h stb_image_write.h
STB_DEST_DIRS = ext_src/stb

.PHONY: update-stb
update-stb:
	@tmpdir=$$(mktemp -d)
	trap 'rm -rf "$$tmpdir"' EXIT
	for f in $(STB_IMAGE_HEADERS); do
		echo "Fetching $$f..."
		curl -fsSL "$(STB_RAW_URL)/$$f" -o "$$tmpdir/$$f"
	done
	for dest in $(STB_DEST_DIRS); do
		mkdir -p "$$dest"
		for f in $(STB_IMAGE_HEADERS); do
			cp "$$tmpdir/$$f" "$$dest/$$f"
			echo "Updated $$dest/$$f"
		done
	done
	for f in $(STB_IMAGE_HEADERS); do
		grep -m1 'Version:\|stb_image' "$$tmpdir/$$f" || head -n 1 "$$tmpdir/$$f"
	done

# Fetch latest cgltf / cgltf_write from https://github.com/jkuhlmann/cgltf
# and copy them into every tree that vendors those headers.
CGLTF_RAW_URL = https://raw.githubusercontent.com/jkuhlmann/cgltf/master
CGLTF_HEADERS = cgltf.h cgltf_write.h
CGLTF_DEST_DIRS = ext_src/cgltf

.PHONY: update-cgltf
update-cgltf:
	@tmpdir=$$(mktemp -d)
	trap 'rm -rf "$$tmpdir"' EXIT
	for f in $(CGLTF_HEADERS); do
		echo "Fetching $$f..."
		curl -fsSL "$(CGLTF_RAW_URL)/$$f" -o "$$tmpdir/$$f"
	done
	for dest in $(CGLTF_DEST_DIRS); do
		mkdir -p "$$dest"
		for f in $(CGLTF_HEADERS); do
			cp "$$tmpdir/$$f" "$$dest/$$f"
			echo "Updated $$dest/$$f"
		done
	done
	for f in $(CGLTF_HEADERS); do
		grep -m1 'Version:' "$$tmpdir/$$f" || head -n 5 "$$tmpdir/$$f"
	done
