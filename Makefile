WINE := $(CURDIR)/Wine Staging.app/Contents/Resources/wine/bin/wine
WINEPREFIX := $(CURDIR)/wineprefix

WINEDLLOVERRIDES := dxsetup.exe=d

export WINEPREFIX
export WINEDLLOVERRIDES

WINESERVER := $(CURDIR)/Wine Staging.app/Contents/Resources/wine/bin/wineserver

run-steam:
	"$(WINE)" "C:\Games\Steam\steam.exe"

clear:
	-"$(WINESERVER)" --kill
