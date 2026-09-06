APP := build/CodexHostIdentity.app
EXECUTABLE := $(APP)/Contents/MacOS/codex-host-identity

.PHONY: all package sign test clean
all: package

$(EXECUTABLE): host_identity.m quota_reader.m quota_reader.h power_policy.c power_policy.h
	mkdir -p "$(APP)/Contents/MacOS" build/module-cache
	xcrun clang -fobjc-arc -fmodules -fmodules-cache-path=build/module-cache \
		-mmacosx-version-min=13.0 -O2 -Wall -Wextra -Werror \
		-framework Foundation -framework AppKit -framework CoreBluetooth -framework IOKit \
		-framework SystemConfiguration host_identity.m quota_reader.m power_policy.c -o "$@"

$(APP)/Contents/Info.plist: Info.plist
	mkdir -p "$(APP)/Contents"
	cp "$<" "$@"

package: $(EXECUTABLE) $(APP)/Contents/Info.plist scripts/package.sh
	/bin/sh scripts/package.sh "$(APP)" build/CodexHostIdentity.zip

# Signing happens only in an isolated non-iCloud copy; the deliverable is ZIP.
sign: package

build/quota-reader-test: tests/quota_reader_test.m quota_reader.m quota_reader.h
	mkdir -p build
	xcrun clang -fobjc-arc -O2 -Wall -Wextra -Werror -mmacosx-version-min=13.0 \
		-framework Foundation tests/quota_reader_test.m quota_reader.m -o "$@"

build/power-policy-test: tests/power_policy_test.c power_policy.c power_policy.h
	mkdir -p build
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror tests/power_policy_test.c power_policy.c -o "$@"

test: $(EXECUTABLE) build/quota-reader-test build/power-policy-test
	"$(EXECUTABLE)" --self-test
	"$(EXECUTABLE)" --help
	chmod +x tests/fake_codex.py
	build/quota-reader-test "$(CURDIR)/tests/fake_codex.py"
	build/power-policy-test

clean:
	rm -rf build
