NCURSES_PATH = /set/me/
all:
	gcc main.c -o main -I$(NCURSES_PATH)include/ -I$(NCURSES_PATH)include/ncurses/ -lmenu -lpthread -lncurses -L$(NCURSES_PATH)lib/
