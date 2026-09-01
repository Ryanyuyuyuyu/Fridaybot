APP := build/FridayCompanion.app
EXECUTABLE := $(APP)/Contents/MacOS/friday-companion
PLIST := $(APP)/Contents/Info.plist

.PHONY: all clean run

all: $(EXECUTABLE) $(PLIST)

$(EXECUTABLE): friday_companion.m friday_overlay.m friday_overlay.h
	mkdir -p "$(APP)/Contents/MacOS"
	xcrun clang -fobjc-arc -fmodules -fmodules-cache-path=build/module-cache -O2 -Wall -Wextra -mmacosx-version-min=13.0 \
		-framework Foundation -framework AppKit -framework CoreBluetooth \
		-framework ApplicationServices friday_companion.m friday_overlay.m -o "$(EXECUTABLE)"

$(PLIST): Info.plist
	mkdir -p "$(APP)/Contents"
	cp Info.plist "$(PLIST)"

run: all
	"$(EXECUTABLE)"

clean:
	rm -rf build
