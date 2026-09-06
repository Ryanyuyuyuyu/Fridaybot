APP := build/FridayCompanion.app
EXECUTABLE := $(APP)/Contents/MacOS/friday-companion
PLIST := $(APP)/Contents/Info.plist
OBJECTS := build/obj/friday_companion.o build/obj/friday_overlay.o

.PHONY: all clean run sign

all: sign

$(EXECUTABLE): $(OBJECTS)
	mkdir -p "$(APP)/Contents/MacOS"
	xcrun clang -mmacosx-version-min=13.0 \
		-framework Foundation -framework AppKit -framework CoreBluetooth \
		-framework ApplicationServices \
		$(OBJECTS) -o "$(EXECUTABLE)"

build/obj/%.o: %.m friday_overlay.h
	mkdir -p build/obj build/module-cache
	xcrun clang -c -fobjc-arc -fmodules -fmodules-cache-path=build/module-cache -O2 -Wall -Wextra \
		-mmacosx-version-min=13.0 $< -o $@

$(PLIST): Info.plist
	mkdir -p "$(APP)/Contents"
	cp Info.plist "$(PLIST)"

sign: $(EXECUTABLE) $(PLIST)
	xattr -cr "$(APP)"
	codesign --force --deep --sign - --identifier com.friday.companion "$(APP)"
	@xattr -d com.apple.FinderInfo "$(APP)" 2>/dev/null || true
	@xattr -d 'com.apple.fileprovider.fpfs#P' "$(APP)" 2>/dev/null || true

run: sign
	open -n "$(APP)"

clean:
	rm -rf build
