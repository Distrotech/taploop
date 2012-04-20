#Copyright (C) 2012  Gregory Nietsky <gregory@distrotetch.co.za> 
#        http://www.distrotech.co.za
#
#This program is free software: you can redistribute it and/or modify
#it under the terms of the GNU General Public License as published by
#the Free Software Foundation, either version 3 of the License, or
#(at your option) any later version.
#
#This program is distributed in the hope that it will be useful,
#but WITHOUT ANY WARRANTY; without even the implied warranty of
#MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#GNU General Public License for more details.
#
#You should have received a copy of the GNU General Public License
#along with this program.  If not, see <http://www.gnu.org/licenses/>.

CFLAGS=-g -Wall -I./include

TL_OBJS = taploop.o refobj.o util.o lookup3.o thread.o vlan.o tlsock.o \
		clientserv.o packet.o
TLC_OBJS = tapclient.o

all: taploopd taploop

install: all
	echo "Put ME Where";

clean:
	rm -f taploop *.o core

taploop: $(TLC_OBJS)
	gcc -g -o $@ $^ -lpthread

taploopd: $(TL_OBJS)
	gcc -g -o $@ $^ -lpthread
