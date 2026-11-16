.PHONY: all build package clean

all: build

build:
	sh scripts/build.sh

package: build
	sh scripts/package.sh

clean:
	rm -rf out dist
