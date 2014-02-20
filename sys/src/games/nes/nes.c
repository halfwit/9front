#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <keyboard.h>
#include "dat.h"
#include "fns.h"

extern uchar ppuram[16384];
int nprg, nchr, map;
uchar *prg, *chr;
int scale;
Rectangle picr;
Image *tmp, *bg;
int clock, ppuclock, syncclock, syncfreq, checkclock, sleeps;
Mousectl *mc;
int keys;
extern void (*mapper[])(int, u8int);
int mirr;

void
loadrom(char *file)
{
	int fd;
	int nes20;
	static uchar header[16];
	static u32int flags;
	
	fd = open(file, OREAD);
	if(fd < 0)
		sysfatal("open: %r");
	if(readn(fd, header, sizeof(header)) < sizeof(header))
		sysfatal("read: %r");
	if(memcmp(header, "NES\x1a", 4) != 0)
		sysfatal("not a ROM");
	if(header[15] != 0)
		memset(header + 7, 0, 9);
	flags = header[6] | header[7] << 8;
	nes20 = (flags & FLNES20M) == FLNES20V;
	if(flags & (FLVS | FLPC10))
		sysfatal("ROM not supported");
	nprg = header[HPRG];
	if(nes20)
		nprg |= (header[HROMH] & 0xf) << 8;
	if(nprg == 0)
		sysfatal("invalid ROM");
	nchr = header[HCHR];
	if(nes20)
		nchr |= (header[HROMH] & 0xf0) << 4;
	map = (flags >> FLMAPPERL) & 0x0f | (((flags >> FLMAPPERH) & 0x0f) << 4);
	if(nes20)
		map |= (header[8] & 0x0f) << 8;
	if(map >= 256 || mapper[map] == nil)
		sysfatal("unimplemented mapper %d", map);

	memset(mem, 0, sizeof(mem));
	if((flags & FLTRAINER) != 0 && readn(fd, mem + 0x7000, 512) < 512)
			sysfatal("read: %r");
	prg = malloc(nprg * PRGSZ);
	if(prg == nil)
		sysfatal("malloc: %r");
	if(readn(fd, prg, nprg * PRGSZ) < nprg * PRGSZ)
		sysfatal("read: %r");
	if(nchr != 0){
		chr = malloc(nchr * CHRSZ);
		if(chr == nil)
			sysfatal("malloc: %r");
		if(readn(fd, chr, nchr * CHRSZ) < nchr * CHRSZ)
			sysfatal("read: %r");
	}else{
		nchr = 16;
		chr = malloc(16 * CHRSZ);
		if(chr == nil)
			sysfatal("malloc: %r");
	}
	if((flags & FLFOUR) != 0)
		mirr = MFOUR;
	else if((flags & FLMIRROR) != 0)
		mirr = MVERT;
	else
		mirr = MHORZ;
	mapper[map](-1, 0);
}

extern int trace;

void
keyproc(void *)
{
	int fd;
	char buf[256], *s;
	Rune r;

	fd = open("/dev/kbd", OREAD);
	if(fd < 0)
		sysfatal("open: %r");
	for(;;){
		if(read(fd, buf, 256) <= 0)
			sysfatal("read /dev/kbd: %r");
		if(buf[0] == 'c'){
			if(utfrune(buf, Kdel))
				threadexitsall(nil);
			if(utfrune(buf, 't'))
				trace ^= 1;
		}
		if(buf[0] != 'k' && buf[0] != 'K')
			continue;
		s = buf + 1;
		keys = 0;
		while(*s != 0){
			s += chartorune(&r, s);
			switch(r){
			case Kdel: threadexitsall(nil);
			case 'x': keys |= 1<<0; break;
			case 'z': keys |= 1<<1; break;
			case Kshift: keys |= 1<<2; break;
			case 10: keys |= 1<<3; break;
			case Kup: keys |= 1<<4; break;
			case Kdown: keys |= 1<<5; break;
			case Kleft: keys |= 1<<6; break;
			case Kright: keys |= 1<<7; break;
			}
		}	
	}
}

void
threadmain(int argc, char **argv)
{
	int t;
	Point p;
	uvlong old, new, diff;

	scale = 1;
	ARGBEGIN {
	case '2':
		scale = 2;
		break;
	case '3':
		scale = 3;
		break;
	} ARGEND;

	if(argc < 1)
		sysfatal("missing argument");
	loadrom(argv[0]);
	if(initdraw(nil, nil, nil) < 0)
		sysfatal("initdraw: %r");
	mc = initmouse(nil, screen);
	if(mc == nil)
		sysfatal("initmouse: %r");
	proccreate(keyproc, nil, 8192);
	originwindow(screen, Pt(0, 0), screen->r.min);
	p = divpt(addpt(screen->r.min, screen->r.max), 2);
	picr = (Rectangle){subpt(p, Pt(scale * 128, scale * 120)), addpt(p, Pt(scale * 128, scale * 120))};
	if(screen->chan != XRGB32)
		tmp = allocimage(display, Rect(0, 0, scale * 256, scale * 240), XRGB32, 0, 0);
	bg = allocimage(display, Rect(0, 0, 1, 1), screen->chan, 1, 0xCCCCCCFF);
	draw(screen, screen->r, bg, nil, ZP);
	
	pc = memread(0xFFFC) | memread(0xFFFD) << 8;
	rP = FLAGI;
	syncfreq = FREQ / 30;
	old = nsec();
	for(;;){
		t = step() * 12;
		clock += t;
		ppuclock += t;
		syncclock += t;
		checkclock += t;
		while(ppuclock >= 4){
			ppustep();
			ppuclock -= 4;
		}
		if(syncclock >= syncfreq){
			sleep(10);
			sleeps++;
			syncclock = 0;
		}
		if(checkclock >= FREQ){
			new = nsec();
			diff = new - old - sleeps * 10 * MILLION;
			diff = BILLION - diff;
			if(diff <= 0)
				syncfreq = FREQ;
			else
				syncfreq = ((vlong)FREQ) * 10 * MILLION / diff;
			if(syncfreq < FREQ / 100)
				syncfreq = FREQ / 100;
			old = new;
			checkclock = 0;
			sleeps = 0;
		}
	}
}