Ignore assets/ and README.md, they are meant to be public facing

dx9mt/ contains the all of the code and tests that create our actual d3d9.dll that the game interfaces with. As well, it also contains the code for the Metal Viewer (which displays our rendered frames).

dx9mt-output/ contains the dx9mt_runtime.log as well as frame dumps from gameplay for debugging. They are sorted into folders by session timestamp

wineprefix/ contains the wine prefix within which we run Fallout New Vegas. Refer to Makefile for more details

